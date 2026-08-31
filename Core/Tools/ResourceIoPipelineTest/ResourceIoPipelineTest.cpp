#include "Lib/ResourceIoPipeline.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace
{
int failures = 0;
std::thread::id owner;
std::atomic<unsigned> wrongDestructionThread(0), sourcesDestroyed(0), decodersDestroyed(0);
void check(bool value, const char *message)
{
	if (!value) { ++failures; std::printf("FAIL: %s\n", message); }
}
struct Gate
{
	Gate() : entered(false), open(false) {}
	void block()
	{
		std::unique_lock<std::mutex> lock(mutex);
		entered = true; changed.notify_all();
		changed.wait_for(lock, std::chrono::seconds(5), [this]() { return open; });
	}
	bool reached()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return entered;
	}
	void release()
	{
		std::lock_guard<std::mutex> lock(mutex);
		open = true; changed.notify_all();
	}
	std::mutex mutex;
	std::condition_variable changed;
	bool entered, open;
};
struct Source : rts::ResourceIoSource
{
	Source(const std::vector<unsigned char> &bytes, Gate *gate = 0, int fault = 0)
		: bytes(bytes), gate(gate), fault(fault), readOnIo(false) {}
	~Source()
	{
		++sourcesDestroyed;
		if (std::this_thread::get_id() != owner) ++wrongDestructionThread;
	}
	size_t size() const { return bytes.size(); }
	int read(size_t offset, void *destination, unsigned count)
	{
		readOnIo = rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_IO) &&
			!rts::JobSystem::instance().isWorkerThread();
		if (gate && offset == 0) gate->block();
		if (fault == 1) return static_cast<int>(count) - 1;
		if (fault == 2) return -1;
		if (fault == 3) throw std::runtime_error("read fault");
		if (offset > bytes.size() || count > bytes.size() - offset) return -1;
		std::memcpy(destination, bytes.data() + offset, count);
		return static_cast<int>(count);
	}
	std::vector<unsigned char> bytes;
	Gate *gate;
	int fault;
	bool readOnIo;
};
struct Decode : rts::ResourceDecodeOperation
{
	Decode(size_t workspace, Gate *gate = 0, int fault = 0)
		: workspace(workspace), gate(gate), fault(fault), ownerPrepared(false), workerDecoded(false) {}
	~Decode()
	{
		++decodersDestroyed;
		if (std::this_thread::get_id() != owner) ++wrongDestructionThread;
	}
	bool prepare(const unsigned char *, size_t, size_t &bytes)
	{
		ownerPrepared = std::this_thread::get_id() == owner;
		bytes = workspace;
		if (fault == 1) return false;
		if (fault == 2) throw std::runtime_error("header fault");
		return true;
	}
	bool decode(const unsigned char *bytes, size_t size, const rts::ResourceCancellation &cancel)
	{
		workerDecoded = rts::JobSystem::instance().isWorkerThread();
		if (gate) gate->block();
		if (cancel.isCancelled()) return false;
		if (fault == 3) return false;
		if (fault == 4) throw std::runtime_error("decode fault");
		output.resize(size);
		for (size_t i = 0; i < size; ++i) output[i] = bytes[i] ^ 0x5a;
		return true;
	}
	size_t workspace;
	Gate *gate;
	int fault;
	bool ownerPrepared, workerDecoded;
	std::vector<unsigned char> output;
};
template<class Predicate> bool pumpUntil(rts::ResourceIoPipeline &pipeline, Predicate predicate)
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!predicate() && std::chrono::steady_clock::now() < end)
	{
		pipeline.pump();
		std::this_thread::yield();
	}
	return predicate();
}
void consume(rts::ResourceIoPipeline &pipeline, const rts::ResourceIoTicket &ticket,
	rts::ResourceIoStatus expected)
{
	rts::ResourceIoStatus status = rts::RESOURCE_IO_PENDING;
	rts::ResourceDecodeOperation *operation = 0;
	check(pipeline.take(ticket, status, operation), "completed ticket transfers its operation to owner");
	check(status == expected, "completion has the expected success/failure/cancellation state");
	delete operation;
}
void testParity(unsigned workers)
{
	rts::JobSystemConfig jobs = rts::JobSystem::startupConfig();
	jobs.workerCount = workers; jobs.queueCapacity = 128;
	check(rts::JobSystem::instance().start(jobs), "compute pool starts");
	rts::ResourceIoPipeline pipeline;
	check(pipeline.start(rts::ResourceIoConfig(), rts::JobSystem::instance().createGroup()), "IO owner starts");
	std::vector<unsigned char> producer(16384);
	for (size_t i = 0; i < producer.size(); ++i) producer[i] = static_cast<unsigned char>(i * 31);
	std::vector<rts::ResourceIoTicket> tickets(12);
	for (unsigned i = 0; i < tickets.size(); ++i)
	{
		Source *source = new Source(producer);
		Decode *decode = new Decode(producer.size());
		check(pipeline.submit(source, decode, rts::JOB_PRIORITY_STREAMING, tickets[i]), "bounded fixture admitted");
	}
	std::memset(producer.data(), 0, producer.size());
	for (unsigned i = 0; i < tickets.size(); ++i)
	{
		check(pipeline.wait(tickets[i]), "normal request completes");
		rts::ResourceIoTicket result;
		rts::ResourceIoStatus status;
		rts::ResourceDecodeOperation *operation = 0;
		check(pipeline.takeNext(result, status, operation), "FIFO publication is available");
		check(result.id == tickets[i].id && status == rts::RESOURCE_IO_SUCCEEDED, "completion preserves submission identity/order");
		Decode *decode = static_cast<Decode*>(operation);
		if (decode)
		{
			check(decode->ownerPrepared && decode->output.size() == producer.size(), "header preparation belongs to owner");
			for (size_t b = 0; b < decode->output.size(); ++b)
				if (decode->output[b] != (static_cast<unsigned char>(b * 31) ^ 0x5a))
					{ check(false, "immutable source bytes match serial reference"); break; }
		}
		delete operation;
	}
	check(pipeline.metrics().bytesRead == producer.size() * tickets.size(), "byte telemetry measures actual reads");
	check(pipeline.metrics().inputBytes == 0 && pipeline.metrics().decodeBytes == 0, "all retained byte reservations released");
	pipeline.shutdown();
	rts::JobSystem::instance().shutdown();
}
void testFaultsAndPressure()
{
	rts::JobSystemConfig jobs = rts::JobSystem::startupConfig(); jobs.workerCount = 2;
	check(rts::JobSystem::instance().start(jobs), "fault pool starts");
	rts::ResourceIoPipeline pipeline;
	rts::ResourceIoConfig config; config.maximumRequests = 2; config.inputByteBudget = 32; config.decodeByteBudget = 16;
	check(pipeline.start(config, rts::JobSystem::instance().createGroup()), "fault pipeline starts");
	std::vector<unsigned char> bytes(16, 7);
	for (int fault = 1; fault <= 3; ++fault)
	{
		rts::ResourceIoTicket ticket;
		check(pipeline.submit(new Source(bytes, 0, fault), new Decode(16), rts::JOB_PRIORITY_STREAMING, ticket), "read fault accepted");
		pipeline.wait(ticket);
		consume(pipeline, ticket, rts::RESOURCE_IO_READ_FAILED);
	}
	for (int fault = 1; fault <= 4; ++fault)
	{
		rts::ResourceIoTicket ticket;
		check(pipeline.submit(new Source(bytes), new Decode(16, 0, fault), rts::JOB_PRIORITY_STREAMING, ticket), "decode fault accepted");
		pipeline.wait(ticket);
		consume(pipeline, ticket, rts::RESOURCE_IO_DECODE_FAILED);
	}
	rts::ResourceIoTicket first, second, rejected;
	check(pipeline.submit(new Source(bytes), new Decode(16), rts::JOB_PRIORITY_STREAMING, first), "first budget request");
	check(pipeline.submit(new Source(bytes), new Decode(16), rts::JOB_PRIORITY_STREAMING, second), "second budget request");
	Source *source = new Source(bytes); Decode *decode = new Decode(16);
	check(!pipeline.submit(source, decode, rts::JOB_PRIORITY_STREAMING, rejected) && !rejected.isValid(), "saturation rejects without ownership transfer");
	delete source; delete decode;
	check(pipeline.wait(first), "first output holds decoded-byte budget");
	check(!pipeline.wait(second), "owner-retained output pressure returns instead of deadlocking");
	consume(pipeline, first, rts::RESOURCE_IO_SUCCEEDED);
	check(pipeline.wait(second), "taking first output unblocks second");
	consume(pipeline, second, rts::RESOURCE_IO_SUCCEEDED);
	rts::ResourceIoTicket excessive;
	check(pipeline.submit(new Source(bytes), new Decode(17), rts::JOB_PRIORITY_STREAMING, excessive), "header can reject oversized decode after bounded input");
	pipeline.wait(excessive); consume(pipeline, excessive, rts::RESOURCE_IO_DECODE_FAILED);
	check(pipeline.metrics().inputHighWater <= 32 && pipeline.metrics().decodeHighWater <= 16, "byte high-water stays bounded");
	pipeline.shutdown();
	rts::JobSystem::instance().shutdown();
}
void testOverlapCancellationAndShutdown()
{
	rts::JobSystemConfig jobs = rts::JobSystem::startupConfig(); jobs.workerCount = 4;
	check(rts::JobSystem::instance().start(jobs), "overlap pool starts");
	rts::ResourceIoPipeline pipeline;
	check(pipeline.start(rts::ResourceIoConfig(), rts::JobSystem::instance().createGroup()), "overlap pipeline starts");
	Gate ioGate, decodeGate;
	std::vector<unsigned char> bytes(1024, 3);
	rts::ResourceIoTicket decoding, reading;
	Source *firstSource = new Source(bytes);
	check(pipeline.submit(firstSource, new Decode(1024, &decodeGate), rts::JOB_PRIORITY_STREAMING, decoding), "decode overlap fixture");
	check(pipeline.submit(new Source(bytes, &ioGate), new Decode(1024), rts::JOB_PRIORITY_STREAMING, reading), "IO overlap fixture");
	check(pumpUntil(pipeline, [&]() { return ioGate.reached() && decodeGate.reached(); }), "blocking IO overlaps a shared compute job");
	check(firstSource->readOnIo, "disk reads execute on IO role, never compute workers");
	check(pipeline.metrics().maximumOverlappingIoAndDecode >= 2, "actual overlap telemetry is nonzero");
	check(pipeline.cancel(decoding) && pipeline.cancel(reading), "cancel accepted during active decode and IO");
	decodeGate.release(); ioGate.release();
	pipeline.advanceGeneration();
	consume(pipeline, decoding, rts::RESOURCE_IO_STALE);
	consume(pipeline, reading, rts::RESOURCE_IO_STALE);
	check(pipeline.generation() > reading.generation, "transition advances resource generation");
	// Shutdown may precede texture subsystem teardown. No lazy scheduler restart.
	rts::ResourceIoTicket pending;
	check(pipeline.submit(new Source(bytes), new Decode(1024), rts::JOB_PRIORITY_STREAMING, pending), "pre-shutdown pending IO accepted");
	rts::JobSystem::instance().shutdown();
	pipeline.shutdown();
	check(!rts::JobSystem::instance().isRunning(), "resource teardown cannot resurrect compute");
	check(pipeline.empty() && pipeline.metrics().inputBytes == 0, "shutdown cancels/drains retained inputs and outputs");
}
void testSerialFallback()
{
	rts::JobSystemConfig jobs = rts::JobSystem::startupConfig(); jobs.workerCount = 1;
	check(rts::JobSystem::instance().start(jobs), "fallback pool starts");
	rts::ResourceIoPipeline pipeline;
	// Invalid group models scheduler/submission unavailability while retaining IO.
	check(pipeline.start(rts::ResourceIoConfig(), rts::JobGroup()), "serial decode pipeline starts");
	rts::ResourceIoTicket ticket;
	check(pipeline.submit(new Source(std::vector<unsigned char>(32, 9)), new Decode(32), rts::JOB_PRIORITY_NORMAL, ticket), "serial fallback source admitted");
	pipeline.wait(ticket);
	rts::ResourceIoStatus status;
	rts::ResourceDecodeOperation *operation = 0;
	check(pipeline.take(ticket, status, operation), "serial fallback publishes");
	Decode *decode = static_cast<Decode*>(operation);
	check(status == rts::RESOURCE_IO_SUCCEEDED && decode && !decode->workerDecoded && decode->ownerPrepared,
		"serial fallback runs decode/publication on owner");
	check(pipeline.metrics().serialFallbacks == 1, "fallback counted");
	delete operation;
	pipeline.shutdown(); rts::JobSystem::instance().shutdown();
}

void testFifoBudgetAndWorkerFanout()
{
	rts::JobSystemConfig jobs = rts::JobSystem::startupConfig(); jobs.workerCount = 4;
	check(rts::JobSystem::instance().start(jobs), "FIFO budget pool starts");
	rts::ResourceIoPipeline pipeline;
	rts::ResourceIoConfig config; config.decodeByteBudget = 128;
	check(pipeline.start(config, rts::JobSystem::instance().createGroup()), "FIFO budget pipeline starts");
	std::vector<unsigned char> bytes(16, 11);
	rts::ResourceIoTicket c, a, b;
	check(pipeline.submit(new Source(bytes), new Decode(64), rts::JOB_PRIORITY_STREAMING, c), "C reserves 64");
	check(pipeline.submit(new Source(bytes), new Decode(96), rts::JOB_PRIORITY_STREAMING, a), "A requires 96");
	check(pipeline.submit(new Source(bytes), new Decode(64), rts::JOB_PRIORITY_STREAMING, b), "B requires 64");
	check(pipeline.promote(b), "later IO may run at foreground priority");
	check(pipeline.wait(c), "C completes while retaining its output");
	pipeline.pump();
	check(!pipeline.isComplete(b), "later small B cannot strand earlier A behind retained bytes");
	check(!pipeline.wait(b), "later unprepared ticket reports a real earlier retained-output budget block");
	consume(pipeline, c, rts::RESOURCE_IO_SUCCEEDED);
	check(pipeline.wait(a), "A makes progress after C publication");
	consume(pipeline, a, rts::RESOURCE_IO_SUCCEEDED);
	check(pipeline.wait(b), "B follows A without a FIFO/budget deadlock");
	consume(pipeline, b, rts::RESOURCE_IO_SUCCEEDED);
	Gate gates[4];
	rts::ResourceIoTicket tickets[4];
	for (unsigned i = 0; i < 4; ++i)
		check(pipeline.submit(new Source(bytes), new Decode(16, &gates[i]),
			rts::JOB_PRIORITY_STREAMING, tickets[i]), "independent decode admitted for fanout");
	check(pumpUntil(pipeline, [&]() { return gates[0].reached() && gates[1].reached() &&
		gates[2].reached() && gates[3].reached(); }), "four shared workers prepare independent resources concurrently");
	check(rts::JobSystem::instance().metrics().maximumActiveWorkers >= 4, "resource fanout is not capped at two workers");
	for (Gate &gate : gates) gate.release();
	for (const rts::ResourceIoTicket &ticket : tickets)
	{
		check(pipeline.wait(ticket), "fanout decode completes");
		consume(pipeline, ticket, rts::RESOURCE_IO_SUCCEEDED);
	}
	pipeline.shutdown(); rts::JobSystem::instance().shutdown();
}

void testWaitForNewlyPublishedReads()
{
	rts::JobSystemConfig jobs = rts::JobSystem::startupConfig(); jobs.workerCount = 4;
	check(rts::JobSystem::instance().start(jobs), "read publication pool starts");
	rts::ResourceIoPipeline pipeline;
	check(pipeline.start(rts::ResourceIoConfig(), rts::JobSystem::instance().createGroup()),
		"read publication pipeline starts");
	// No output is retained between requests and the budget exceeds every
	// workspace. A newly completed read must never look like budget pressure.
	for (unsigned iteration = 0; iteration < 256; ++iteration)
	{
		rts::ResourceIoTicket ticket;
		check(pipeline.submit(new Source(std::vector<unsigned char>(16, 19)), new Decode(32),
			rts::JOB_PRIORITY_STREAMING, ticket), "publication-race input admitted");
		check(pipeline.wait(ticket), "wait includes read publication and decode admission");
		consume(pipeline, ticket, rts::RESOURCE_IO_SUCCEEDED);
	}
	pipeline.shutdown(); rts::JobSystem::instance().shutdown();
}

void testOwnedNativeRange()
{
#if defined(_WIN32)
	char directory[MAX_PATH], path[MAX_PATH];
	if (!GetTempPathA(MAX_PATH, directory) || !GetTempFileNameA(directory, "rio", 0, path))
		{ check(false, "native range fixture created"); return; }
	FILE *file = std::fopen(path, "wb");
	const unsigned char bytes[] = {99, 98, 1, 2, 3, 4, 97, 96};
	if (!file) { check(false, "native range fixture opens"); DeleteFileA(path); return; }
	check(std::fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes), "native range fixture written");
	std::fclose(file);
	rts::ResourceIoSource *source = rts::ResourceIoSource::openFileRange(path, 2, 4);
	check(source != 0, "resolved member range captures independent owned handle");
	check(rts::ResourceIoSource::openFileRange(path, 7, 2) == 0, "range beyond file rejected");
	check(rts::ResourceIoSource::openFileRange(path, 1, static_cast<rts::JobMetricCounter>(-1)) == 0,
		"range arithmetic overflow rejected");
	check(DeleteFileA(path) != FALSE, "owned handle survives directory-entry removal");
	if (source)
	{
		unsigned char read[5] = {0};
		check(source->read(0, read, 4) == 4 && std::memcmp(read, bytes + 2, 4) == 0,
			"captured archive member range retains exact bytes after unlink");
		check(source->read(0, read, 5) == -1 && source->read(5, read, 0) == -1,
			"native source cannot read outside its resolved member");
		delete source;
	}
#endif
}
}

int main()
{
	owner = std::this_thread::get_id();
	const unsigned workers[] = {1, 2, 4, 8, 16, 0};
	for (unsigned count : workers) testParity(count);
	testFaultsAndPressure();
	testOverlapCancellationAndShutdown();
	testSerialFallback();
	testFifoBudgetAndWorkerFanout();
	testWaitForNewlyPublishedReads();
	testOwnedNativeRange();
	check(wrongDestructionThread.load() == 0, "source and operation destructors are owner-only");
	check(sourcesDestroyed.load() == decodersDestroyed.load(), "source/operation ownership balances across failures");
	std::printf("resource IO pipeline: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
