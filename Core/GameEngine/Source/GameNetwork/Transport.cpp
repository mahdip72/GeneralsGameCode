/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////


#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/crc.h"
#include "GameNetwork/NetCommandValidation.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/Transport.h"

#if defined(_WIN64)
#include "Lib/PipelineExecutionPolicy.h"

static_assert(MAX_NETWORK_MESSAGE_LEN <= rts::network::NETWORK_IO_MAX_DATAGRAM,
	"Socket I/O datagram budget must cover the configured transport payload");
#endif

//--------------------------------------------------------------------------
// Packet-level encryption is an XOR operation, for speed reasons.  To get
// the max throughput, we only XOR whole 4-byte words, so the last bytes
// can be non-XOR'd.

// This assumes the buf is a multiple of 4 bytes.  Extra is not encrypted.
static inline void encryptBuf( unsigned char *buf, Int len )
{
	UnsignedInt mask = 0x0000Fade;

	UnsignedInt *uintPtr = (UnsignedInt *) (buf);

	for (int i=0 ; i<len/4 ; i++) {
		*uintPtr = (*uintPtr) ^ mask;
		*uintPtr = htonl(*uintPtr);
		uintPtr++;
		mask += 0x00000321; // just for fun
	}
}

// This assumes the buf is a multiple of 4 bytes.  Extra is not encrypted.
static inline void decryptBuf( unsigned char *buf, Int len )
{
	UnsignedInt mask = 0x0000Fade;

	UnsignedInt *uintPtr = (UnsignedInt *) (buf);

	for (int i=0 ; i<len/4 ; i++) {
		*uintPtr = htonl(*uintPtr);
		*uintPtr = (*uintPtr) ^ mask;
		uintPtr++;
		mask += 0x00000321; // just for fun
	}
}

//--------------------------------------------------------------------------

Transport::Transport()
{
	m_winsockInit = false;
	m_udpsock = nullptr;
	m_useLatency = false;
	m_usePacketLoss = false;
#if defined(_WIN64)
	m_ioOwner = nullptr;
	m_ioGeneration = 0;
	m_asyncSocketIO = rts::UseParallelPipelines();
	m_socketIOPolicyOverride = false;
	m_ioLastError = 0;
	m_ioFallbackCount = 0;
	memset(&m_stoppedIOMetrics, 0, sizeof(m_stoppedIOMetrics));
	memset(m_ioPending, 0, sizeof(m_ioPending));
#endif
}

Transport::~Transport()
{
	reset();
}

Bool Transport::init( AsciiString ip, UnsignedShort port )
{
	return init(ResolveIP(ip), port);
}

Bool Transport::init( UnsignedInt ip, UnsignedShort port )
{
	// A rebind is a session boundary, including callers that omit reset(). Join
	// the previous I/O owner before clearing buffers or reusing the endpoint.
	reset();
#if defined(_WIN64)
	// Resolve the default at initialization, after command-line policy selection,
	// and freeze it before creating any socket execution thread.
	rts::LockPipelineExecutionMode();
	if (!m_socketIOPolicyOverride) m_asyncSocketIO = rts::UseParallelPipelines();
	if (m_asyncSocketIO)
	{
		m_ioOwner = rts::network::NetworkIoOwner::Create();
		if (m_ioOwner && m_ioOwner->Start(ip, port))
		{
			m_ioGeneration = m_ioOwner->Generation();
		}
		else
		{
			// Start failure joins and closes before the legacy path can bind.
			if (m_ioOwner) m_stoppedIOMetrics = m_ioOwner->Metrics();
			rts::network::NetworkIoOwner::Destroy(m_ioOwner);
			m_ioOwner = nullptr;
			++m_ioFallbackCount;
			DEBUG_LOG(("Transport::init - Socket I/O owner unavailable; using serial socket I/O"));
		}
	}
	if (!m_ioOwner)
	{
#endif
	// ----- Initialize Winsock -----
	if (!m_winsockInit)
	{
		WORD verReq = MAKEWORD(2, 2);
		WSADATA wsadata;

		int err = WSAStartup(verReq, &wsadata);
		if (err != 0) {
			return false;
		}

		if ((LOBYTE(wsadata.wVersion) != 2) || (HIBYTE(wsadata.wVersion) !=2)) {
			WSACleanup();
			return false;
		}
		m_winsockInit = true;
	}

	// ------- Bind our port --------
	delete m_udpsock;
	m_udpsock = NEW UDP();

	if (!m_udpsock)
		return false;

	int retval = -1;
	time_t now = timeGetTime();
	while ((retval != 0) && ((timeGetTime() - now) < 1000)) {
		retval = m_udpsock->Bind(ip, port);
	}

	if (retval != 0) {
		DEBUG_CRASH(("Could not bind to 0x%8.8X:%d", ip, port));
		DEBUG_LOG(("Transport::init - Failure to bind socket with error code %x", retval));
		delete m_udpsock;
		m_udpsock = nullptr;
		return false;
	}
#if defined(_WIN64)
	}
	memset(m_ioPending, 0, sizeof(m_ioPending));
	m_ioLastError = 0;
#endif

	// ------- Clear buffers --------
	int i=0;
	for (; i<MAX_MESSAGES; ++i)
	{
		m_outBuffer[i].length = 0;
		m_inBuffer[i].length = 0;
#if defined(RTS_DEBUG)
		m_delayedInBuffer[i].message.length = 0;
#endif
	}
	for (i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		m_incomingBytes[i] = 0;
		m_outgoingBytes[i] = 0;
		m_unknownBytes[i] = 0;
		m_incomingPackets[i] = 0;
		m_outgoingPackets[i] = 0;
		m_unknownPackets[i] = 0;
	}
	m_statisticsSlot = 0;
	m_lastSecond = timeGetTime();

	m_port = port;

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_latencyAverage > 0 || TheGlobalData->m_latencyNoise)
		m_useLatency = true;

	if (TheGlobalData->m_packetLoss)
		m_usePacketLoss = true;
#endif

	return true;
}

void Transport::reset()
{
#if defined(_WIN64)
	if (m_ioOwner)
	{
		// Lobby leave/quit paths explicitly call doSend before reset. Attempt
		// accepted datagrams before closing, but never publish old ingress.
		if (!m_ioOwner->Stop(true))
			DEBUG_LOG(("Transport::reset - Socket I/O drain failed or cancelled queued sends"));
		m_stoppedIOMetrics = m_ioOwner->Metrics();
		rts::network::NetworkIoOwner::Destroy(m_ioOwner);
		m_ioOwner = nullptr;
	}
	m_ioGeneration = 0;
	memset(m_ioPending, 0, sizeof(m_ioPending));
#endif
	delete m_udpsock;
	m_udpsock = nullptr;

	if (m_winsockInit)
	{
		WSACleanup();
		m_winsockInit = false;
	}
	for (size_t i = 0; i < ARRAY_SIZE(m_inBuffer); ++i)
	{
		m_inBuffer[i].length = 0;
		m_outBuffer[i].length = 0;
#if defined(RTS_DEBUG)
		m_delayedInBuffer[i].message.length = 0;
#endif
	}
}

Bool Transport::hasSocket() const
{
#if defined(_WIN64)
	if (m_ioOwner) return true;
#endif
	return m_udpsock != nullptr;
}

Bool Transport::hasAddressError()
{
#if defined(_WIN64)
	if (m_ioOwner) return m_ioLastError == WSAEADDRNOTAVAIL;
#endif
	return m_udpsock && m_udpsock->GetStatus() == UDP::ADDRNOTAVAIL;
}

Bool Transport::allowBroadcasts(Bool enabled)
{
#if defined(_WIN64)
	if (m_ioOwner) return m_ioOwner->AllowBroadcasts(enabled);
#endif
	return m_udpsock && m_udpsock->AllowBroadcasts(enabled);
}

Int Transport::socketError() const
{
#if defined(_WIN64)
	if (m_ioOwner) return m_ioLastError;
#endif
	return WSAGetLastError();
}

Int Transport::readSocket(unsigned char *bytes, UnsignedInt capacity, sockaddr_in *from)
{
#if defined(_WIN64)
	if (m_ioOwner)
	{
		rts::network::NetworkIoDatagram datagram;
		const rts::network::NetworkIoOwner::Result result = m_ioOwner->Receive(m_ioGeneration, datagram);
		if (result == rts::network::NetworkIoOwner::EMPTY) return 0;
		if (result != rts::network::NetworkIoOwner::ACCEPTED)
		{
			m_ioLastError = m_ioOwner->LastError();
			return -1;
		}
		// Match recvfrom truncation behavior: reject an oversized datagram,
		// never publish a truncated prefix as a valid game message.
		if (datagram.length > capacity) { m_ioLastError = WSAEMSGSIZE; return -1; }
		memcpy(bytes, datagram.bytes, datagram.length);
		memset(from, 0, sizeof(*from));
		from->sin_family = AF_INET;
		from->sin_addr.s_addr = htonl(datagram.address);
		from->sin_port = htons(datagram.port);
		return static_cast<Int>(datagram.length);
	}
#endif
	return m_udpsock->Read(bytes, capacity, from);
}

#if defined(_WIN64)
rts::network::NetworkIoMetrics Transport::getSocketIOMetrics() const
{
	if (m_ioOwner) return m_ioOwner->Metrics();
	return m_stoppedIOMetrics;
}

Bool Transport::collectSocketSends()
{
	Bool result = true;
	rts::network::NetworkIoCompletion completion;
	while (m_ioOwner->PollSendCompletion(m_ioGeneration, completion) == rts::network::NetworkIoOwner::ACCEPTED)
	{
		if (completion.generation != m_ioGeneration || completion.token >= MAX_MESSAGES || !m_ioPending[completion.token])
			continue;
		m_ioPending[completion.token] = false;
		if (completion.sentBytes > 0)
		{
			++m_outgoingPackets[m_statisticsSlot];
			m_outgoingBytes[m_statisticsSlot] += completion.requestedBytes;
			m_outBuffer[completion.token].length = 0;
			if (completion.sentBytes != completion.requestedBytes)
				DEBUG_LOG(("Transport::doSend - wanted to send %u bytes, only sent %d bytes", completion.requestedBytes, completion.sentBytes));
		}
		else
		{
			// Keep owner-side bytes for the next submission; a socket failure
			// does not turn an accepted queue entry into a successful send.
			m_ioLastError = completion.error;
			result = false;
		}
	}
	return result;
}
#endif

Bool Transport::update()
{
	Bool retval = TRUE;
	if (doRecv() == FALSE && hasAddressError())
	{
		retval = FALSE;
	}
	DEBUG_ASSERTLOG(retval, ("WSA error is %s", GetWSAErrorString(socketError()).str()));
	if (doSend() == FALSE && hasAddressError())
	{
		retval = FALSE;
	}
	DEBUG_ASSERTLOG(retval, ("WSA error is %s", GetWSAErrorString(socketError()).str()));
	return retval;
}

Bool Transport::doSend() {
#if defined(_WIN64)
	m_ioLastError = 0;
#endif
	if (!hasSocket())
	{
		DEBUG_LOG(("Transport::doSend() - m_udpSock is null!"));
		return FALSE;
	}

	Bool retval = TRUE;

	// Statistics gathering
	UnsignedInt now = timeGetTime();
	if (m_lastSecond + 1000 < now)
	{
		m_lastSecond = now;
		m_statisticsSlot = (m_statisticsSlot + 1) % MAX_TRANSPORT_STATISTICS_SECONDS;
		m_outgoingPackets[m_statisticsSlot] = 0;
		m_outgoingBytes[m_statisticsSlot] = 0;
		m_incomingPackets[m_statisticsSlot] = 0;
		m_incomingBytes[m_statisticsSlot] = 0;
		m_unknownPackets[m_statisticsSlot] = 0;
		m_unknownBytes[m_statisticsSlot] = 0;
	}

#if defined(_WIN64)
	if (m_ioOwner && !collectSocketSends()) retval = FALSE;
#endif

	// Send all messages
	for (size_t i = 0; i < ARRAY_SIZE(m_outBuffer); ++i)
	{
		if (m_outBuffer[i].length > 0)
		{
			int bytesSent = 0;
			// TheSuperHackers @info The handling of data sizing of the payload within a UDP packet is confusing due to the current networking implementation
			// The max game packet size needs to be smaller than max udp payload by sizeof(TransportMessageHeader)
			// But the max network message size needs to include the bytes of the transport message header and equal the max udp payload
			// Therefore, transmitted data needs to add the extra bytes of the network header to the payloads length
			int bytesToSend = m_outBuffer[i].length + sizeof(TransportMessageHeader);
#if defined(_WIN64)
			if (m_ioOwner)
			{
				if (m_ioPending[i]) continue;
				const rts::network::NetworkIoOwner::Result submitted = m_ioOwner->Submit(m_ioGeneration,
					static_cast<unsigned int>(i), m_outBuffer[i].addr, m_outBuffer[i].port,
					reinterpret_cast<const unsigned char *>(&m_outBuffer[i]), bytesToSend);
				if (submitted == rts::network::NetworkIoOwner::ACCEPTED) m_ioPending[i] = true;
				else { retval = FALSE; break; } // Preserve FIFO and keep unsent owner bytes.
				continue;
			}
#endif
			// Send this message
			if ((bytesSent = m_udpsock->Write((unsigned char *)(&m_outBuffer[i]), bytesToSend, m_outBuffer[i].addr, m_outBuffer[i].port)) > 0)
			{
				//DEBUG_LOG(("Sending %d bytes to %d.%d.%d.%d:%d", bytesToSend, PRINTF_IP_AS_4_INTS(m_outBuffer[i].addr), m_outBuffer[i].port));
				m_outgoingPackets[m_statisticsSlot]++;
				m_outgoingBytes[m_statisticsSlot] += m_outBuffer[i].length + sizeof(TransportMessageHeader);
				m_outBuffer[i].length = 0;  // Remove from queue
				if (bytesSent != bytesToSend)
				{
					DEBUG_LOG(("Transport::doSend - wanted to send %d bytes, only sent %d bytes to %d.%d.%d.%d:%d",
						bytesToSend, bytesSent,
						PRINTF_IP_AS_4_INTS(m_outBuffer[i].addr), m_outBuffer[i].port));
				}
			}
			else
			{
				//DEBUG_LOG(("Could not write to socket!!!  Not discarding message!"));
				retval = FALSE;
				//DEBUG_LOG(("Transport::doSend returning FALSE"));
			}
		}
	}

#if defined(RTS_DEBUG)
	// Latency simulation - deliver anything we're holding on to that is ready
	if (m_useLatency)
	{
		size_t bufferIndex = 0;

		for (size_t i = 0; i < ARRAY_SIZE(m_delayedInBuffer); ++i)
		{
			if (m_delayedInBuffer[i].message.length > 0 && m_delayedInBuffer[i].deliveryTime <= now)
			{
				for (; bufferIndex < ARRAY_SIZE(m_inBuffer); ++bufferIndex)
				{
					if (m_inBuffer[bufferIndex].length <= 0)
					{
						// Empty slot; use it
						memcpy(&m_inBuffer[bufferIndex], &m_delayedInBuffer[i].message, sizeof(TransportMessage));
						m_delayedInBuffer[i].message.length = 0;
						++bufferIndex;
						break;
					}
				}
			}
		}
	}
#endif
	return retval;
}

Bool Transport::doRecv()
{
#if defined(_WIN64)
	m_ioLastError = 0;
#endif
	if (!hasSocket())
	{
		DEBUG_LOG(("Transport::doRecv() - m_udpSock is null!"));
		return FALSE;
	}

	Bool retval = TRUE;

	// Read in anything on our socket
	sockaddr_in from;
#if defined(RTS_DEBUG)
	UnsignedInt now = timeGetTime();
#endif
	// TheSuperHackers @info The handling of data sizing of the payload within a UDP packet is confusing due to the current networking implementation
	// The max game packet size needs to be smaller than max udp payload by sizeof(TransportMessageHeader)
	// But the max network message size needs to include the bytes of the transport message header and equal the max udp payload
	// Therefore, when receiving data we use the max udp payload size to receive the game packet payload and network header
	TransportMessage incomingMessage;
	unsigned char *buf = (unsigned char *)&incomingMessage;
	int len = MAX_NETWORK_MESSAGE_LEN;
	size_t bufferIndex = 0;
	size_t bufferCapacity = ARRAY_SIZE(m_inBuffer);
#if defined(RTS_DEBUG)
	if (m_useLatency)
	{
		bufferCapacity = ARRAY_SIZE(m_delayedInBuffer);
		while (bufferIndex < bufferCapacity && m_delayedInBuffer[bufferIndex].message.length > 0)
			++bufferIndex;
	}
	else
#endif
	{
		while (bufferIndex < bufferCapacity && m_inBuffer[bufferIndex].length > 0)
			++bufferIndex;
	}

	UnsignedInt processedMessages = 0;
//	DEBUG_LOG(("Transport::doRecv - checking"));
	while (ShouldReceiveNetworkMessage(processedMessages, bufferIndex < bufferCapacity) &&
		(len=readSocket(buf, MAX_NETWORK_MESSAGE_LEN, &from)) > 0)
	{
		++processedMessages;
#if defined(RTS_DEBUG)
		// Packet loss simulation
		if (m_usePacketLoss)
		{
			if ( TheGlobalData->m_packetLoss >= GameClientRandomValue(0, 100) )
			{
				continue;
			}
		}
#endif

//		DEBUG_LOG(("Transport::doRecv - Got something! len = %d", len));
		// Decrypt the packet
//		DEBUG_LOG_RAW(("buffer = "));
//		for (Int munkee = 0; munkee < len; ++munkee) {
//			DEBUG_LOG_RAW(("%02x", *(buf + munkee)));
//		}
//		DEBUG_LOG_RAW(("\n"));
		decryptBuf(buf, len);

		incomingMessage.length = len - sizeof(TransportMessageHeader);

		if (len <= sizeof(TransportMessageHeader) || !isGeneralsPacket( &incomingMessage ))
		{
			DEBUG_LOG(("Transport::doRecv - unknownPacket! len = %d", len));
			m_unknownPackets[m_statisticsSlot]++;
			m_unknownBytes[m_statisticsSlot] += len;
			continue;
		}

		// Something there; stick it somewhere
//		DEBUG_LOG(("Saw %d bytes from %d:%d", len, ntohl(from.sin_addr.S_un.S_addr), ntohs(from.sin_port)));
		m_incomingPackets[m_statisticsSlot]++;
		m_incomingBytes[m_statisticsSlot] += len;

		DEBUG_ASSERTCRASH(bufferIndex < MAX_MESSAGES, ("Message lost!"));

#if defined(RTS_DEBUG)
		// Latency simulation
		if (m_useLatency)
		{
			for (; bufferIndex < ARRAY_SIZE(m_delayedInBuffer); ++bufferIndex)
			{
				if (m_delayedInBuffer[bufferIndex].message.length <= 0)
				{
					// Empty slot; use it
					m_delayedInBuffer[bufferIndex].deliveryTime =
						now + TheGlobalData->m_latencyAverage +
						(Int)(TheGlobalData->m_latencyAmplitude * sin(now * TheGlobalData->m_latencyPeriod)) +
						GameClientRandomValue(-TheGlobalData->m_latencyNoise, TheGlobalData->m_latencyNoise);
					m_delayedInBuffer[bufferIndex].message.length = incomingMessage.length;
					m_delayedInBuffer[bufferIndex].message.addr = ntohl(from.sin_addr.S_un.S_addr);
					m_delayedInBuffer[bufferIndex].message.port = ntohs(from.sin_port);
					memcpy(&m_delayedInBuffer[bufferIndex].message, buf, len);
					++bufferIndex;
					while (bufferIndex < bufferCapacity && m_delayedInBuffer[bufferIndex].message.length > 0)
						++bufferIndex;
					break;
				}
			}

			continue;
		}
#endif

		for (; bufferIndex < ARRAY_SIZE(m_inBuffer); ++bufferIndex)
		{
			if (m_inBuffer[bufferIndex].length <= 0)
			{
				// Empty slot; use it
				m_inBuffer[bufferIndex].length = incomingMessage.length;
				m_inBuffer[bufferIndex].addr = ntohl(from.sin_addr.S_un.S_addr);
				m_inBuffer[bufferIndex].port = ntohs(from.sin_port);
				memcpy(&m_inBuffer[bufferIndex], buf, len);
				++bufferIndex;
				while (bufferIndex < bufferCapacity && m_inBuffer[bufferIndex].length > 0)
					++bufferIndex;
				break;
			}
		}
	}

	if (len == -1) {
		// there was a socket error trying to perform a read.
		//DEBUG_LOG(("Transport::doRecv returning FALSE"));
		retval = FALSE;
	}

	return retval;
}

Bool Transport::queueSend(UnsignedInt addr, UnsignedShort port, const UnsignedByte *buf, Int len /*,
						  NetMessageFlags flags, Int id */)
{
	if (len < 1 || len > MAX_PACKET_SIZE)
	{
		DEBUG_LOG(("Transport::queueSend - Invalid Packet size"));
		return false;
	}

	for (size_t i = 0; i < ARRAY_SIZE(m_outBuffer); ++i)
	{
		if (m_outBuffer[i].length <= 0)
		{
			// Insert data here
			m_outBuffer[i].length = len;
			memcpy(m_outBuffer[i].data, buf, len);
			m_outBuffer[i].addr = addr;
			m_outBuffer[i].port = port;
//			m_outBuffer[i].header.flags = flags;
//			m_outBuffer[i].header.id = id;
			m_outBuffer[i].header.magic = GENERALS_MAGIC_NUMBER;

			CRC crc;
			crc.computeCRC( (unsigned char *)(&(m_outBuffer[i].header.magic)), m_outBuffer[i].length + sizeof(TransportMessageHeader) - sizeof(UnsignedInt) );
//			DEBUG_LOG(("About to assign the CRC for the packet"));
			m_outBuffer[i].header.crc = crc.get();

			// Encrypt packet
//			DEBUG_LOG(("buffer: "));
			encryptBuf((unsigned char *)&m_outBuffer[i], len + sizeof(TransportMessageHeader));
//			DEBUG_LOG((""));

			return true;
		}
	}
	DEBUG_LOG(("Send Queue is getting full, dropping packets"));
	return false;
}

Bool Transport::isGeneralsPacket( TransportMessage *msg )
{
	if (!msg)
		return false;

	if (msg->length < 0 || msg->length > MAX_PACKET_SIZE)
		return false;

	CRC crc;
//	crc.computeCRC( (unsigned char *)msg->data, msg->length );
	crc.computeCRC( (unsigned char *)(&(msg->header.magic)), msg->length + sizeof(TransportMessageHeader) - sizeof(UnsignedInt) );

	if (crc.get() != msg->header.crc)
		return false;

	if (msg->header.magic != GENERALS_MAGIC_NUMBER)
		return false;

	return true;
}

// Statistics ---------------------------------------------------
Real Transport::getIncomingBytesPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_incomingBytes[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getIncomingPacketsPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_incomingPackets[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getOutgoingBytesPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_outgoingBytes[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getOutgoingPacketsPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_outgoingPackets[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getUnknownBytesPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_unknownBytes[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getUnknownPacketsPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_unknownPackets[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}



