#include "Lib/NetworkIoOwner.h"
#include "Lib/JobSystem.h"

// Deliberately separate from the game PCH, UDP logger and game allocators.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <new>
#include <process.h>
#include <stdint.h>
#include <string.h>

namespace rts
{
namespace network
{
namespace
{
class NativeSocket : public NetworkIoSocket
{
public:
	NativeSocket() : m_socket(INVALID_SOCKET), m_started(false), m_port(0) {}
	int Open(unsigned int address, unsigned short port) override
	{
		WSADATA data;
		int error = WSAStartup(MAKEWORD(2, 2), &data);
		if (error) return error;
		m_started = true;
		if (LOBYTE(data.wVersion) != 2 || HIBYTE(data.wVersion) != 2)
			return WSAVERNOTSUPPORTED;
		{
			m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (m_socket == INVALID_SOCKET) return WSAGetLastError();
			sockaddr_in endpoint = {};
			endpoint.sin_family = AF_INET;
			endpoint.sin_addr.s_addr = htonl(address);
			endpoint.sin_port = htons(port);
			if (bind(m_socket, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint)) == 0)
			{
				unsigned long nonblocking = 1;
				if (ioctlsocket(m_socket, FIONBIO, &nonblocking) != 0) return WSAGetLastError();
				int size = sizeof(endpoint);
				if (getsockname(m_socket, reinterpret_cast<sockaddr *>(&endpoint), &size) != 0)
					return WSAGetLastError();
				m_port = ntohs(endpoint.sin_port);
				return 0;
			}
			error = WSAGetLastError();
			closesocket(m_socket);
			m_socket = INVALID_SOCKET;
		}
		return error;
	}
	int Send(const NetworkIoDatagram &datagram, int &error) override
	{
		if (!datagram.address || !datagram.port)
		{
			error = WSAEADDRNOTAVAIL;
			return -1;
		}
		sockaddr_in endpoint = {};
		endpoint.sin_family = AF_INET;
		endpoint.sin_addr.s_addr = htonl(datagram.address);
		endpoint.sin_port = htons(datagram.port);
		const int sent = sendto(m_socket, reinterpret_cast<const char *>(datagram.bytes),
			static_cast<int>(datagram.length), 0, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint));
		error = sent == SOCKET_ERROR ? WSAGetLastError() : 0;
		return sent;
	}
	int Receive(NetworkIoDatagram &datagram, int &error) override
	{
		sockaddr_in endpoint = {};
		int size = sizeof(endpoint);
		const int count = recvfrom(m_socket, reinterpret_cast<char *>(datagram.bytes),
			NETWORK_IO_MAX_DATAGRAM, 0, reinterpret_cast<sockaddr *>(&endpoint), &size);
		error = count == SOCKET_ERROR ? WSAGetLastError() : 0;
		if (error == WSAEWOULDBLOCK) { error = 0; return 0; }
		if (count > 0)
		{
			datagram.address = ntohl(endpoint.sin_addr.s_addr);
			datagram.port = ntohs(endpoint.sin_port);
			datagram.length = static_cast<unsigned int>(count);
		}
		return count;
	}
	int AllowBroadcasts(bool enabled) override
	{
		const BOOL value = enabled ? TRUE : FALSE;
		return setsockopt(m_socket, SOL_SOCKET, SO_BROADCAST,
			reinterpret_cast<const char *>(&value), sizeof(value)) == 0 ? 0 : WSAGetLastError();
	}
	unsigned short LocalPort() const override { return m_port; }
	void Close() override
	{
		if (m_socket != INVALID_SOCKET) closesocket(m_socket);
		m_socket = INVALID_SOCKET;
		m_port = 0;
		if (m_started) WSACleanup();
		m_started = false;
	}
private:
	SOCKET m_socket;
	bool m_started;
	unsigned short m_port;
};

struct Guard
{
	explicit Guard(CRITICAL_SECTION &lock) : m_lock(lock) { EnterCriticalSection(&m_lock); }
	~Guard() { LeaveCriticalSection(&m_lock); }
	CRITICAL_SECTION &m_lock;
};
}

struct NetworkIoOwner::Impl
{
	struct Service
	{
		CRITICAL_SECTION lock;
		HANDLE wake, ready, thread;
		Impl *sessions[NETWORK_IO_MAX_SESSIONS];
		DWORD producer;
		bool terminate, registered;

		Service() : wake(0), ready(0), thread(0), producer(0), terminate(false), registered(false)
		{
			memset(sessions, 0, sizeof(sessions));
			InitializeCriticalSection(&lock);
			wake = CreateEventW(0, FALSE, FALSE, 0);
			ready = CreateEventW(0, TRUE, FALSE, 0);
		}
		~Service()
		{
			if (wake) CloseHandle(wake);
			if (ready) CloseHandle(ready);
			DeleteCriticalSection(&lock);
		}
		static Service &Shared()
		{
			// Process-lifetime registry only; the thread exits when the last
			// session stops. Session storage is never a game allocation.
			static Service service;
			return service;
		}
		static unsigned __stdcall Run(void *context)
		{
			Service &service = *static_cast<Service *>(context);
			const bool registered = rts::JobSystem::instance().registerCurrentThread(rts::JOB_OWNER_NETWORK);
			{
				Guard guard(service.lock);
				service.registered = registered;
			}
			SetEvent(service.ready);
			if (!registered) return 0;
			for (;;)
			{
				Impl *batch[NETWORK_IO_MAX_SESSIONS];
				{
					Guard guard(service.lock);
					if (service.terminate) break;
					memcpy(batch, service.sessions, sizeof(batch));
				}
				for (unsigned int index = 0; index < NETWORK_IO_MAX_SESSIONS; ++index)
				{
					Impl *session = batch[index];
					if (session && session->Slice())
					{
						{
							Guard guard(service.lock);
							service.sessions[index] = 0;
						}
						// Last access: Stop can now release the session safely.
						SetEvent(session->stopped);
					}
				}
				WaitForSingleObject(service.wake, 2);
			}
			rts::JobSystem::instance().unregisterCurrentThread(rts::JOB_OWNER_NETWORK);
			return 0;
		}
		bool Attach(Impl *session)
		{
			// Construct runtime bookkeeping on the producer, not inside the
			// socket owner. Registration itself does not start compute workers.
			(void)rts::JobSystem::instance();
			unsigned int index = 0;
			{
				Guard guard(lock);
				if (!wake || !ready || (producer && producer != GetCurrentThreadId())) return false;
				for (; index < NETWORK_IO_MAX_SESSIONS && sessions[index]; ++index) {}
				if (index == NETWORK_IO_MAX_SESSIONS) return false;
				producer = GetCurrentThreadId();
				if (!thread)
				{
					terminate = registered = false;
					ResetEvent(ready);
					const uintptr_t threadHandle = _beginthreadex(0, 0, Run, this, 0, 0);
					if (!threadHandle) { producer = 0; return false; }
					thread = reinterpret_cast<HANDLE>(threadHandle);
				}
			}
			WaitForSingleObject(ready, INFINITE);
			bool ownerRegistered = false;
			{
				Guard guard(lock);
				ownerRegistered = registered;
			}
			if (!ownerRegistered) { StopIfIdle(); return false; }
			{
				Guard guard(lock);
				sessions[index] = session;
			}
			SetEvent(wake);
			return true;
		}
		void StopIfIdle()
		{
			{
				Guard guard(lock);
				for (unsigned int index = 0; index < NETWORK_IO_MAX_SESSIONS; ++index)
					if (sessions[index]) return;
				if (!thread) return;
				terminate = true;
			}
			SetEvent(wake);
			WaitForSingleObject(thread, INFINITE);
			CloseHandle(thread);
			Guard guard(lock);
			thread = 0;
			producer = 0;
		}
	};

	CRITICAL_SECTION lock;
	HANDLE ready, controlDone, stopped;
	DWORD producer;
	NativeSocket native;
	NetworkIoSocket *socket;
	NetworkIoGeneration generation;
	NetworkIoDatagram sends[NETWORK_IO_QUEUE_CAPACITY];
	NetworkIoDatagram receives[NETWORK_IO_QUEUE_CAPACITY];
	NetworkIoCompletion completions[NETWORK_IO_QUEUE_CAPACITY];
	unsigned int sendHead, sendCount, receiveHead, receiveCount, completionHead, completionCount, outstanding;
	unsigned int address, drainTimeout;
	unsigned short port, localPort;
	bool attached, opening, accepting, stopping, drain, startOk, controlPending, broadcastValue, controlOk, drainOk;
	DWORD openBegin, stopBegin;
	int lastError, receiveError;
	NetworkIoMetrics metrics;

	Impl(NetworkIoSocket *backend) : ready(0), controlDone(0), stopped(0),
		producer(GetCurrentThreadId()), socket(backend ? backend : &native), generation(0),
		sendHead(0), sendCount(0), receiveHead(0), receiveCount(0), completionHead(0), completionCount(0), outstanding(0),
		address(0), drainTimeout(1000), port(0), localPort(0), attached(false), opening(false),
		accepting(false), stopping(false), drain(false), startOk(false),
		controlPending(false), broadcastValue(false), controlOk(false), drainOk(true),
		openBegin(0), stopBegin(0), lastError(0), receiveError(0)
	{
		memset(&metrics, 0, sizeof(metrics));
		InitializeCriticalSection(&lock);
		ready = CreateEventW(0, TRUE, FALSE, 0);
		controlDone = CreateEventW(0, TRUE, FALSE, 0);
		stopped = CreateEventW(0, TRUE, FALSE, 0);
	}
	~Impl()
	{
		if (ready) CloseHandle(ready);
		if (controlDone) CloseHandle(controlDone);
		if (stopped) CloseHandle(stopped);
		DeleteCriticalSection(&lock);
	}
	bool IsProducer() const { return producer == GetCurrentThreadId(); }
	void Wake() { SetEvent(Service::Shared().wake); }
	bool DrainExpired()
	{
		return stopping && drain && sendCount && GetTickCount() - stopBegin >= drainTimeout;
	}
	// One fair, bounded nonblocking slice. Only the shared owner calls it.
	// true means the socket is closed and this session can be removed.
	bool Slice()
	{
		if (opening)
		{
			int error = 0;
			unsigned short openedPort = 0;
			try
			{
				error = socket->Open(address, port);
				if (!error) openedPort = socket->LocalPort();
			}
			catch (...) { error = WSAEFAULT; }
			if (error)
			{
				try { socket->Close(); } catch (...) {}
				Guard guard(lock);
				lastError = error;
				// Retry address contention for the historical one-second bind
				// window without blocking the other live socket sessions.
				if (error == WSAEADDRINUSE && GetTickCount() - openBegin < 1000) return false;
				opening = false;
				++metrics.startFailures;
				SetEvent(ready);
				return true;
			}
			{
				Guard guard(lock);
				metrics.ioThreadId = GetCurrentThreadId();
				opening = false;
				startOk = accepting = true;
				localPort = openedPort;
			}
			SetEvent(ready);
		}

		bool exitNow = false, doControl = false, controlValue = false;
		{
			Guard guard(lock);
			if (DrainExpired())
			{
				++metrics.drainTimeouts;
				lastError = WSAETIMEDOUT;
				drain = false;
			}
			exitNow = stopping && (!drain || sendCount == 0);
			if (controlPending) { doControl = true; controlValue = broadcastValue; }
		}
		if (!exitNow && doControl)
		{
			int error = 0;
			try { error = socket->AllowBroadcasts(controlValue); }
			catch (...) { error = WSAEFAULT; }
			Guard guard(lock);
			controlOk = error == 0;
			if (error) lastError = error;
			controlPending = false;
			SetEvent(controlDone);
		}
		for (unsigned int index = 0; !exitNow && index < 32; ++index)
		{
			NetworkIoDatagram datagram;
			{
				Guard guard(lock);
				if (!sendCount || (stopping && !drain) || DrainExpired()) break;
				datagram = sends[sendHead];
				sendHead = (sendHead + 1) % NETWORK_IO_QUEUE_CAPACITY;
				--sendCount;
			}
			int error = 0, sent = -1;
			try { sent = socket->Send(datagram, error); }
			catch (...) { error = WSAEFAULT; }
			Guard guard(lock);
			NetworkIoCompletion &completion = completions[(completionHead + completionCount) % NETWORK_IO_QUEUE_CAPACITY];
			completion.generation = datagram.generation;
			completion.token = datagram.token;
			completion.requestedBytes = datagram.length;
			completion.sentBytes = sent;
			completion.error = error;
			++completionCount;
			if (sent > 0) ++metrics.sent;
			else
			{
				++metrics.sendFailures;
				if (stopping) drainOk = false;
				lastError = error;
			}
		}
		for (unsigned int index = 0; !exitNow && index < 32; ++index)
		{
			{
				Guard guard(lock);
				if (stopping) break;
				if (receiveCount == NETWORK_IO_QUEUE_CAPACITY) { ++metrics.receiveSaturation; break; }
			}
			NetworkIoDatagram datagram = {};
			int error = 0, count = -1;
			try { count = socket->Receive(datagram, error); }
			catch (...) { error = WSAEFAULT; }
			Guard guard(lock);
			if (count <= 0)
			{
				if (count < 0) { receiveError = error; lastError = error; ++metrics.receiveFailures; }
				break;
			}
			if (stopping) { ++metrics.discardedReceives; break; }
			if (static_cast<unsigned int>(count) > NETWORK_IO_MAX_DATAGRAM)
			{
				receiveError = WSAEMSGSIZE; lastError = WSAEMSGSIZE; ++metrics.receiveFailures;
				break;
			}
			datagram.length = static_cast<unsigned int>(count);
			datagram.generation = generation;
			receives[(receiveHead + receiveCount) % NETWORK_IO_QUEUE_CAPACITY] = datagram;
			++receiveCount;
			++metrics.received;
			if (receiveCount > metrics.receiveHighWater) metrics.receiveHighWater = receiveCount;
		}
		if (!exitNow) return false;

		try { socket->Close(); } catch (...) { Guard guard(lock); drainOk = false; lastError = WSAEFAULT; }
		{
			Guard guard(lock);
			accepting = false;
			metrics.cancelledSends += sendCount;
			if (sendCount) drainOk = false;
			metrics.discardedReceives += receiveCount;
			sendCount = receiveCount = 0;
			controlPending = false;
		}
		SetEvent(controlDone);
		return true;
	}
};

NetworkIoOwner::NetworkIoOwner() : m_impl(0) {}
NetworkIoOwner::~NetworkIoOwner() {}

NetworkIoOwner *NetworkIoOwner::Create(NetworkIoSocket *testSocket)
{
	void *ownerStorage = HeapAlloc(GetProcessHeap(), 0, sizeof(NetworkIoOwner));
	void *implStorage = HeapAlloc(GetProcessHeap(), 0, sizeof(Impl));
	if (!ownerStorage || !implStorage)
	{
		if (ownerStorage) HeapFree(GetProcessHeap(), 0, ownerStorage);
		if (implStorage) HeapFree(GetProcessHeap(), 0, implStorage);
		return 0;
	}
	NetworkIoOwner *owner = new (ownerStorage) NetworkIoOwner;
	owner->m_impl = new (implStorage) Impl(testSocket);
	if (!owner->m_impl->ready || !owner->m_impl->controlDone || !owner->m_impl->stopped)
	{
		Destroy(owner);
		return 0;
	}
	return owner;
}

void NetworkIoOwner::Destroy(NetworkIoOwner *owner)
{
	if (!owner) return;
	// Never free storage while its registered producer/service still owns it.
	if (!owner->m_impl->IsProducer()) return;
	owner->Stop(false);
	owner->m_impl->~Impl();
	HeapFree(GetProcessHeap(), 0, owner->m_impl);
	owner->~NetworkIoOwner();
	HeapFree(GetProcessHeap(), 0, owner);
}

bool NetworkIoOwner::Start(unsigned int address, unsigned short port)
{
	Impl &state = *m_impl;
	if (!state.IsProducer()) return false;
	Stop(true);
	{
		Guard guard(state.lock);
		if (++state.generation == 0) ++state.generation;
		state.address = address;
		state.port = port;
		state.localPort = 0;
		state.sendHead = state.sendCount = state.receiveHead = state.receiveCount = 0;
		state.completionHead = state.completionCount = state.outstanding = 0;
		state.stopping = state.accepting = state.startOk = state.controlPending = false;
		state.opening = state.drainOk = true;
		state.lastError = state.receiveError = 0;
		state.openBegin = GetTickCount();
		ResetEvent(state.ready);
		ResetEvent(state.stopped);
	}
	const DWORD begin = GetTickCount();
	state.attached = Impl::Service::Shared().Attach(&state);
	if (!state.attached)
	{
		Guard guard(state.lock);
		state.lastError = WSAENOBUFS;
		++state.metrics.startFailures;
		return false;
	}
	WaitForSingleObject(state.ready, INFINITE);
	{
		Guard guard(state.lock);
		state.metrics.ownerWaitMilliseconds += GetTickCount() - begin;
		if (state.startOk) return true;
	}
	Stop(false);
	return false;
}

bool NetworkIoOwner::Stop(bool drainSends, unsigned int drainTimeoutMilliseconds)
{
	Impl &state = *m_impl;
	if (!state.IsProducer()) return false;
	if (!state.attached) return true;
	{
		Guard guard(state.lock);
		state.accepting = false;
		state.stopping = true;
		state.drain = drainSends;
		state.stopBegin = GetTickCount();
		state.drainTimeout = drainTimeoutMilliseconds;
		for (unsigned int index = 0; index < state.completionCount; ++index)
			if (state.completions[(state.completionHead + index) % NETWORK_IO_QUEUE_CAPACITY].sentBytes <= 0)
				state.drainOk = false;
	}
	state.Wake();
	const DWORD begin = GetTickCount();
	// The service applies the drain deadline between nonblocking calls. It must
	// still acknowledge Close before any session memory can be released.
	WaitForSingleObject(state.stopped, INFINITE);
	Impl::Service::Shared().StopIfIdle();
	Guard guard(state.lock);
	state.attached = false;
	state.metrics.ownerWaitMilliseconds += GetTickCount() - begin;
	state.completionCount = state.outstanding = 0;
	return state.drainOk;
}

NetworkIoOwner::Result NetworkIoOwner::Submit(NetworkIoGeneration generation, unsigned int token,
	unsigned int address, unsigned short port, const unsigned char *bytes, unsigned int length)
{
	Impl &state = *m_impl;
	if (!state.IsProducer()) return WRONG_THREAD;
	Guard guard(state.lock);
	if (generation != state.generation) { ++state.metrics.staleRejected; return STALE; }
	if (!state.accepting) return STOPPED;
	if (!bytes || !length || length > NETWORK_IO_MAX_DATAGRAM) return FAILED;
	// Reserve completion capacity at submission, not when a socket call finishes.
	if (state.outstanding == NETWORK_IO_QUEUE_CAPACITY) { ++state.metrics.sendSaturation; return FULL; }
	NetworkIoDatagram &datagram = state.sends[(state.sendHead + state.sendCount) % NETWORK_IO_QUEUE_CAPACITY];
	datagram.generation = generation;
	datagram.token = token;
	datagram.address = address;
	datagram.port = port;
	datagram.length = length;
	memcpy(datagram.bytes, bytes, length);
	++state.sendCount;
	++state.outstanding;
	++state.metrics.submitted;
	if (state.outstanding > state.metrics.sendHighWater) state.metrics.sendHighWater = state.outstanding;
	state.Wake();
	return ACCEPTED;
}

NetworkIoOwner::Result NetworkIoOwner::Receive(NetworkIoGeneration generation, NetworkIoDatagram &datagram)
{
	Impl &state = *m_impl;
	if (!state.IsProducer()) return WRONG_THREAD;
	Guard guard(state.lock);
	if (generation != state.generation) { ++state.metrics.staleRejected; return STALE; }
	if (!state.accepting) return STOPPED;
	if (state.receiveCount)
	{
		datagram = state.receives[state.receiveHead];
		state.receiveHead = (state.receiveHead + 1) % NETWORK_IO_QUEUE_CAPACITY;
		--state.receiveCount;
		state.Wake();
		return ACCEPTED;
	}
	if (state.receiveError) { state.lastError = state.receiveError; state.receiveError = 0; return FAILED; }
	return EMPTY;
}

NetworkIoOwner::Result NetworkIoOwner::PollSendCompletion(NetworkIoGeneration generation, NetworkIoCompletion &completion)
{
	Impl &state = *m_impl;
	if (!state.IsProducer()) return WRONG_THREAD;
	Guard guard(state.lock);
	if (generation != state.generation) { ++state.metrics.staleRejected; return STALE; }
	if (!state.accepting) return STOPPED;
	if (!state.completionCount) return EMPTY;
	completion = state.completions[state.completionHead];
	state.completionHead = (state.completionHead + 1) % NETWORK_IO_QUEUE_CAPACITY;
	--state.completionCount;
	--state.outstanding;
	return ACCEPTED;
}

bool NetworkIoOwner::AllowBroadcasts(bool enabled)
{
	Impl &state = *m_impl;
	if (!state.IsProducer()) return false;
	{
		Guard guard(state.lock);
		if (!state.accepting) return false;
		state.broadcastValue = enabled;
		state.controlPending = true;
		state.controlOk = false;
		ResetEvent(state.controlDone);
	}
	state.Wake();
	const DWORD begin = GetTickCount();
	WaitForSingleObject(state.controlDone, INFINITE);
	Guard guard(state.lock);
	state.metrics.ownerWaitMilliseconds += GetTickCount() - begin;
	return state.controlOk;
}

NetworkIoGeneration NetworkIoOwner::Generation() const { Guard guard(m_impl->lock); return m_impl->generation; }
unsigned short NetworkIoOwner::LocalPort() const { Guard guard(m_impl->lock); return m_impl->localPort; }
int NetworkIoOwner::LastError() const { Guard guard(m_impl->lock); return m_impl->lastError; }
NetworkIoMetrics NetworkIoOwner::Metrics() const { Guard guard(m_impl->lock); return m_impl->metrics; }
}
}
