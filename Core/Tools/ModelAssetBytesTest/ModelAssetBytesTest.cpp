#include "Lib/ModelAssetBytes.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace
{
int failures = 0;
std::thread::id owner;
std::atomic<unsigned> wrongReadOwner(0), wrongDestructionOwner(0), sourcesDestroyed(0);

void check(bool condition, const char *message)
{
	if (!condition) { ++failures; std::printf("FAIL: %s\n", message); }
}

void put32(std::vector<unsigned char> &bytes, size_t offset, unsigned value)
{
	for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<unsigned char>(value >> (i * 8));
}

std::vector<unsigned char> chunk(const std::vector<unsigned char> &payload, bool nested = false)
{
	std::vector<unsigned char> bytes(payload.size() + 8);
	put32(bytes, 0, 0x1234);
	put32(bytes, 4, static_cast<unsigned>(payload.size()) | (nested ? 0x80000000u : 0));
	if (!payload.empty()) std::memcpy(bytes.data() + 8, payload.data(), payload.size());
	return bytes;
}

struct Source : rts::ResourceIoSource
{
	Source(const std::vector<unsigned char> &source, bool fail = false) : bytes(source), fail(fail) {}
	~Source()
	{
		++sourcesDestroyed;
		if (std::this_thread::get_id() != owner) ++wrongDestructionOwner;
	}
	size_t size() const { return bytes.size(); }
	int read(size_t offset, void *destination, unsigned count)
	{
		if (!rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_IO)) ++wrongReadOwner;
		if (fail) return -1;
		if (offset > bytes.size() || count > bytes.size() - offset) return -1;
		std::memcpy(destination, bytes.data() + offset, count);
		return static_cast<int>(count);
	}
	std::vector<unsigned char> bytes;
	bool fail;
};

struct TextureLikeDecode : rts::ResourceDecodeOperation
{
	bool prepare(const unsigned char *, size_t, size_t &workspace) { workspace = 64; return true; }
	bool decode(const unsigned char *, size_t, const rts::ResourceCancellation &cancel)
	{
		return !cancel.isCancelled();
	}
};

void consumeModel(rts::ResourceIoPipeline &pipeline, rts::ModelAssetReadQueue &queue,
	const rts::ResourceIoTicket &ticket, rts::ResourceIoStatus expected,
	const std::vector<unsigned char> *reference = 0)
{
	check(pipeline.wait(ticket), "model byte request completes");
	queue.pump(pipeline);
	rts::ResourceIoStatus status = rts::RESOURCE_IO_PENDING;
	rts::ModelAssetBytes *bytes = 0;
	check(queue.take(ticket, status, bytes), "owner takes model result by ticket");
	check(status == expected, "model result status is preserved");
	if (reference)
		check(bytes && bytes->size() == reference->size() &&
			std::memcmp(bytes->data(), reference->data(), reference->size()) == 0,
			"retained model bytes equal the serial input after pipeline retirement");
	delete bytes;
}

void testCopiesAndEnvelopes(unsigned workers, bool serialDecode)
{
	rts::JobSystemConfig jobs = rts::JobSystem::startupConfig(); jobs.workerCount = workers;
	check(rts::JobSystem::instance().start(jobs), "model test pool starts");
	rts::ResourceIoPipeline pipeline;
	check(pipeline.start(rts::ResourceIoConfig(), serialDecode ? rts::JobGroup() :
		rts::JobSystem::instance().createGroup()), "model test shares one I/O owner");
	rts::ModelAssetReadQueue queue;
	std::vector<unsigned char> leaf = chunk(std::vector<unsigned char>(4096, 0x5a));
	std::vector<unsigned char> producer = chunk(leaf, true);
	const std::vector<unsigned char> reference = producer;
	rts::ResourceIoTicket tickets[4];
	const unsigned destroyedBefore = sourcesDestroyed.load();
	for (unsigned i = 0; i < 4; ++i)
		check(queue.submit(pipeline, new Source(producer), tickets[i]), "independent named model admitted");
	std::memset(producer.data(), 0, producer.size());
	for (unsigned i = 0; i < 4; ++i) check(pipeline.wait(tickets[i]), "all model copies finish");
	if (!serialDecode)
	{
		const rts::JobSystemMetrics metrics = rts::JobSystem::instance().metrics();
		check(metrics.submittedJobCount == 4 &&
			metrics.executedJobCount == 4 &&
			pipeline.metrics().serialFallbacks == 0,
			"model copies are admitted and completed through the shared scheduler");
	}
	queue.pump(pipeline);
	check(pipeline.empty() && pipeline.metrics().inputBytes == 0 && pipeline.metrics().decodeBytes == 0,
		"model pump releases shared input and decode budgets before owner parsing");
	check(sourcesDestroyed.load() == destroyedBefore + 4,
		"source handles retire on owner before retained bytes are consumed");
	check(queue.retainedBytes() == reference.size() * 4, "private copied outputs remain budgeted");
	for (unsigned i = 0; i < 4; ++i)
		consumeModel(pipeline, queue, tickets[i], rts::RESOURCE_IO_SUCCEEDED, &reference);
	check(queue.retainedBytes() == 0 && queue.pendingRequests() == 0, "FIFO adoption releases every model reservation");

	std::vector<std::vector<unsigned char> > malformed;
	malformed.push_back(chunk(leaf, true));
	put32(malformed.back(), 4, 0x80000000u | static_cast<unsigned>(malformed.back().size()));
	malformed.push_back(chunk(leaf, true));
	put32(malformed.back(), 12, static_cast<unsigned>(leaf.size()));
	malformed.push_back(chunk(std::vector<unsigned char>(1, 7), true));
	malformed.push_back(chunk(std::vector<unsigned char>(1, 7)));
	malformed.back().push_back(0); // Incomplete next top-level envelope.
	std::vector<unsigned char> deep = chunk(std::vector<unsigned char>());
	for (unsigned i = 0; i < 255; ++i) deep = chunk(deep, true);
	malformed.push_back(deep);
	for (const std::vector<unsigned char> &bad : malformed)
	{
		rts::ResourceIoTicket ticket;
		check(queue.submit(pipeline, new Source(bad), ticket), "malformed envelope has bounded input admission");
		consumeModel(pipeline, queue, ticket, rts::RESOURCE_IO_DECODE_FAILED);
	}
	// Older W3D files may not mark nested contents. Their payload stays opaque
	// until the legacy owner parser; the byte pass must not guess record types.
	const std::vector<unsigned char> legacy = chunk(std::vector<unsigned char>(11, 0xff));
	rts::ResourceIoTicket legacyTicket;
	check(queue.submit(pipeline, new Source(legacy), legacyTicket), "legacy unflagged model accepted");
	consumeModel(pipeline, queue, legacyTicket, rts::RESOURCE_IO_SUCCEEDED, &legacy);
	check(pipeline.metrics().decoded >= 5, "model byte operation actually executes through resource decode");
	if (serialDecode) check(pipeline.metrics().serialFallbacks != 0, "unavailable compute retains serial decode fallback");
	queue.discard(pipeline); pipeline.shutdown(); rts::JobSystem::instance().shutdown();
}

void testBoundsFailuresAndGeneration()
{
	rts::JobSystemConfig jobs = rts::JobSystem::startupConfig(); jobs.workerCount = 2;
	check(rts::JobSystem::instance().start(jobs), "bounds pool starts");
	rts::ResourceIoPipeline pipeline;
	check(pipeline.start(rts::ResourceIoConfig(), rts::JobSystem::instance().createGroup()), "bounds pipeline starts");
	rts::ModelAssetReadQueue queue;
	const std::vector<unsigned char> small = chunk(std::vector<unsigned char>(8, 1));
	rts::ResourceIoTicket first, second, rejected;
	std::vector<unsigned char> maximum = chunk(std::vector<unsigned char>(rts::ModelAssetBytes::MAXIMUM_BYTES - 8, 2));
	check(queue.submit(pipeline, new Source(maximum), first), "maximum-size model admitted");
	check(queue.submit(pipeline, new Source(maximum), second), "second model fills retained-byte limit");
	Source *overflow = new Source(small);
	check(!queue.submit(pipeline, overflow, rejected) && !rejected.isValid(),
		"retained byte overflow rejects without source ownership transfer");
	delete overflow;
	check(queue.retainedBytes() == rts::ModelAssetReadQueue::RETAINED_BYTE_BUDGET,
		"model reservation high water is exactly bounded");
	queue.discard(pipeline);
	check(pipeline.empty() && queue.retainedBytes() == 0, "discard cancels active copies and releases both budgets");
	maximum.push_back(0);
	Source *oversize = new Source(maximum);
	check(!queue.submit(pipeline, oversize, rejected), "oversized model selects owner fallback before I/O admission");
	delete oversize;
	Source *shortSource = new Source(std::vector<unsigned char>(7));
	check(!queue.submit(pipeline, shortSource, rejected), "incomplete header rejects before I/O admission");
	delete shortSource;
	for (unsigned i = 0; i < rts::ModelAssetReadQueue::MAXIMUM_REQUESTS; ++i)
		check(queue.submit(pipeline, new Source(small), first), "fixed request slot admitted");
	Source *oneMore = new Source(small);
	check(!queue.submit(pipeline, oneMore, rejected), "request count is bounded independently of bytes");
	delete oneMore;
	queue.discard(pipeline);
	check(queue.submit(pipeline, new Source(small, true), first), "read failure fixture admitted");
	consumeModel(pipeline, queue, first, rts::RESOURCE_IO_READ_FAILED);
	check(queue.submit(pipeline, new Source(small), first), "cancel fixture admitted");
	check(pipeline.cancel(first), "in-flight model cancelled");
	consumeModel(pipeline, queue, first, rts::RESOURCE_IO_CANCELLED);
	check(queue.submit(pipeline, new Source(small), first), "map generation fixture admitted");
	pipeline.advanceGeneration();
	consumeModel(pipeline, queue, first, rts::RESOURCE_IO_STALE);
	check(queue.submit(pipeline, new Source(small), first), "retained shutdown fixture admitted");
	check(pipeline.wait(first), "shutdown fixture completes before cancellation");
	queue.pump(pipeline);
	check(pipeline.empty() && queue.retainedBytes() != 0, "shutdown includes already-retired copied output");
	check(queue.submit(pipeline, new Source(small), second), "shutdown also includes an in-flight model");
	rts::JobSystem::instance().shutdown();
	queue.discard(pipeline);
	check(queue.retainedBytes() == 0 && queue.pendingRequests() == 0, "shutdown releases privately retained output");
	pipeline.shutdown();
	check(!rts::JobSystem::instance().isRunning(), "model cancellation cannot restart stopped compute workers");
}

void consumeTexture(rts::ResourceIoPipeline &pipeline, const rts::ResourceIoTicket &ticket)
{
	check(pipeline.wait(ticket), "texture-like request makes forward progress");
	rts::ResourceIoStatus status;
	rts::ResourceDecodeOperation *operation = 0;
	check(pipeline.take(ticket, status, operation) && status == rts::RESOURCE_IO_SUCCEEDED,
		"texture keeps its own typed ticket publication");
	delete operation;
}

void testSharedTexturePressure()
{
	rts::JobSystemConfig jobs = rts::JobSystem::startupConfig(); jobs.workerCount = 2;
	check(rts::JobSystem::instance().start(jobs), "mixed pool starts");
	rts::ResourceIoConfig config; config.decodeByteBudget = 64;
	rts::ResourceIoPipeline pipeline;
	check(pipeline.start(config, rts::JobSystem::instance().createGroup()), "mixed pipeline starts");
	rts::ModelAssetReadQueue queue;
	const std::vector<unsigned char> model = chunk(std::vector<unsigned char>(32, 7));
	rts::ResourceIoTicket modelTicket, textureTicket;
	check(queue.submit(pipeline, new Source(model), modelTicket), "model precedes foreground texture");
	check(pipeline.submit(new Source(model), new TextureLikeDecode,
		rts::JOB_PRIORITY_FRAME_CRITICAL, textureTicket), "foreground texture uses same bounded pipeline");
	check(pipeline.wait(modelTicket), "earlier model copy completes");
	check(!pipeline.wait(textureTicket), "retained model would block texture until model bookkeeping is pumped");
	queue.pump(pipeline);
	consumeTexture(pipeline, textureTicket);
	check(queue.contains(modelTicket) && queue.retainedBytes() == model.size(),
		"texture progress does not prematurely parse or drop pending model output");
	consumeModel(pipeline, queue, modelTicket, rts::RESOURCE_IO_SUCCEEDED, &model);
	check(pipeline.submit(new Source(model), new TextureLikeDecode,
		rts::JOB_PRIORITY_STREAMING, textureTicket), "texture precedes model");
	check(queue.submit(pipeline, new Source(model), modelTicket), "model shares budget behind texture");
	check(pipeline.wait(textureTicket), "earlier texture output retained");
	check(!pipeline.wait(modelTicket), "model wait yields instead of deadlocking behind texture output");
	consumeTexture(pipeline, textureTicket);
	consumeModel(pipeline, queue, modelTicket, rts::RESOURCE_IO_SUCCEEDED, &model);
	check(pipeline.metrics().decodeHighWater <= config.decodeByteBudget, "mixed workloads respect shared decode budget");
	queue.discard(pipeline); pipeline.shutdown();
	config.maximumRequests = 1;
	config.decodeByteBudget = 8;
	check(pipeline.start(config, rts::JobSystem::instance().createGroup()), "small shared-budget pipeline restarts");
	check(queue.submit(pipeline, new Source(model), modelTicket), "oversized decoded workspace has bounded input");
	Source *rejected = new Source(model);
	check(!queue.submit(pipeline, rejected, textureTicket), "shared request saturation rejects without model bookkeeping");
	delete rejected;
	check(queue.pendingRequests() == 1 && queue.retainedBytes() == model.size(),
		"failed shared admission releases its operation without reserving model bytes");
	consumeModel(pipeline, queue, modelTicket, rts::RESOURCE_IO_DECODE_FAILED);
	queue.discard(pipeline); pipeline.shutdown(); rts::JobSystem::instance().shutdown();
}
}

int main()
{
	owner = std::this_thread::get_id();
	testCopiesAndEnvelopes(1, false);
	testCopiesAndEnvelopes(4, false);
	testCopiesAndEnvelopes(1, true);
	testBoundsFailuresAndGeneration();
	testSharedTexturePressure();
	check(wrongReadOwner.load() == 0, "model reads run only on shared I/O owner");
	check(wrongDestructionOwner.load() == 0, "model source destruction remains owner-only");
	std::printf("model asset bytes: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
