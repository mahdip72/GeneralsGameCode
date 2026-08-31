#ifndef RTS_LIB_NETWORKIOOWNER_H
#define RTS_LIB_NETWORKIOOWNER_H

namespace rts
{
namespace network
{
// This is a socket boundary, not a game-packet boundary. Bytes are never parsed
// here and endpoint fields are host order. All storage is fixed and deep-copied.
enum { NETWORK_IO_MAX_DATAGRAM = 1100, NETWORK_IO_QUEUE_CAPACITY = 256, NETWORK_IO_MAX_SESSIONS = 16 };
typedef unsigned long long NetworkIoGeneration;

struct NetworkIoDatagram
{
	NetworkIoGeneration generation;
	unsigned int token;
	unsigned int address;
	unsigned short port;
	unsigned int length;
	unsigned char bytes[NETWORK_IO_MAX_DATAGRAM];
};

struct NetworkIoCompletion
{
	NetworkIoGeneration generation;
	unsigned int token;
	unsigned int requestedBytes;
	int sentBytes;
	int error;
};

struct NetworkIoMetrics
{
	unsigned long long submitted;
	unsigned long long sent;
	unsigned long long received;
	unsigned long long sendFailures;
	unsigned long long receiveFailures;
	unsigned long long sendSaturation;
	unsigned long long receiveSaturation;
	unsigned long long staleRejected;
	unsigned long long cancelledSends;
	unsigned long long discardedReceives;
	unsigned long long startFailures;
	unsigned long long drainTimeouts;
	unsigned long long ownerWaitMilliseconds;
	unsigned int sendHighWater;
	unsigned int receiveHighWater;
	unsigned int ioThreadId;
};

// Optional test backend. The caller owns its lifetime through Stop(). Every
// backend call, including Open and Close, executes exclusively on the I/O
// thread. It must use nonblocking I/O and must not access game objects/globals.
class NetworkIoSocket
{
public:
	virtual ~NetworkIoSocket() {}
	virtual int Open(unsigned int address, unsigned short port) = 0;
	virtual int Send(const NetworkIoDatagram &datagram, int &error) = 0;
	// Zero means no datagram (including a zero-byte UDP datagram); -1 is error.
	virtual int Receive(NetworkIoDatagram &datagram, int &error) = 0;
	virtual int AllowBroadcasts(bool enabled) = 0;
	virtual unsigned short LocalPort() const = 0;
	virtual void Close() = 0;
};

class NetworkIoOwner
{
public:
	enum Result { ACCEPTED, EMPTY, FULL, STALE, STOPPED, WRONG_THREAD, FAILED };

	// Each facade is a socket session on ONE process-wide I/O service (at most
	// NETWORK_IO_MAX_SESSIONS). No game allocator participates in its storage.
	static NetworkIoOwner *Create(NetworkIoSocket *testSocket = 0);
	static void Destroy(NetworkIoOwner *owner);
	bool Start(unsigned int address, unsigned short port);
	// Stop rejects new submissions immediately, joins all active callbacks and
	// closes the socket on its I/O owner. Drain attempts every accepted send once
	// (nonblocking), preserving final lobby/quit sends before reset. False reports
	// a failed/cancelled send, never a delivery guarantee. The deadline bounds
	// drain work; any current nonblocking backend call is quiesced before return.
	// Ingress never survives. Other sessions are not stopped or rebound.
	bool Stop(bool drainSends, unsigned int drainTimeoutMilliseconds = 1000);
	Result Submit(NetworkIoGeneration generation, unsigned int token,
		unsigned int address, unsigned short port,
		const unsigned char *bytes, unsigned int length);
	Result Receive(NetworkIoGeneration generation, NetworkIoDatagram &datagram);
	Result PollSendCompletion(NetworkIoGeneration generation, NetworkIoCompletion &completion);
	bool AllowBroadcasts(bool enabled);
	NetworkIoGeneration Generation() const;
	unsigned short LocalPort() const;
	int LastError() const;
	NetworkIoMetrics Metrics() const;

private:
	NetworkIoOwner();
	~NetworkIoOwner();
	NetworkIoOwner(const NetworkIoOwner &);
	NetworkIoOwner &operator=(const NetworkIoOwner &);
	struct Impl;
	Impl *m_impl;
};
}
}

#endif
