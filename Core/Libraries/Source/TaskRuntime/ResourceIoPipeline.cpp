/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/ResourceIoPipeline.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace rts
{
namespace
{
typedef std::chrono::steady_clock Clock;
JobMetricCounter elapsed(Clock::time_point start)
{
	return static_cast<JobMetricCounter>(std::chrono::duration_cast<
		std::chrono::nanoseconds>(Clock::now() - start).count());
}

class FileRangeSource : public ResourceIoSource
{
public:
#if defined(_WIN32)
	FileRangeSource(HANDLE file, JobMetricCounter offset, size_t bytes)
#else
	FileRangeSource(FILE *file, JobMetricCounter offset, size_t bytes)
#endif
		: m_file(file), m_offset(offset), m_size(bytes) {}
	~FileRangeSource()
	{
#if defined(_WIN32)
		CloseHandle(m_file);
#else
		fclose(m_file);
#endif
	}
	size_t size() const { return m_size; }
	int read(size_t offset, void *destination, unsigned bytes)
	{
		if (offset > m_size || bytes > m_size - offset ||
			bytes > static_cast<unsigned>((std::numeric_limits<int>::max)())) return -1;
#if defined(_WIN32)
		LARGE_INTEGER position;
		position.QuadPart = static_cast<LONGLONG>(m_offset + offset);
		DWORD transferred = 0;
		if (!SetFilePointerEx(m_file, position, 0, FILE_BEGIN) ||
			!ReadFile(m_file, destination, bytes, &transferred, 0)) return -1;
		return static_cast<int>(transferred);
#else
		if (fseeko(m_file, static_cast<off_t>(m_offset + offset), SEEK_SET)) return -1;
		const size_t transferred = fread(destination, 1, bytes, m_file);
		return ferror(m_file) ? -1 : static_cast<int>(transferred);
#endif
	}
private:
#if defined(_WIN32)
	HANDLE m_file;
#else
	FILE *m_file;
#endif
	JobMetricCounter m_offset;
	size_t m_size;
};
}

ResourceIoSource::~ResourceIoSource() {}
ResourceDecodeOperation::~ResourceDecodeOperation() {}
bool ResourceCancellation::isCancelled() const
{
	return static_cast<const std::atomic<bool> *>(m_flag)->load(std::memory_order_acquire);
}

ResourceIoSource *ResourceIoSource::openFileRange(const char *path,
	JobMetricCounter offset, JobMetricCounter bytes)
{
	if (path == 0 || bytes == 0 || bytes > (std::numeric_limits<size_t>::max)() ||
		bytes > static_cast<JobMetricCounter>((std::numeric_limits<long long>::max)()) ||
		offset > static_cast<JobMetricCounter>((std::numeric_limits<long long>::max)()) - bytes)
		return 0;
#if defined(_WIN32)
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
		0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, 0);
	if (file == INVALID_HANDLE_VALUE) return 0;
	LARGE_INTEGER size;
	if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
		offset + bytes > static_cast<JobMetricCounter>(size.QuadPart))
	{
		CloseHandle(file);
		return 0;
	}
#else
	FILE *file = fopen(path, "rb");
	if (file == 0) return 0;
	if (fseeko(file, 0, SEEK_END) || ftello(file) < 0 ||
		offset + bytes > static_cast<JobMetricCounter>(ftello(file)))
	{
		fclose(file);
		return 0;
	}
#endif
	try { return new FileRangeSource(file, offset, static_cast<size_t>(bytes)); }
	catch (...)
	{
#if defined(_WIN32)
		CloseHandle(file);
#else
		fclose(file);
#endif
		return 0;
	}
}

ResourceIoConfig::ResourceIoConfig() : maximumRequests(32),
	inputByteBudget(64u * 1024u * 1024u), decodeByteBudget(128u * 1024u * 1024u),
	readChunkBytes(64u * 1024u) {}
ResourceIoMetrics::ResourceIoMetrics() : accepted(0), rejected(0), reads(0),
	readFailures(0), decoded(0), cancelled(0), stale(0), serialFallbacks(0),
	bytesRead(0), ownershipFailures(0), decodeBudgetStalls(0), readNanoseconds(0), decodeNanoseconds(0), ownerWaitNanoseconds(0),
	pendingRequests(0), requestHighWater(0), activeIo(0), activeDecode(0),
	maximumOverlappingIoAndDecode(0), inputBytes(0), decodeBytes(0),
	inputHighWater(0), decodeHighWater(0) {}

struct ResourceIoPipeline::State
{
	enum Phase { QUEUED, READING, READ_READY, DECODING, DONE };
	struct Request
	{
		Request() : source(0), operation(0), priority(JOB_PRIORITY_STREAMING),
			phase(QUEUED), status(RESOURCE_IO_PENDING), cancelled(false), workspace(0),
			requiredWorkspace(0), headerPrepared(false), decodeJob(0) {}
		~Request() { delete source; delete operation; }
		ResourceIoTicket ticket;
		ResourceIoSource *source;
		ResourceDecodeOperation *operation;
		JobPriority priority;
		Phase phase;
		ResourceIoStatus status;
		std::atomic<bool> cancelled;
		size_t workspace;
		size_t requiredWorkspace;
		bool headerPrepared;
		std::vector<unsigned char> bytes;
		JobHandle decode;
		Job *decodeJob;
	};
	typedef std::shared_ptr<Request> RequestPtr;
	struct DecodeJob : Job
	{
		DecodeJob(State *owner, const RequestPtr &request) : owner(owner), request(request) {}
		void execute(JobContext &context)
		{
			if (context.isCancellationRequested()) request->cancelled.store(true);
			owner->decodeRequest(request);
		}
		State *owner;
		RequestPtr request;
	};
	State() : running(false), stopping(false), registered(false), registrationFinished(false), nextId(0), generation(1) {}
	bool onOwner() const { return owner == std::this_thread::get_id(); }
	RequestPtr find(const ResourceIoTicket &ticket) const
	{
		for (const RequestPtr &request : requests)
			if (request->ticket.id == ticket.id && request->ticket.generation == ticket.generation)
				return request;
		return RequestPtr();
	}
	void finish(Request &request, ResourceIoStatus status)
	{
		if (request.phase == DONE) return;
		request.status = status;
		request.phase = DONE;
		if (status == RESOURCE_IO_CANCELLED) ++metrics.cancelled;
		if (status == RESOURCE_IO_READ_FAILED) ++metrics.readFailures;
		changed.notify_all();
	}
	void overlap()
	{
		if (metrics.activeIo && metrics.activeDecode)
			metrics.maximumOverlappingIoAndDecode = (std::max)(
				metrics.maximumOverlappingIoAndDecode, metrics.activeIo + metrics.activeDecode);
	}
	void decodeRequest(const RequestPtr &request)
	{
		const Clock::time_point start = Clock::now();
		{
			std::lock_guard<std::mutex> lock(mutex);
			++metrics.activeDecode;
			overlap();
		}
		bool succeeded = false;
		try
		{
			const ResourceCancellation cancellation(&request->cancelled);
			if (!cancellation.isCancelled())
				succeeded = request->operation->decode(request->bytes.data(),
					request->bytes.size(), cancellation);
		}
		catch (...) {}
		std::lock_guard<std::mutex> lock(mutex);
		--metrics.activeDecode;
		metrics.decodeNanoseconds += elapsed(start);
		if (succeeded) ++metrics.decoded;
		finish(*request, request->cancelled.load() ? RESOURCE_IO_CANCELLED :
			(succeeded ? RESOURCE_IO_SUCCEEDED : RESOURCE_IO_DECODE_FAILED));
	}
	void ioLoop()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			registered = JobSystem::instance().registerCurrentThread(JOB_OWNER_IO);
			registrationFinished = true;
			changed.notify_all();
			if (!registered) { ++metrics.ownershipFailures; return; }
		}
		for (;;)
		{
			RequestPtr request;
			{
				std::unique_lock<std::mutex> lock(mutex);
				for (;;)
				{
					for (const RequestPtr &candidate : requests)
						if (candidate->phase == QUEUED && (!request || candidate->priority < request->priority))
							request = candidate;
					if (request || stopping) break;
					changed.wait(lock);
				}
				if (!request) break;
				if (request->cancelled.load()) { finish(*request, RESOURCE_IO_CANCELLED); continue; }
				request->phase = READING;
				metrics.activeIo = 1;
				overlap();
			}
			const Clock::time_point start = Clock::now();
			bool succeeded = true;
			size_t offset = 0;
			try
			{
				request->bytes.resize(request->source->size());
				while (offset < request->bytes.size() && !request->cancelled.load())
				{
					const unsigned bytes = static_cast<unsigned>((std::min)(
						request->bytes.size() - offset, static_cast<size_t>(config.readChunkBytes)));
					const int actual = request->source->read(offset, request->bytes.data() + offset, bytes);
					if (actual != static_cast<int>(bytes)) { succeeded = false; break; }
					offset += bytes;
				}
			}
			catch (...) { succeeded = false; }
			std::lock_guard<std::mutex> lock(mutex);
			metrics.activeIo = 0;
			++metrics.reads;
			metrics.bytesRead += offset;
			metrics.readNanoseconds += elapsed(start);
			if (request->cancelled.load()) finish(*request, RESOURCE_IO_CANCELLED);
			else if (!succeeded) finish(*request, RESOURCE_IO_READ_FAILED);
			else { request->phase = READ_READY; changed.notify_all(); }
		}
		if (!JobSystem::instance().unregisterCurrentThread(JOB_OWNER_IO))
		{
			std::lock_guard<std::mutex> lock(mutex);
			++metrics.ownershipFailures;
		}
	}
	mutable std::mutex mutex;
	std::condition_variable changed;
	std::thread io;
	std::thread::id owner;
	bool running, stopping, registered, registrationFinished;
	JobMetricCounter nextId, generation;
	ResourceIoConfig config;
	ResourceIoMetrics metrics;
	JobGroup group;
	std::list<RequestPtr> requests;
};

ResourceIoPipeline::ResourceIoPipeline() : m_state(new State) {}
ResourceIoPipeline::~ResourceIoPipeline() { shutdown(); delete m_state; }
bool ResourceIoPipeline::start(const ResourceIoConfig &config, const JobGroup &decodeGroup)
{
	State &state = *m_state;
	if (state.running || !config.maximumRequests || !config.inputByteBudget ||
		!config.decodeByteBudget || !config.readChunkBytes ||
		config.readChunkBytes > static_cast<unsigned>((std::numeric_limits<int>::max)())) return false;
	state.owner = std::this_thread::get_id();
	state.config = config;
	state.group = decodeGroup;
	state.stopping = false;
	state.registrationFinished = false;
	try { state.io = std::thread(&State::ioLoop, &state); }
	catch (...) { return false; }
	{
		std::unique_lock<std::mutex> lock(state.mutex);
		state.changed.wait(lock, [&state]() { return state.registrationFinished; });
	}
	if (!state.registered) { state.io.join(); return false; }
	state.running = true;
	return true;
}

bool ResourceIoPipeline::submit(ResourceIoSource *source, ResourceDecodeOperation *operation,
	JobPriority priority, ResourceIoTicket &ticket)
{
	State &state = *m_state;
	ticket = ResourceIoTicket();
	std::lock_guard<std::mutex> lock(state.mutex);
	if (!state.running || !state.onOwner() || source == 0 || operation == 0 || !source->size() ||
		state.requests.size() >= state.config.maximumRequests ||
		source->size() > state.config.inputByteBudget - state.metrics.inputBytes)
	{
		++state.metrics.rejected;
		return false;
	}
	State::RequestPtr request;
	try
	{
		request.reset(new State::Request);
		state.requests.push_back(request);
	}
	catch (...) { ++state.metrics.rejected; return false; }
	request->source = source;
	request->operation = operation;
	request->priority = priority;
	request->ticket.id = ++state.nextId;
	request->ticket.generation = state.generation;
	ticket = request->ticket;
	++state.metrics.accepted;
	++state.metrics.pendingRequests;
	state.metrics.inputBytes += source->size();
	state.metrics.inputHighWater = (std::max)(state.metrics.inputHighWater, state.metrics.inputBytes);
	state.metrics.requestHighWater = (std::max)(state.metrics.requestHighWater, state.metrics.pendingRequests);
	state.changed.notify_all();
	return true;
}

void ResourceIoPipeline::pump()
{
	State &state = *m_state;
	if (!state.running || !state.onOwner()) return;
	// Owner-only container mutation makes this iterator stable while callbacks
	// run outside the lock. Neither the I/O thread nor jobs erase requests.
	for (const State::RequestPtr &request : state.requests)
	{
		std::unique_lock<std::mutex> lock(state.mutex);
		// Publication retains output in submission order. Do not let a later
		// small result reserve the bytes needed by an earlier large request;
		// this also covers priority-reordered I/O whose earlier read is pending.
		if (request->phase == State::QUEUED || request->phase == State::READING) break;
		if (request->phase == State::DECODING && request->decode.isComplete())
		{
			// A cancelled shared group may retire a queued job without execute.
			state.finish(*request, request->decode.wasCancelled() ? RESOURCE_IO_CANCELLED : RESOURCE_IO_DECODE_FAILED);
		}
		if (request->phase != State::READ_READY) continue;
		if (request->cancelled.load() || state.group.wasCancelled())
		{
			request->cancelled.store(true);
			state.finish(*request, RESOURCE_IO_CANCELLED);
			continue;
		}
		size_t workspace = request->requiredWorkspace;
		bool prepared = request->headerPrepared;
		if (!prepared)
		{
			lock.unlock();
			try { prepared = request->operation->prepare(request->bytes.data(), request->bytes.size(), workspace); }
			catch (...) {}
			lock.lock();
			request->requiredWorkspace = workspace;
			request->headerPrepared = prepared;
		}
		if (request->cancelled.load()) { state.finish(*request, RESOURCE_IO_CANCELLED); continue; }
		if (!prepared || workspace > state.config.decodeByteBudget)
		{
			state.finish(*request, RESOURCE_IO_DECODE_FAILED);
			continue;
		}
		if (workspace > state.config.decodeByteBudget - state.metrics.decodeBytes)
		{
			++state.metrics.decodeBudgetStalls;
			break;
		}
		request->workspace = workspace;
		state.metrics.decodeBytes += workspace;
		state.metrics.decodeHighWater = (std::max)(state.metrics.decodeHighWater, state.metrics.decodeBytes);
		request->phase = State::DECODING;
		lock.unlock();
		State::DecodeJob *job = 0;
		try { job = new State::DecodeJob(&state, request); } catch (...) {}
		lock.lock();
		request->decodeJob = job;
		if (job != 0 && JobSystem::instance().isRunning() && state.group.isValid())
			request->decode = JobSystem::instance().trySubmit(job, request->priority, state.group);
		const bool submitted = request->decode.isValid();
		if (!submitted) request->decodeJob = 0;
		lock.unlock();
		if (!submitted)
		{
			delete job;
			{ std::lock_guard<std::mutex> metricLock(state.mutex); ++state.metrics.serialFallbacks; }
			JobSystem::instance().recordSerialFallback();
			state.decodeRequest(request);
		}
	}
}

bool ResourceIoPipeline::promote(const ResourceIoTicket &ticket)
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	State::RequestPtr request = m_state->find(ticket);
	if (!request) return false;
	request->priority = JOB_PRIORITY_FRAME_CRITICAL;
	if (request->decodeJob != 0 && request->decode.isValid() && !request->decode.isComplete())
		JobSystem::instance().tryPromote(request->decodeJob, JOB_PRIORITY_FRAME_CRITICAL);
	m_state->changed.notify_all();
	return true;
}
bool ResourceIoPipeline::cancel(const ResourceIoTicket &ticket)
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	State::RequestPtr request = m_state->find(ticket);
	if (!request) return false;
	request->cancelled.store(true, std::memory_order_release);
	if (request->phase == State::QUEUED || request->phase == State::READ_READY)
		m_state->finish(*request, RESOURCE_IO_CANCELLED);
	m_state->changed.notify_all();
	return true;
}
bool ResourceIoPipeline::isComplete(const ResourceIoTicket &ticket) const
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	State::RequestPtr request = m_state->find(ticket);
	return !request || (request->phase == State::DONE &&
		(!request->decode.isValid() || request->decode.isComplete()));
}
bool ResourceIoPipeline::wait(const ResourceIoTicket &ticket)
{
	if (!m_state->onOwner()) return false;
	const Clock::time_point start = Clock::now();
	promote(ticket);
	while (!isComplete(ticket))
	{
		pump();
		JobHandle decode;
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			State::RequestPtr request = m_state->find(ticket);
			if (request) decode = request->decode;
		}
		if (decode.isValid()) JobSystem::instance().wait(decode);
		else
		{
			std::unique_lock<std::mutex> lock(m_state->mutex);
			State::RequestPtr request = m_state->find(ticket);
			if (request && request->phase == State::READ_READY &&
				m_state->metrics.activeDecode == 0 && m_state->metrics.activeIo == 0)
			{
				// A read can finish after pump() checked its phase. READ_READY
				// alone does not prove a budget deadlock: the next pump must
				// still prepare/admit that freshly published input. Only a
				// prepared request actually blocked by retained output needs
				// owner publication. Inspect earlier tickets too, because FIFO
				// admission can leave this ticket's header unprepared.
				for (const State::RequestPtr &candidate : m_state->requests)
				{
					if (candidate->phase == State::READ_READY && candidate->headerPrepared &&
						candidate->requiredWorkspace > m_state->config.decodeByteBudget - m_state->metrics.decodeBytes)
					{
						m_state->metrics.ownerWaitNanoseconds += elapsed(start);
						return false;
					}
				}
			}
			m_state->changed.wait_for(lock, std::chrono::milliseconds(1));
		}
	}
	std::lock_guard<std::mutex> lock(m_state->mutex);
	m_state->metrics.ownerWaitNanoseconds += elapsed(start);
	return true;
}
void ResourceIoPipeline::cancelAndDrain()
{
	if (!m_state->onOwner()) return;
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		for (const State::RequestPtr &request : m_state->requests)
		{
			request->cancelled.store(true);
			if (request->phase == State::QUEUED || request->phase == State::READ_READY)
				m_state->finish(*request, RESOURCE_IO_CANCELLED);
		}
		m_state->changed.notify_all();
	}
	for (const State::RequestPtr &request : m_state->requests) wait(request->ticket);
}
void ResourceIoPipeline::advanceGeneration()
{
	if (!m_state->onOwner()) return;
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		++m_state->generation;
		for (const State::RequestPtr &request : m_state->requests) request->cancelled.store(true);
		m_state->changed.notify_all();
	}
	cancelAndDrain();
}
void ResourceIoPipeline::shutdown()
{
	State &state = *m_state;
	if (!state.running || !state.onOwner()) return;
	advanceGeneration();
	{
		std::lock_guard<std::mutex> lock(state.mutex);
		state.stopping = true;
		state.changed.notify_all();
	}
	state.io.join();
	state.requests.clear();
	state.metrics.pendingRequests = 0;
	state.metrics.inputBytes = state.metrics.decodeBytes = 0;
	state.running = false;
	state.group = JobGroup();
}
bool ResourceIoPipeline::take(const ResourceIoTicket &ticket, ResourceIoStatus &status,
	ResourceDecodeOperation *&operation)
{
	operation = 0;
	State &state = *m_state;
	if (!state.onOwner() || !isComplete(ticket)) return false;
	State::RequestPtr taken;
	ResourceIoSource *releasedSource = 0;
	{
		std::lock_guard<std::mutex> lock(state.mutex);
		for (std::list<State::RequestPtr>::iterator it = state.requests.begin(); it != state.requests.end(); ++it)
		{
			if ((*it)->ticket.id != ticket.id || (*it)->ticket.generation != ticket.generation) continue;
			taken = *it;
			state.requests.erase(it);
			break;
		}
		if (!taken) return false;
		status = taken->ticket.generation == state.generation ? taken->status : RESOURCE_IO_STALE;
		if (taken->cancelled.load() && status == RESOURCE_IO_SUCCEEDED)
		{
			status = RESOURCE_IO_CANCELLED;
			++state.metrics.cancelled;
		}
		if (status == RESOURCE_IO_STALE) ++state.metrics.stale;
		--state.metrics.pendingRequests;
		state.metrics.inputBytes -= taken->source->size();
		state.metrics.decodeBytes -= taken->workspace;
		releasedSource = taken->source;
		taken->source = 0;
		operation = taken->operation;
		taken->operation = 0;
	}
	// The I/O loop can still hold its last shared request reference after
	// publishing READ_READY. Explicit owner retirement keeps handle closure
	// off that thread even if it releases the final bookkeeping reference.
	delete releasedSource;
	return true;
}
bool ResourceIoPipeline::takeNext(ResourceIoTicket &ticket, ResourceIoStatus &status,
	ResourceDecodeOperation *&operation)
{
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		if (m_state->requests.empty()) return false;
		ticket = m_state->requests.front()->ticket;
	}
	return take(ticket, status, operation);
}
bool ResourceIoPipeline::empty() const
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	return m_state->requests.empty();
}
JobMetricCounter ResourceIoPipeline::generation() const
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	return m_state->generation;
}
ResourceIoMetrics ResourceIoPipeline::metrics() const
{
	std::lock_guard<std::mutex> lock(m_state->mutex);
	return m_state->metrics;
}
}
