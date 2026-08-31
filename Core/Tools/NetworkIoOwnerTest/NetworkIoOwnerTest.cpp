#include "Lib/NetworkIoOwner.h"
#include "Lib/JobSystem.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

using namespace rts::network;

namespace
{
int failures = 0;
void Check(bool value, const char *message)
{
	if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

template <class Predicate> bool WaitUntil(Predicate predicate)
{
	const DWORD begin = GetTickCount();
	do { if (predicate()) return true; Sleep(1); } while (GetTickCount() - begin < 3000);
	return false;
}

class FakeSocket : public NetworkIoSocket
{
public:
	std::atomic<unsigned int> ownerThread{0}, calls{0}, opens{0}, closes{0}, ownershipFailures{0};
	std::atomic<int> openError{0}, sendError{0}, receiveError{0}, broadcastError{0};
	std::atomic<bool> throwSend{false}, broadcast{false};
	HANDLE sendEntered = CreateEventW(0, TRUE, FALSE, 0);
	HANDLE sendRelease = CreateEventW(0, TRUE, TRUE, 0);
	std::mutex mutex;
	std::deque<NetworkIoDatagram> incoming;
	std::vector<NetworkIoDatagram> sent;
	unsigned short boundPort = 0;

	~FakeSocket() override { CloseHandle(sendEntered); CloseHandle(sendRelease); }
	void Record()
	{
		const unsigned int current = GetCurrentThreadId();
		unsigned int empty = 0;
		ownerThread.compare_exchange_strong(empty, current);
		if (ownerThread != current || !rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_NETWORK))
			++ownershipFailures;
		++calls;
	}
	int Open(unsigned int, unsigned short port) override
	{
		Record(); ++opens; boundPort = port ? port : 43210;
		return openError;
	}
	int Send(const NetworkIoDatagram &datagram, int &error) override
	{
		Record(); SetEvent(sendEntered);
		// Fault-injection barrier used only to prove shutdown quiesces an active
		// callback. The production backend is always nonblocking.
		WaitForSingleObject(sendRelease, INFINITE);
		if (throwSend.exchange(false)) throw 1;
		error = sendError.exchange(0);
		if (error) return -1;
		std::lock_guard<std::mutex> guard(mutex);
		sent.push_back(datagram);
		return static_cast<int>(datagram.length);
	}
	int Receive(NetworkIoDatagram &datagram, int &error) override
	{
		Record(); error = receiveError.exchange(0);
		if (error) return -1;
		std::lock_guard<std::mutex> guard(mutex);
		if (incoming.empty()) return 0;
		datagram = incoming.front(); incoming.pop_front();
		return static_cast<int>(datagram.length);
	}
	int AllowBroadcasts(bool enabled) override { Record(); broadcast = enabled; return broadcastError; }
	unsigned short LocalPort() const override { return boundPort; }
	void Close() override
	{
		Record(); ++closes;
		std::lock_guard<std::mutex> guard(mutex);
		incoming.clear();
	}
	void Inject(unsigned int value, unsigned int address = 0x0a010203U, unsigned short port = 32001)
	{
		NetworkIoDatagram datagram = {};
		datagram.address = address; datagram.port = port; datagram.length = sizeof(value);
		std::memcpy(datagram.bytes, &value, sizeof(value));
		std::lock_guard<std::mutex> guard(mutex);
		incoming.push_back(datagram);
	}
	std::vector<NetworkIoDatagram> Sent()
	{
		std::lock_guard<std::mutex> guard(mutex);
		return sent;
	}
};

struct Session
{
	explicit Session(NetworkIoSocket *socket = 0) : owner(NetworkIoOwner::Create(socket))
	{ Check(owner != 0, "allocate bounded socket session"); }
	~Session() { NetworkIoOwner::Destroy(owner); }
	NetworkIoOwner *owner;
};

bool SubmitValue(NetworkIoOwner *owner, unsigned int value, unsigned int address = 0x7f000001U, unsigned short port = 12345)
{
	return owner->Submit(owner->Generation(), value, address, port,
		reinterpret_cast<const unsigned char *>(&value), sizeof(value)) == NetworkIoOwner::ACCEPTED;
}

void TestCopiesFifoAndQueuePressure()
{
	FakeSocket socket;
	Session session(&socket);
	if (!session.owner || !session.owner->Start(0, 12345)) { Check(false, "start fake socket"); return; }
	Check(session.owner->AllowBroadcasts(true) && socket.broadcast, "broadcast option runs on socket owner");
	const auto generation = session.owner->Generation();
	for (unsigned int index = 0; index < NETWORK_IO_QUEUE_CAPACITY; ++index)
	{
		unsigned int value = index;
		Check(session.owner->Submit(generation, index, 0x0a000001U + index, 31000 + index,
			reinterpret_cast<const unsigned char *>(&value), sizeof(value)) == NetworkIoOwner::ACCEPTED,
			"reserve bounded outgoing and completion slots");
		value = 0xffffffffU; // A queued command must not retain producer bytes.
	}
	unsigned char byte = 0;
	Check(session.owner->Submit(generation, 999, 1, 1, &byte, 1) == NetworkIoOwner::FULL,
		"unpublished completions still count against the bounded byte budget");
	Check(WaitUntil([&] { return session.owner->Metrics().sent == NETWORK_IO_QUEUE_CAPACITY; }),
		"service sends while producer does not poll or run compute jobs");
	const auto sent = socket.Sent();
	Check(sent.size() == NETWORK_IO_QUEUE_CAPACITY, "all accepted datagrams attempted once");
	for (unsigned int index = 0; index < sent.size(); ++index)
	{
		unsigned int value = 0; std::memcpy(&value, sent[index].bytes, sizeof(value));
		Check(value == index && sent[index].token == index && sent[index].generation == generation,
			"deep copied payload, generation and FIFO submission order");
		Check(sent[index].address == 0x0a000001U + index && sent[index].port == 31000 + index,
			"send endpoint is copied in host order");
		NetworkIoCompletion completion;
		Check(session.owner->PollSendCompletion(generation, completion) == NetworkIoOwner::ACCEPTED &&
			completion.token == index && completion.sentBytes == sizeof(value) && completion.requestedBytes == sizeof(value),
			"completion FIFO preserves per-slot accounting");
	}
	for (unsigned int index = 0; index <= NETWORK_IO_QUEUE_CAPACITY; ++index) socket.Inject(index);
	Check(WaitUntil([&] { return session.owner->Metrics().receiveSaturation != 0; }), "receive pressure is observable");
	for (unsigned int index = 0; index <= NETWORK_IO_QUEUE_CAPACITY; ++index)
	{
		NetworkIoDatagram datagram;
		Check(WaitUntil([&] { return session.owner->Receive(generation, datagram) == NetworkIoOwner::ACCEPTED; }),
			"receive queue pressure leaves next datagram at socket rather than dropping it");
		unsigned int value = ~0U; std::memcpy(&value, datagram.bytes, sizeof(value));
		Check(value == index && datagram.generation == generation && datagram.address == 0x0a010203U && datagram.port == 32001,
			"ingress bytes, source identity and arrival order survive handoff");
	}
	const auto metrics = session.owner->Metrics();
	Check(metrics.sendHighWater == NETWORK_IO_QUEUE_CAPACITY && metrics.receiveHighWater == NETWORK_IO_QUEUE_CAPACITY,
		"both byte budgets have fixed high-water bounds");
	Check(metrics.ioThreadId != GetCurrentThreadId() && socket.ownershipFailures == 0,
		"all native callbacks execute on registered network owner, not producer");
	Check(session.owner->Stop(true), "clean session drain reports success");
}

void TestSessionIsolationAndGeneration()
{
	FakeSocket lobby, match;
	Session a(&lobby), b(&match);
	if (!a.owner || !b.owner) return;
	Check(a.owner->Start(0, 20000) && b.owner->Start(0, 20001), "coexisting lobby and match socket sessions");
	Check(a.owner->Metrics().ioThreadId == b.owner->Metrics().ioThreadId, "one service thread owns all sessions");
	const auto oldGeneration = a.owner->Generation();
	lobby.Inject(71);
	Check(WaitUntil([&] { return a.owner->Metrics().received == 1; }), "old ingress waits for owner publication");
	for (unsigned int index = 0; index < 80; ++index) Check(SubmitValue(a.owner, index), "queue final leave traffic");
	Check(a.owner->Stop(true), "reset drains accepted final sends on old endpoint");
	Check(lobby.Sent().size() == 80 && lobby.closes == 1 && match.closes == 0,
		"reset quiesces one socket without closing other active sessions");
	Check(SubmitValue(b.owner, 90), "other session still accepts traffic during reset");
	Check(a.owner->Start(0, 20002), "rebind after socket close");
	Check(a.owner->Generation() != oldGeneration, "restart advances session generation");
	unsigned char value = 2;
	Check(a.owner->Submit(oldGeneration, 1, 1, 1, &value, 1) == NetworkIoOwner::STALE, "stale sends rejected");
	NetworkIoDatagram datagram;
	NetworkIoCompletion completion;
	Check(a.owner->Receive(oldGeneration, datagram) == NetworkIoOwner::STALE, "stale receive consumers rejected");
	Check(a.owner->PollSendCompletion(oldGeneration, completion) == NetworkIoOwner::STALE, "stale completion consumers rejected");
	Check(a.owner->Receive(a.owner->Generation(), datagram) == NetworkIoOwner::EMPTY, "old buffered ingress never enters new session");
	Check(a.owner->PollSendCompletion(a.owner->Generation(), completion) == NetworkIoOwner::EMPTY,
		"old completion never frees a new owner output slot");
	Check(a.owner->Metrics().discardedReceives == 1 && a.owner->Metrics().staleRejected == 3, "stale/reset work is accounted");
	Check(a.owner->Metrics().ioThreadId == b.owner->Metrics().ioThreadId, "rebind reuses shared owner while another socket is active");
	Check(a.owner->Stop(true) && b.owner->Stop(true), "all sessions drain and stop");
	Check(lobby.ownershipFailures == 0 && match.ownershipFailures == 0, "close and restart stay on shared registered owner");
}

void TestFailureAndThreadContracts()
{
	FakeSocket socket;
	Session session(&socket);
	if (!session.owner) return;
	socket.openError = WSAEACCES;
	Check(!session.owner->Start(0, 12345) && socket.closes == 1, "failed bind closes before caller may use synchronous fallback");
	socket.openError = 0;
	// A conflict in the dedicated execution-owner registry fails startup safely.
	Check(rts::JobSystem::instance().registerCurrentThread(rts::JOB_OWNER_NETWORK), "claim conflicting owner for failure seam");
	Check(!session.owner->Start(0, 12345) && socket.opens == 1, "registration failure never opens socket on wrong thread");
	Check(rts::JobSystem::instance().unregisterCurrentThread(rts::JOB_OWNER_NETWORK), "release conflicting test owner");
	socket.ownerThread = 0; // A fully stopped service may get a new OS thread ID.
	Check(session.owner->Start(0, 12345), "start recovery after ownership failure");
	std::atomic<int> result{0};
	std::thread other([&] {
		unsigned char byte = 0;
		result = session.owner->Submit(session.owner->Generation(), 0, 1, 1, &byte, 1);
	});
	other.join();
	Check(result == NetworkIoOwner::WRONG_THREAD, "game producer identity enforced");
	socket.sendError = WSAEWOULDBLOCK;
	Check(SubmitValue(session.owner, 10), "enqueue send that will fail");
	NetworkIoCompletion completion;
	Check(WaitUntil([&] { return session.owner->PollSendCompletion(session.owner->Generation(), completion) == NetworkIoOwner::ACCEPTED; }) &&
		completion.sentBytes < 0 && completion.error == WSAEWOULDBLOCK,
		"send failure is a completion, not a successful accounting event");
	Check(SubmitValue(session.owner, 10), "producer can resubmit unchanged owner bytes after failure");
	Check(WaitUntil([&] { return session.owner->PollSendCompletion(session.owner->Generation(), completion) == NetworkIoOwner::ACCEPTED; }) &&
		completion.sentBytes == sizeof(unsigned int), "retry succeeds without duplicate I/O ownership");
	socket.throwSend = true;
	Check(SubmitValue(session.owner, 11), "enqueue throwing backend seam");
	Check(WaitUntil([&] { return session.owner->PollSendCompletion(session.owner->Generation(), completion) == NetworkIoOwner::ACCEPTED; }) &&
		completion.sentBytes < 0 && completion.error == WSAEFAULT, "backend exception contained and reported");
	socket.receiveError = WSAEADDRNOTAVAIL;
	NetworkIoDatagram datagram;
	Check(WaitUntil([&] { return session.owner->Receive(session.owner->Generation(), datagram) == NetworkIoOwner::FAILED; }) &&
		session.owner->LastError() == WSAEADDRNOTAVAIL, "receive status crosses boundary without worker logging/game globals");
	socket.broadcastError = WSAEACCES;
	Check(!session.owner->AllowBroadcasts(true), "broadcast control failure is explicit");
	Check(session.owner->Stop(true), "already-published failures do not poison a later empty drain");
	Check(socket.ownershipFailures == 0, "failure paths retain callback owner");
}

void TestDeadlineAndCallbackQuiescence()
{
	FakeSocket socket;
	Session session(&socket);
	if (!session.owner || !session.owner->Start(0, 12345)) return;
	ResetEvent(socket.sendRelease);
	Check(SubmitValue(session.owner, 0), "submit callback for controlled quiescence seam");
	Check(WaitForSingleObject(socket.sendEntered, 3000) == WAIT_OBJECT_0, "callback is active before stop");
	for (unsigned int index = 1; index < NETWORK_IO_QUEUE_CAPACITY; ++index) Check(SubmitValue(session.owner, index), "queue bounded pending shutdown work");
	std::thread releaser([&] { Sleep(20); SetEvent(socket.sendRelease); });
	Check(!session.owner->Stop(true, 0), "expired drain reports rejected pending sends");
	releaser.join();
	Check(socket.closes == 1 && socket.Sent().size() == 1, "active callback quiesces before close and cancelled sends do not run");
	const auto metrics = session.owner->Metrics();
	Check(metrics.cancelledSends == NETWORK_IO_QUEUE_CAPACITY - 1 && metrics.drainTimeouts == 1,
		"drain deadline reports exact cancellation count");
	const auto calls = socket.calls.load();
	Sleep(5);
	Check(socket.calls == calls, "no callback after Stop returns");
	Check(!SubmitValue(session.owner, 99), "stopped session rejects new work");
}

void TestSessionLimit()
{
	FakeSocket sockets[NETWORK_IO_MAX_SESSIONS + 1];
	NetworkIoOwner *owners[NETWORK_IO_MAX_SESSIONS + 1] = {};
	for (unsigned int index = 0; index <= NETWORK_IO_MAX_SESSIONS; ++index)
	{
		owners[index] = NetworkIoOwner::Create(&sockets[index]);
		Check(owners[index] != 0, "allocate facade within test budget");
		if (owners[index]) Check(owners[index]->Start(0, 20000 + index) == (index < NETWORK_IO_MAX_SESSIONS),
			"process-wide session registration is bounded");
	}
	for (unsigned int index = 0; index <= NETWORK_IO_MAX_SESSIONS; ++index) NetworkIoOwner::Destroy(owners[index]);
}

void TestNativeLoopback()
{
	Session a, b;
	if (!a.owner || !b.owner) return;
	Check(a.owner->Start(0x7f000001U, 0) && b.owner->Start(0x7f000001U, 0), "bind real nonblocking loopback sockets");
	Check(a.owner->LocalPort() != 0 && b.owner->LocalPort() != 0, "ephemeral binding returns actual local endpoint");
	Check(a.owner->Metrics().ioThreadId == b.owner->Metrics().ioThreadId, "native sockets share the registered I/O owner");
	Check(a.owner->AllowBroadcasts(true), "real broadcast socket option");
	for (unsigned int index = 0; index < 24; ++index)
		Check(SubmitValue(a.owner, index, 0x7f000001U, b.owner->LocalPort()), "enqueue loopback bytes");
	for (unsigned int index = 0; index < 24; ++index)
	{
		NetworkIoDatagram datagram = {};
		Check(WaitUntil([&] { return b.owner->Receive(b.owner->Generation(), datagram) == NetworkIoOwner::ACCEPTED; }), "loopback arrives without game tick socket I/O");
		unsigned int value = ~0U; std::memcpy(&value, datagram.bytes, sizeof(value));
		Check(value == index && datagram.address == 0x7f000001U && datagram.port == a.owner->LocalPort(),
			"loopback payload/order/native source endpoint are unchanged");
	}
	Check(SubmitValue(a.owner, 99, 0x7f000001U, 0), "invalid endpoint reports asynchronously");
	Check(WaitUntil([&] { return a.owner->Metrics().sendFailures == 1; }), "native invalid endpoint failure recorded");
	NetworkIoCompletion completion;
	bool sawFailure = false;
	while (a.owner->PollSendCompletion(a.owner->Generation(), completion) == NetworkIoOwner::ACCEPTED)
		if (completion.token == 99) sawFailure = completion.error == WSAEADDRNOTAVAIL && completion.sentBytes < 0;
	Check(sawFailure, "native invalid endpoint status preserved");
	// A peer outside the service can send a datagram larger than its receive
	// budget. recvfrom must report truncation, never publish a valid prefix.
	WSADATA wsa;
	Check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "start independent oversized test peer");
	SOCKET peer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	sockaddr_in destination = {};
	destination.sin_family = AF_INET; destination.sin_addr.s_addr = htonl(0x7f000001U); destination.sin_port = htons(b.owner->LocalPort());
	char oversized[NETWORK_IO_MAX_DATAGRAM + 1] = {};
	Check(sendto(peer, oversized, sizeof(oversized), 0, reinterpret_cast<const sockaddr *>(&destination), sizeof(destination)) == sizeof(oversized),
		"send oversized real datagram");
	NetworkIoDatagram datagram;
	Check(WaitUntil([&] { return b.owner->Receive(b.owner->Generation(), datagram) == NetworkIoOwner::FAILED; }) &&
		b.owner->LastError() == WSAEMSGSIZE, "native truncation fails explicitly without ingress publication");
	closesocket(peer); WSACleanup();
	Check(a.owner->Stop(true) && b.owner->Stop(true), "native loopback socket lifetimes drain and close");
}
}

int main()
{
	const unsigned int workerCount = rts::JobSystem::instance().workerCount();
	TestCopiesFifoAndQueuePressure();
	TestSessionIsolationAndGeneration();
	TestFailureAndThreadContracts();
	TestDeadlineAndCallbackQuiescence();
	TestSessionLimit();
	TestNativeLoopback();
	Check(rts::JobSystem::instance().workerCount() == workerCount, "socket service does not start or consume compute workers");
	std::printf("Network I/O owner tests: %d failures\n", failures);
	return failures ? 1 : 0;
}
