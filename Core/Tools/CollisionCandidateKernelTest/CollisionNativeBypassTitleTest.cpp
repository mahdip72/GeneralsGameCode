// Each target compiles the entire selected title entry and its real contact
// implementation. These adapters supply game data/owner context only; they do
// not implement capture, classification, preparation, fallback or proof.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Lib/CollisionCandidateKernel.h"
#include "Lib/FrameTimingDiagnostics.h"
#include "Lib/MultiplayerSimulationPolicy.h"
#include "Lib/SimulationExecutionPolicy.h"
#include "../TestSupport/NativeKernelSourceConsumerTest.h"
#include "CollisionNativeBypassLiterals.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <new>
#include <stdio.h>
#include <thread>
#include <vector>

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_job_system_set_test_fault(unsigned, unsigned);
extern "C" void rts_job_system_set_test_pause_mask(unsigned);
extern "C" bool rts_job_system_wait_for_test_pause(unsigned, unsigned);
extern "C" void rts_job_system_release_test_pause(unsigned);
#endif

typedef unsigned UnsignedInt;
typedef int Int;
typedef bool Bool;
typedef float Real;
#ifndef TRUE
#define TRUE true
#define FALSE false
#endif

class PartitionData;
class PartitionContactList;
struct Coord3D { Real x, y, z; };
struct GeometryInfo
{
	Real major = 1, minor = 1;
	UnsignedInt type = 0;
	Bool smallGeometry = FALSE;
	Real getMajorRadius() const { return major; }
	Real getMinorRadius() const { return minor; }
	UnsignedInt getGeomType() const { return type; }
	Bool getIsSmall() const { return smallGeometry; }
};
struct Object
{
	UnsignedInt id;
	PartitionData *partition = nullptr;
	Coord3D position = {0, 0, 0};
	GeometryInfo geometry;
	Real orientation = 0;
	explicit Object(UnsignedInt value) : id(value) {}
	UnsignedInt getID() const { return id; }
	const Coord3D *getPosition() const { return &position; }
	const GeometryInfo &getGeometryInfo() const { return geometry; }
	Real getOrientation() const { return orientation; }
	PartitionData *friend_getPartitionData() const { return partition; }
};
struct CellAndObjectIntersection;
struct PartitionCell
{
	Int count = 0;
	CellAndObjectIntersection *first = nullptr;
	Int getCoiCount() const { return count; }
	CellAndObjectIntersection *getFirstCoiInCell() const { return first; }
};
struct CellAndObjectIntersection
{
	PartitionCell *cell = nullptr;
	PartitionData *module = nullptr;
	CellAndObjectIntersection *next = nullptr;
	PartitionCell *getCell() const { return cell; }
	PartitionData *getModule() const { return module; }
	CellAndObjectIntersection *getNextCoi() const { return next; }
};
class PartitionData
{
public:
	explicit PartitionData(Object *object) : m_object(object)
	{ if (object != nullptr) object->partition = this; }
	Object *getObject() const { return m_object; }
	void addPossibleCollisions(PartitionContactList *, UnsignedInt ownerOrdinal);
	CellAndObjectIntersection *m_coiArray = nullptr;
	Int m_coiInUseCount = 0;
private:
	Object *m_object;
};
struct NativeTitleGameLogic
{
	unsigned frame = 1;
	rts::JobMetricCounter attemptOrdinal = 0;
	bool resolveObjects = true;
	std::vector<Object *> objects;
	unsigned getFrame() const { return frame; }
	bool isInMultiplayerGame() const { return false; }
	Object *findObjectByID(unsigned id) const
	{
		if (!resolveObjects) return nullptr;
		for (Object *object : objects) if (object->getID() == id) return object;
		return nullptr;
	}
	rts::performance::KernelPerformanceAttempt beginPerformanceReceiptAttempt(
		unsigned kind, unsigned subtype)
	{
		using namespace rts::performance;
		const KernelPerformanceAttemptIdentity identity =
			{kind, subtype, 1, ++attemptOrdinal, KERNEL_PHASE_SPATIAL_WORK, frame};
		return KernelPerformanceReferenceLedger::instance().beginAttempt(identity);
	}
};
static NativeTitleGameLogic *TheGameLogic = nullptr;
struct NativeTitleNetwork
{
	bool isMultiplayerSimulationKernelEnabled(rts::MultiplayerSimulationKernel) const
	{ return false; }
};
static NativeTitleNetwork *TheNetwork = nullptr;
#include "CollisionNativeBypassTitlePolicy.inc"

// Only the allocation backend is adapted. The actual extracted add method
// performs its own hash lookup, duplicate rejection, links and return value.
class MemoryPoolObject {};
static std::vector<void *> s_nativeNodes;
template<class T> static T *allocateNativeTitleNode()
{
	T *value = new T;
	s_nativeNodes.push_back(value);
	return value;
}
template<class T> static void deleteNativeTitleNode(T *value)
{
	const auto found = std::find(s_nativeNodes.begin(), s_nativeNodes.end(), value);
	if (found != s_nativeNodes.end()) s_nativeNodes.erase(found);
	delete value;
}
#define MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(type, name) public: type() = default; ~type();
#define newInstance(type) allocateNativeTitleNode<type>()
#define deleteInstance(value) deleteNativeTitleNode(value)
#include "CollisionNativeBypassTitleSource.inc"
#undef deleteInstance
#undef newInstance
#undef MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE

namespace
{
using namespace rts::performance;
unsigned s_failures = 0;
void requireNative(bool condition, const char *message)
{
	if (!condition)
	{
		++s_failures;
		fprintf(stderr, "%s NATIVE_BYPASS FAIL: %s\n", COLLISION_PERFORMANCE_TITLE, message);
	}
}

enum NativeCase { TrueZero, OwnerOnly, InsertAndDuplicate, InsufficientSpread, GrowthThenReuse };
struct NativeExpected
{
	unsigned occupants, encounters, samples, queries, contacts, inserted, duplicates, calls;
	bool warm;
};
const NativeExpected expected[] = {
	{0, 0, 0, 0, 0, 0, 0, 1, false},
	{1, 0, 0, 0, 0, 0, 0, 1, true},
	{4, 3, 3, 0, 3, 2, 1, 1, true},
	{256, 255, 64, 1, 255, 1, 254, 1, true},
	{1, 0, 0, 0, 0, 0, 0, 2, false}
};

struct NativeFixture
{
	Object ownerObject{10}, firstObject{20}, secondObject{30};
	PartitionData owner{&ownerObject}, first{&firstObject}, second{&secondObject};
	PartitionCell cell;
	CellAndObjectIntersection ownerCell;
	std::vector<CellAndObjectIntersection> entries;
	PartitionContactList contacts;
	explicit NativeFixture(NativeCase kind) : entries(expected[kind].occupants)
	{
		if (entries.empty()) return;
		owner.m_coiArray = &ownerCell; owner.m_coiInUseCount = 1;
		ownerCell.cell = &cell; cell.count = static_cast<Int>(entries.size());
		cell.first = entries.data();
		for (unsigned i = 0; i != entries.size(); ++i)
		{
			entries[i].cell = &cell;
			entries[i].module = i == 0 ? &owner : kind == InsertAndDuplicate && i == 3 ? &second : &first;
			entries[i].next = i + 1 == entries.size() ? nullptr : &entries[i + 1];
		}
	}
};
struct NativeEvent
{
	PartitionCollisionNativeTestEvent event;
	unsigned first, second, third, fourth;
	bool result;
};
struct NativeObservation
{
	rts_test::NativeKernelClock *clock;
	std::vector<NativeEvent> events;
	static void observe(void *context, PartitionCollisionNativeTestEvent event,
		unsigned first, unsigned second, unsigned third, unsigned fourth, bool result)
	{
		auto &self = *static_cast<NativeObservation *>(context);
		self.events.push_back({event, first, second, third, fourth, result});
		++self.clock->now;
	}
	std::vector<NativeEvent> matching(PartitionCollisionNativeTestEvent event) const
	{
		std::vector<NativeEvent> result;
		for (const auto &value : events) if (value.event == event) result.push_back(value);
		return result;
	}
};

// Run/phase/clock setup only. The native title owns its timing batch and all
// attempts, decisions, capture, fallback and terminal calls.
struct NativeTitleRun
{
	rts_test::NativeKernelClock clock;
	KernelPerformanceReferenceLedger &reference = KernelPerformanceReferenceLedger::instance();
	KernelPerformanceLedger &timing = KernelPerformanceLedger::instance();
	KernelPerformanceFrame frame;
	KernelPerformanceSnapshot timingSnapshot;
	bool baseline = false, started = false;
	~NativeTitleRun() { if (started) timingSnapshot = timing.freeze(); }
	bool begin(rts_test::NativeKernelTrace &trace, bool consume)
	{
		baseline = consume;
		auto options = consume ? trace.consumerOptions() : trace.options;
		options.clock = rts_test::NativeKernelClock::read; options.clockContext = &clock;
		KernelPerformanceTimingRunOptions timingOptions;
		timingOptions.enabled = true;
		timingOptions.role = consume ? KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE : KERNEL_PERFORMANCE_PIPELINE;
		timingOptions.clock = rts_test::NativeKernelClock::read; timingOptions.clockContext = &clock;
		if (!timing.beginRun(timingOptions)) return false;
		started = true;
		if (!reference.beginRun(options)) return false;
		if (!consume) return true;
		frame = timing.beginFrame(1, 1, rts_test::NativeKernelSchedulerBoundary());
		if (!frame.valid()) return false;
		for (unsigned i = 0; i <= KERNEL_PHASE_SPATIAL_WORK; ++i)
		{
			const auto phase = static_cast<KernelPerformancePhase>(i); ++clock.now;
			if (!timing.beginPhase(frame, phase)) return false;
			if (phase != KERNEL_PHASE_SPATIAL_WORK && !timing.endPhase(frame, phase)) return false;
		}
		return true;
	}
	bool close(const KernelPerformanceSchedulerBoundary &actual)
	{
		if (baseline)
		{
			++clock.now;
			if (!timing.endPhase(frame, KERNEL_PHASE_SPATIAL_WORK)) return false;
			for (unsigned i = KERNEL_PHASE_SPATIAL_WORK + 1; i != KERNEL_PHASE_COUNT; ++i)
			{
				const auto phase = static_cast<KernelPerformancePhase>(i); ++clock.now;
				if (!timing.beginPhase(frame, phase) || !timing.endPhase(frame, phase)) return false;
			}
			++clock.now;
			if (!timing.endFrame(frame, 1, actual) || !timing.sealAdmissions() ||
				!timing.sealExecutionClosure(actual)) return false;
		}
		else if (!timing.sealAdmissions()) return false;
		timingSnapshot = timing.freeze();
		return timingSnapshot.complete;
	}
};

void resetFixtureWorkspace()
{
	// Fixture-only lifetime management of the real native workspace. There is
	// no production reset/capacity setter and no work may retain these arrays.
	auto &workspace = livePartitionCollisionWorkspace();
	workspace.~LivePartitionCollisionWorkspace();
	new (&workspace) LivePartitionCollisionWorkspace();
}

bool runNativeRole(rts_test::NativeKernelTrace &trace, bool baseline, NativeCase kind, void *fixtureStorage)
{
	const unsigned before = s_failures;
	printf("B_NATIVE_BYPASS %s BEGIN role=%s case=%u\n", COLLISION_PERFORMANCE_TITLE,
		baseline ? "consumer" : "source", static_cast<unsigned>(kind));
	const NativeExpected &want = expected[kind];
	auto &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2; config.queueCapacity = 16;
	config.scratchBytesPerWorker = 4096; config.pinWorkers = false;
	if (!jobs.start(config) || !jobs.registerCurrentThread(rts::JOB_OWNER_GAME))
	{
		requireNative(false, "explicit two-worker owner fixture starts");
		jobs.shutdown(); jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME); return false;
	}
	requireNative(rts::SetSimulationExecutionMode(rts::SIMULATION_EXECUTION_PARALLEL),
		"actual native product policy requests parallel preparation");
	resetFixtureWorkspace();
	if (want.warm)
		requireNative(livePartitionCollisionWorkspace().reserve(1, want.occupants),
			"actual workspace reserve establishes reusable storage before observation");
	NativeTitleRun run;
	const bool opened = run.begin(trace, baseline);
	requireNative(opened, "actual owner ledger opens the selected immutable source before native entry");
	if (!opened)
	{
		resetFixtureWorkspace(); jobs.shutdown(); jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME);
		return false;
	}
	NativeObservation observation{&run.clock};
	PartitionCollisionNativeTestHooks hooks = {NativeObservation::observe, &observation};
	NativeTitleGameLogic game;
	{
		struct FixtureLifetime
		{
			NativeFixture *value;
			~FixtureLifetime() { value->~NativeFixture(); }
		} lifetime = {new (fixtureStorage) NativeFixture(kind)};
		NativeFixture &fixture = *lifetime.value;
		game.objects = {&fixture.ownerObject, &fixture.firstObject, &fixture.secondObject};
		TheGameLogic = &game;
		s_partitionCollisionNativeTestHooks = &hooks;
		for (unsigned call = 0; call != want.calls; ++call)
		{
			run.clock.now.fetch_add(5);
			fixture.owner.addPossibleCollisions(&fixture.contacts, 0);
			run.clock.now.fetch_add(5);
		}
		s_partitionCollisionNativeTestHooks = nullptr;
		const auto reserveBegin = observation.matching(PARTITION_COLLISION_TEST_RESERVE_BEGIN);
		const auto reserveEnd = observation.matching(PARTITION_COLLISION_TEST_RESERVE_END);
		const auto captures = observation.matching(PARTITION_COLLISION_TEST_CAPTURE_COMPLETE);
		const auto queries = observation.matching(PARTITION_COLLISION_TEST_SPREAD_QUERY);
		const auto classes = observation.matching(PARTITION_COLLISION_TEST_CLASSIFIED);
		const auto starts = observation.matching(PARTITION_COLLISION_TEST_FALLBACK_BEGIN);
		const auto contacts = observation.matching(PARTITION_COLLISION_TEST_FALLBACK_CONTACT);
		const auto finishes = observation.matching(PARTITION_COLLISION_TEST_FALLBACK_END);
		requireNative(reserveBegin.size() == want.calls && reserveEnd.size() == want.calls,
			"every actual native attempt executes its real reserve once");
		for (unsigned i = 0; i < reserveBegin.size() && i < reserveEnd.size(); ++i)
		{
			const unsigned beforeCells = want.warm || (kind == GrowthThenReuse && i != 0) ? 1 : 0;
			const unsigned beforeOccupants = want.warm ? want.occupants : kind == GrowthThenReuse && i != 0 ? 1 : 0;
			requireNative(reserveBegin[i].first == (want.occupants ? 1U : 0U) &&
				reserveBegin[i].second == want.occupants && reserveBegin[i].third == beforeCells &&
				reserveBegin[i].fourth == beforeOccupants && reserveEnd[i].result &&
				reserveEnd[i].third == (want.occupants ? 1U : 0U) && reserveEnd[i].fourth == want.occupants,
				"reserve controls report actual reuse or actual first growth, never a requested classification");
		}
		requireNative(captures.size() == want.calls && classes.size() == want.calls &&
			queries.size() == want.queries && starts.size() == want.calls && finishes.size() == want.calls,
			"real capture/classification/short-circuit/fallback control counts match literals");
		for (const auto &capture : captures)
			requireNative(capture.first == want.occupants && capture.second == (want.occupants ? 1U : 0U) &&
				capture.third == want.encounters && capture.fourth == want.samples && capture.result,
				"capture binds the actual empty or populated live cell span and sampler observations");
		for (const auto &decision : classes)
			requireNative(decision.first == (kind == InsufficientSpread ? 2U : 1U) &&
				decision.second == want.occupants && !decision.result,
				"the actual native below-threshold or spread-rejection branch was selected");
		for (const auto &query : queries)
			requireNative(query.first == 255 && query.second == 64 && !query.result,
				"insufficient spread is the one actual reservoir query, not a supplied Boolean");
		requireNative(contacts.size() == want.contacts, "actual addToContactList call count matches native fallback");
		unsigned inserted = 0, duplicate = 0;
		for (unsigned i = 0; i != contacts.size(); ++i)
		{
			const unsigned participant = kind == InsertAndDuplicate && i == 2 ? 30 : 20;
			const bool result = i == 0 || (kind == InsertAndDuplicate && i == 2);
			requireNative(contacts[i].first == 10 && contacts[i].second == participant && contacts[i].result == result,
				"real contact insertion and duplicate-false results preserve literal call order");
			if (contacts[i].result) ++inserted; else ++duplicate;
		}
		requireNative(inserted == want.inserted && duplicate == want.duplicates && s_nativeNodes.size() == want.inserted,
			"actual contact hash/list inserts only the distinct native pairs");
		for (const auto &finish : finishes)
			requireNative(finish.first == want.contacts && finish.second == want.inserted && finish.result,
				"actual fallback terminal totals are independently literal");
		requireNative(observation.matching(PARTITION_COLLISION_TEST_PREPARE_BEGIN).empty(),
			"a genuine rejected native slice never enters core normalization or dispatch");
		if (want.inserted)
			requireNative(fixture.contacts.containsContact(&fixture.owner, &fixture.first), "actual first contact remains published");
		if (kind == InsertAndDuplicate)
		{
			requireNative(fixture.contacts.containsContact(&fixture.owner, &fixture.second), "actual second distinct contact remains published");
			if (s_nativeNodes.size() == 2)
			{
				const auto *last = static_cast<const PartitionContactListNode *>(s_nativeNodes[1]);
				requireNative(last->m_other == &fixture.second && last->m_next == s_nativeNodes[0],
					"actual list prepends B after A without a detached expected executor");
			}
		}
	}
	TheGameLogic = nullptr;
	requireNative(s_nativeNodes.empty(), "actual contact-list destructor reclaims every fixture node");
	const auto scheduler = rts_test::NativeKernelSchedulerBoundary();
	requireNative(scheduler.submittedJobs == 0 && scheduler.executedJobs == 0 && scheduler.ownerHelpJobs == 0 &&
		scheduler.outstandingJobs == 0 && scheduler.pendingJobs == 0,
		"native bypass has no physical job, outstanding group or owner completion in either role");
	resetFixtureWorkspace();
	const bool sealed = run.reference.sealObservationWindow() && run.reference.sealExecutionClosure();
	const auto snapshot = run.reference.freeze();
	const bool timingClosed = run.close(scheduler);
	requireNative(timingClosed && run.timingSnapshot.errors == 0 && run.timingSnapshot.streamCount == 1 &&
		run.timingSnapshot.streams[0].attemptedBatches == want.calls &&
		run.timingSnapshot.streams[0].admittedBatches == 0 &&
		run.timingSnapshot.streams[0].stageSamples[KERNEL_PERFORMANCE_CAPTURE] == 2 * want.calls,
		"actual title timing records both capture scopes and real not-admitted batch closure");
	if (baseline && timingClosed)
		requireNative(run.timingSnapshot.phaseAccounting.phases[KERNEL_PHASE_SPATIAL_WORK].pureNanoseconds == 0 &&
			run.timingSnapshot.phaseAccounting.phases[KERNEL_PHASE_SPATIAL_WORK].serialNanoseconds > 0,
			"native fallback and observation time stay serial with no fabricated pure body");
	const bool controls = s_failures == before;
	const bool closure = sealed && snapshot.trace.complete && snapshot.trace.errors == 0 &&
		snapshot.errors == 0 && !snapshot.complete && snapshot.streamCount == 0 &&
		snapshot.trace.attemptCount == want.calls && snapshot.trace.capturedAttemptCount == want.calls &&
		snapshot.trace.capturedOperationCount == want.calls * want.occupants &&
		snapshot.trace.notAdmittedAttemptCount == want.calls && snapshot.trace.reapCount == want.calls &&
		snapshot.trace.admittedAttemptCount == 0 && snapshot.trace.dispatchCount == 0 && snapshot.trace.rangeCount == 0 &&
		snapshot.trace.coalescedSpanCount == 1 && snapshot.trace.coalescedAttemptCount == 1;
	requireNative(closure, "native title supplies authentic capture/classification/fallback finish/reap and bounded encoding");
	if (closure && kind != GrowthThenReuse)
		requireNative(snapshot.trace.recordCount == 5 && snapshot.trace.logicalEventCount == 13,
			"one native nine-event bypass has literal five-record/thirteen-logical closure");
	if (closure && kind == GrowthThenReuse)
		requireNative(snapshot.trace.recordCount == 14 && snapshot.trace.logicalEventCount == 22,
			"real growth retains nine full records before one nine-event reuse span");
	if (!baseline)
	{
		trace.source = snapshot;
		if (closure && (kind == OwnerOnly || kind == TrueZero))
		{
			const unsigned char *literal = kind == OwnerOnly ? kNativeOwnerOnlyEnvelope : kNativeZeroEnvelope;
			requireNative(trace.bytes.size() >= 399 + 241 && std::memcmp(trace.bytes.data() + 399, literal, 241) == 0,
				"actual native capture/contact semantics match the independent fixed envelope golden");
		}
		if (closure && kind == GrowthThenReuse)
		{
			// Full native attempt:97 begin +5*208 decisions +118 capture
			// +167 restricted fallback finish +101 reap =1523 bytes.
			const unsigned char fullBegin[] = {1, 1, 0, 0, 0, 3, 0, 0, 0};
			const unsigned char laterSpan[] = {1, 1, 0, 0, 0, 12, 0, 0, 0};
			requireNative(trace.bytes.size() >= 399 + 1523 + 241 &&
				std::memcmp(trace.bytes.data() + 399, fullBegin, sizeof(fullBegin)) == 0 &&
				std::memcmp(trace.bytes.data() + 399 + 1523, laterSpan, sizeof(laterSpan)) == 0,
				"actual growth is full first; only the subsequent genuine capacity reuse coalesces");
		}
	}
	printf("B_NATIVE_BYPASS %s END role=%s case=%u controls=%u source_closure=%u failures=%u\n",
		COLLISION_PERFORMANCE_TITLE, baseline ? "consumer" : "source", static_cast<unsigned>(kind),
		static_cast<unsigned>(controls), static_cast<unsigned>(closure), s_failures - before);
	jobs.shutdown(); jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME);
	return controls && closure;
}
// Additive native continuation cases exercise the actual title extraction
// after the existing compact bypass controls.
enum NativeFullCase
{
	NativeRetainedAbort = 5,
	NativeSuccessfulCommit = 6,
	NativeValidatedOwnerAbort = 7
};

struct NativeFullFixture
{
	Object ownerObject{10};
	PartitionData owner{&ownerObject};
	std::vector<Object> objects;
	std::vector<PartitionData> participants;
	std::vector<CellAndObjectIntersection> entries;
	PartitionCell cell;
	CellAndObjectIntersection ownerCell, ownerOnlyEntry;
	PartitionContactList contacts;
	NativeFullFixture() : entries(1536)
	{
		objects.reserve(1535);
		for (unsigned id = 20; id != 1555; ++id) objects.emplace_back(id);
		participants.reserve(1535);
		for (Object &object : objects) participants.emplace_back(&object);
		owner.m_coiArray = &ownerCell; owner.m_coiInUseCount = 1;
		ownerCell.cell = &cell;
		ownerOnlyEntry.cell = &cell; ownerOnlyEntry.module = &owner;
		for (unsigned i = 0; i != 1536; ++i)
		{
			entries[i].cell = &cell;
			entries[i].module = i == 0 ? &owner : &participants[i - 1];
			entries[i].next = i == 1535 ? nullptr : &entries[i + 1];
		}
	}
	void selectOwnerOnly(bool ownerOnly)
	{
		cell.count = ownerOnly ? 1 : 1536;
		cell.first = ownerOnly ? &ownerOnlyEntry : entries.data();
	}
};

struct NativeFullObservation
{
	rts_test::NativeKernelClock &clock;
	NativeObservation native[3];
	unsigned call = 0;
	std::atomic<unsigned> entered[8], normalized[8], sorted[8], finishes[8], completed[8], units[8], released[8];
	std::atomic<unsigned> wrongIdentity{0}, ownerValidation{0}, ownerReduction{0}, publication{0};
	bool releaseBeforePrepareReturn = false, drainedBeforeFallback = false;
	bool drainedBeforeNativeValidation = false, drainedBeforeCommit = false;
	unsigned releasedAtPrepareReturn = 0;
	NativeTitleGameLogic *game = nullptr;
	bool rejectLiveOwner = false;
	explicit NativeFullObservation(rts_test::NativeKernelClock &value) : clock(value)
	{
		for (auto &record : native) record.clock = &clock;
		for (unsigned i = 0; i != 8; ++i)
		{
			entered[i] = normalized[i] = sorted[i] = finishes[i] = completed[i] = units[i] = released[i] = 0;
		}
	}
	unsigned totalReleased() const
	{
		unsigned count = 0;
		for (const auto &value : released) count += value.load();
		return count;
	}
	static void observeNative(void *context, PartitionCollisionNativeTestEvent event,
		unsigned first, unsigned second, unsigned third, unsigned fourth, bool result)
	{
		auto &self = *static_cast<NativeFullObservation *>(context);
		NativeObservation::observe(&self.native[self.call], event, first, second, third, fourth, result);
		if (self.call != 1) return;
		const auto boundary = rts_test::NativeKernelSchedulerBoundary();
		const bool drained = boundary.outstandingJobs == 0 && boundary.pendingJobs == 0;
		if (event == PARTITION_COLLISION_TEST_PREPARE_END)
		{
			self.releasedAtPrepareReturn = self.totalReleased();
			self.releaseBeforePrepareReturn = self.releasedAtPrepareReturn != 0 && drained;
			if (self.rejectLiveOwner && self.game != nullptr)
				self.game->resolveObjects = false;
		}
		if (event == PARTITION_COLLISION_TEST_FALLBACK_BEGIN) self.drainedBeforeFallback = drained;
		if (event == PARTITION_COLLISION_TEST_LIVE_VALIDATION) self.drainedBeforeNativeValidation = drained;
		if (event == PARTITION_COLLISION_TEST_AUTHORITATIVE_COMMIT_END) self.drainedBeforeCommit = drained;
	}
	static void observeCore(void *context, rts::CollisionCandidateTestEvent event, unsigned range,
		unsigned begin, unsigned end, unsigned work, bool complete, rts::CollisionCandidate *storage)
	{
		auto &self = *static_cast<NativeFullObservation *>(context);
		if (event == rts::COLLISION_CANDIDATE_TEST_RANGE_ENTERED ||
			event == rts::COLLISION_CANDIDATE_TEST_PARTITION_NORMALIZED ||
			event == rts::COLLISION_CANDIDATE_TEST_LOCAL_SORT ||
			event == rts::COLLISION_CANDIDATE_TEST_RANGE_FINISHED)
			++self.clock.now;
		if (self.call != 1 || storage != nullptr) ++self.wrongIdentity;
		if (event == rts::COLLISION_CANDIDATE_TEST_OWNER_VALIDATION ||
			event == rts::COLLISION_CANDIDATE_TEST_OWNER_REDUCTION || event == rts::COLLISION_CANDIDATE_TEST_PUBLICATION)
		{
			if (range != 0 || begin != 0 || end != 1536 || !complete ||
				work != (event == rts::COLLISION_CANDIDATE_TEST_PUBLICATION ? 1535U : 1536U)) ++self.wrongIdentity;
			if (event == rts::COLLISION_CANDIDATE_TEST_OWNER_VALIDATION) ++self.ownerValidation;
			if (event == rts::COLLISION_CANDIDATE_TEST_OWNER_REDUCTION) ++self.ownerReduction;
			if (event == rts::COLLISION_CANDIDATE_TEST_PUBLICATION) ++self.publication;
			return;
		}
		if (range >= 8 || begin != 192 * range || end != 192 * (range + 1))
		{ ++self.wrongIdentity; return; }
		if (event == rts::COLLISION_CANDIDATE_TEST_RANGE_ENTERED) ++self.entered[range];
		else if (event == rts::COLLISION_CANDIDATE_TEST_PARTITION_NORMALIZED)
		{
			if (work != self.normalized[range].fetch_add(1)) ++self.wrongIdentity;
		}
		else if (event == rts::COLLISION_CANDIDATE_TEST_LOCAL_SORT)
		{
			if (work != 192) ++self.wrongIdentity;
			++self.sorted[range];
		}
		else if (event == rts::COLLISION_CANDIDATE_TEST_RANGE_FINISHED)
		{
			self.units[range] = work;
			if (complete) ++self.completed[range];
			++self.finishes[range];
		}
		else if (event == rts::COLLISION_CANDIDATE_TEST_RANGE_RELEASED) ++self.released[range];
		else ++self.wrongIdentity;
	}
};

#if defined(RTS_BUILD_CORE_EXTRAS)
// Existing scheduler fault/pause only. This thread cannot execute or cancel
// a collision body, and no test checkpoint is installed on the native path.
class NativeFullPartialSubmission
{
public:
	NativeFullPartialSubmission(NativeFullObservation &observed, bool enabled) : m_enabled(enabled)
	{
		if (!enabled) return;
		rts_job_system_set_test_pause_mask(4);
		rts_job_system_set_test_fault(6, 2);
		try
		{
			m_controller = std::thread([this, &observed]() {
				pauseReached = rts_job_system_wait_for_test_pause(4, 1000);
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
				while (pauseReached && std::chrono::steady_clock::now() < deadline)
				{
					if (observed.finishes[0] == 1 && observed.completed[0] == 1 && observed.units[0] == 192 &&
						rts::JobSystem::instance().outstandingJobCount() == 1)
					{ firstRetired = true; break; }
					std::this_thread::yield();
				}
				rts_job_system_release_test_pause(4);
			});
			controllerStarted = true;
		}
		catch (...)
		{
			rts_job_system_release_test_pause(4);
			rts_job_system_set_test_fault(0, 0);
			rts_job_system_set_test_pause_mask(0);
		}
	}
	~NativeFullPartialSubmission() { finish(); }
	void finish()
	{
		if (!m_enabled) return;
		rts_job_system_release_test_pause(4);
		if (m_controller.joinable()) m_controller.join();
		rts_job_system_set_test_fault(0, 0);
		rts_job_system_set_test_pause_mask(0);
		m_enabled = false;
	}
	bool controllerStarted = false, pauseReached = false, firstRetired = false;
private:
	bool m_enabled;
	std::thread m_controller;
};
#endif

// Read-only typed framing of the ACTUAL completed trace. No expected digest,
// positive proof, parser authority, source alteration or execution is created.
struct NativeFullWireRecord
{
	unsigned kind;
	rts::JobMetricCounter value[30]{};
	bool seen[30]{};
};
bool readNativeFullWire(const std::vector<unsigned char> &bytes, std::vector<NativeFullWireRecord> &records)
{
	// The frozen header is399 bytes and contains repeated digest tags. Its
	// independent literal is already covered; inspect only whole records here.
	if (bytes.size() < 399 || std::memcmp(bytes.data(), "RTS-KERNEL-FIELDS-v1", 20) != 0 ||
		bytes[20] != 1 || bytes[21] != 0x50 || bytes[22] != 0 || bytes[23] != 0) return false;
	std::size_t at = 399;
	while (at != bytes.size())
	{
		if (bytes.size() - at < 5) return false;
		const unsigned type = bytes[at++];
		unsigned tag = 0;
		for (unsigned i = 0; i != 4; ++i) tag |= static_cast<unsigned>(bytes[at++]) << (8 * i);
		const unsigned width = type == 3 ? 8 : type == 5 ? 1 : type == 1 || type == 2 || type == 4 || type == 6 ? 4 : 0;
		if (width == 0 || bytes.size() - at < width || tag >= 30) return false;
		rts::JobMetricCounter value = 0;
		for (unsigned i = 0; i != width; ++i) value |= static_cast<rts::JobMetricCounter>(bytes[at++]) << (8 * i);
		if (tag == 1)
		{
			if (type != 1) return false;
			records.push_back({static_cast<unsigned>(value)});
		}
		if (records.empty() || records.back().seen[tag]) return false;
		records.back().seen[tag] = true; records.back().value[tag] = value;
	}
	return !records.empty();
}

void requireNativeFullWire(const rts_test::NativeKernelTrace &trace,
	bool partialAbort, bool validatedOwnerAbort)
{
	const bool abort = partialAbort || validatedOwnerAbort;
	std::vector<NativeFullWireRecord> records;
	const bool framed = readNativeFullWire(trace.bytes, records);
	requireNative(framed, "full native source trace has independently readable typed record framing");
	if (!framed) return;
	unsigned spans = 0, begin = 0, decisions = 0, captures = 0, dispatches = 0, plans = 0, releases = 0, finish = 0, reap = 0;
	const unsigned wantedRanges = partialAbort ? 1 : 8;
	for (const auto &record : records)
	{
		const auto &v = record.value;
		if (record.kind == 12)
		{
			++spans;
			requireNative(v[14] == 1 && v[15] == 9 && v[12] == (spans == 1 ? 1U : 3U),
				"full middle attempt splits the two authentic one-attempt bypass spans");
			if (spans == 2) requireNative(finish == 1 && reap == 1,
				"the second compact span follows actual full terminal and reap");
			continue;
		}
		if (record.kind < 3 || record.kind > 10) continue;
		requireNative(v[3] == 2, "every full record belongs to the same authentic middle attempt serial2");
		if (record.kind == 3) { ++begin; requireNative(spans == 1, "full begin follows the first completed span"); }
		if (record.kind == 4)
		{
			requireNative(v[4] == decisions && v[5] == (decisions < 5 ? decisions + 1 : 1),
				"actual native decision ordinals0..4 precede same-attempt core ordinal5");
			if (decisions == 4)
				requireNative(v[6] == 0x5002 && v[7] == 3 && v[8] == 1 && v[9] == KERNEL_ADMISSION_NOT_REQUESTED,
					"actual sufficient-spread decision is full class3, not a fallback profile");
			if (decisions == 5)
				requireNative(v[6] == 1 && v[7] == (partialAbort ? 3U : 1U) && v[9] == KERNEL_ADMISSION_ACCEPTED,
					"actual core acceptance retains its real late cleanup cause without a fake poll");
			++decisions;
		}
		if (record.kind == 5)
		{
			++captures;
			requireNative(decisions == 5 && v[4] == 1 && v[5] == 1536,
				"core binds captured schema1 exactly once after the five native decisions");
		}
		if (record.kind == 6)
		{
			++dispatches;
			requireNative(v[4] == 1 && v[7] == wantedRanges && v[8] == 192 * wantedRanges && v[9] == 128,
				"dispatch contains only actual admitted default-grain native ranges");
		}
		if (record.kind == 7 || record.kind == 8)
		{
			unsigned &ordinal = record.kind == 7 ? plans : releases;
			requireNative(v[4] == 1 && v[5] == ordinal && v[6] == 2 && v[7] == 192 * ordinal &&
				v[8] == 192 * (ordinal + 1) && v[9] == 192,
				"actual partition range identity and admitted192-unit bounds are literal");
			if (record.kind == 8)
				requireNative(v[10] == 1 && v[11] == 0 && v[12] == 4 && v[13] == 0 && v[14] == 192 &&
					v[18] == 5 && v[19] == 192 && v[20] == 192 * (ordinal + 1) && v[21] == KERNEL_RANGE_COMPLETED &&
					v[22] == (partialAbort ? KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL : KERNEL_PUBLICATION_PUBLISHED),
					"real completed range has four false polls and the actual owner publication disposition");
			++ordinal;
		}
		if (record.kind == 9)
		{
			++finish;
			requireNative(releases == wantedRanges && v[4] == (abort ? KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION : KERNEL_PERFORMANCE_COMMITTED) &&
				v[7] == static_cast<unsigned>(abort) && v[8] == static_cast<unsigned>(abort) &&
				v[9] == static_cast<unsigned>(!partialAbort),
				"actual terminal follows all releases and records real fallback versus commit");
			if (!partialAbort) requireNative(v[10] == 1 && v[11] == 1536 && v[12] != 0 &&
				v[18] == static_cast<unsigned>(!abort),
				"validated finish links exactly one real token and its committed or discarded disposition");
		}
		if (record.kind == 10) { ++reap; requireNative(finish == 1, "native reap follows actual terminal closure"); }
	}
	requireNative(spans == 2 && begin == 1 && decisions == 6 && captures == 1 && dispatches == 1 &&
		plans == wantedRanges && releases == wantedRanges && finish == 1 && reap == 1,
		"actual full trace has one continuation, one capture and one terminal between two spans");
}

bool runNativeFullRole(rts_test::NativeKernelTrace &trace, bool baseline, NativeFullCase kind, void *fixtureStorage)
{
	const bool partialAbort = kind == NativeRetainedAbort;
	const bool validatedOwnerAbort = kind == NativeValidatedOwnerAbort;
	const bool abort = partialAbort || validatedOwnerAbort;
	const unsigned before = s_failures, wantedBodies = partialAbort ? 1 : 8;
	printf("B_NATIVE_FULL %s BEGIN role=%s case=%u\n", COLLISION_PERFORMANCE_TITLE,
		baseline ? "consumer" : "source", static_cast<unsigned>(kind));
	auto &jobs = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2; config.queueCapacity = 16; config.scratchBytesPerWorker = 4096; config.pinWorkers = false;
	const bool schedulerStarted = jobs.start(config) && jobs.registerCurrentThread(rts::JOB_OWNER_GAME);
	requireNative(schedulerStarted, "full native fixture starts its explicit two-worker owner scheduler");
	if (!schedulerStarted) { jobs.shutdown(); jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME); return false; }
	requireNative(rts::SetSimulationExecutionMode(rts::SIMULATION_EXECUTION_PARALLEL), "real product policy selects native parallel preparation");
	resetFixtureWorkspace();
	requireNative(livePartitionCollisionWorkspace().reserve(1, 1536), "full native fixture warms actual capacity without synthetic reuse");
	NativeTitleRun run;
	const bool opened = run.begin(trace, baseline);
	requireNative(opened, "full native owner opens the actual selected source before any title call");
	if (!opened) { resetFixtureWorkspace(); jobs.shutdown(); jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME); return false; }
	NativeTitleGameLogic game;
	NativeFullObservation observation(run.clock);
	observation.game = &game;
	observation.rejectLiveOwner = validatedOwnerAbort;
	PartitionCollisionNativeTestHooks hooks = {NativeFullObservation::observeNative, &observation, NativeFullObservation::observeCore};
	{
		struct Lifetime { NativeFullFixture *value; ~Lifetime() { value->~NativeFullFixture(); } }
			lifetime = {new (fixtureStorage) NativeFullFixture()};
		auto &fixture = *lifetime.value;
		game.objects.push_back(&fixture.ownerObject);
		for (Object &object : fixture.objects) game.objects.push_back(&object);
		TheGameLogic = &game; s_partitionCollisionNativeTestHooks = &hooks;
		for (unsigned call = 0; call != 3; ++call)
		{
			observation.call = call; fixture.selectOwnerOnly(call != 1);
			run.clock.now.fetch_add(5);
#if defined(RTS_BUILD_CORE_EXTRAS)
			NativeFullPartialSubmission partial(observation,
				!baseline && partialAbort && call == 1);
#endif
			fixture.owner.addPossibleCollisions(&fixture.contacts, 0);
			game.resolveObjects = true;
#if defined(RTS_BUILD_CORE_EXTRAS)
			partial.finish();
			if (!baseline && partialAbort && call == 1)
				requireNative(partial.controllerStarted && partial.pauseReached && partial.firstRetired,
					"actual second-submit failure pauses until real range0 has retired192 units");
#endif
			run.clock.now.fetch_add(5);
		}
		s_partitionCollisionNativeTestHooks = nullptr;
		for (unsigned call = 0; call != 3; ++call)
		{
			const auto &seen = observation.native[call];
			const unsigned count = call == 1 ? 1536 : 1;
			const auto reserves = seen.matching(PARTITION_COLLISION_TEST_RESERVE_END);
			const auto reserveBegins = seen.matching(PARTITION_COLLISION_TEST_RESERVE_BEGIN);
			const auto captures = seen.matching(PARTITION_COLLISION_TEST_CAPTURE_COMPLETE);
			const auto classes = seen.matching(PARTITION_COLLISION_TEST_CLASSIFIED);
			requireNative(reserveBegins.size() == 1 && reserves.size() == 1 && captures.size() == 1 && classes.size() == 1,
				"each actual title call reserves, captures and classifies exactly once");
			if (reserveBegins.size() == 1) requireNative(reserveBegins[0].first == 1 && reserveBegins[0].second == count &&
				reserveBegins[0].third == 1 && reserveBegins[0].fourth == 1536,
				"each native reserve begins with its real already-warmed capacity");
			if (reserves.size() == 1) requireNative(reserves[0].first == 1 && reserves[0].second == count &&
				reserves[0].third == 1 && reserves[0].fourth == 1536 && reserves[0].result,
				"real workspace capacity stays reused through the full interleave");
			if (captures.size() == 1) requireNative(captures[0].first == count && captures[0].second == 1 &&
				captures[0].third == (call == 1 ? 1535U : 0U) && captures[0].fourth == (call == 1 ? 64U : 0U) && captures[0].result,
				"full native capture sees all1536 occupants and real sampler encounters");
			if (classes.size() == 1) requireNative(classes[0].first == (call == 1 ? 3U : 1U) &&
				classes[0].second == count && classes[0].result == (call == 1),
				"actual sufficient spread is full class3 between two genuine below-minimum bypasses");
			const auto queries = seen.matching(PARTITION_COLLISION_TEST_SPREAD_QUERY);
			requireNative(queries.size() == (call == 1 ? 1U : 0U), "only actual full middle call queries spread once");
			if (call == 1 && queries.size() == 1) requireNative(queries[0].first == 1535 && queries[0].second == 64 && queries[0].result,
				"real sampler has useful spread without a supplied classifier result");
			const auto contacts = seen.matching(PARTITION_COLLISION_TEST_FALLBACK_CONTACT);
			const auto fallbackEnds = seen.matching(PARTITION_COLLISION_TEST_FALLBACK_END);
			const unsigned wantedContacts = abort && call == 1 ? 1535 : 0;
			requireNative(contacts.size() == wantedContacts &&
				seen.matching(PARTITION_COLLISION_TEST_FALLBACK_BEGIN).size() == (call != 1 || abort ? 1U : 0U) &&
				fallbackEnds.size() == (call != 1 || abort ? 1U : 0U),
				"real fallback executes once only for the actual selected native fallback path");
			for (unsigned i = 0; i != contacts.size(); ++i)
				requireNative(contacts[i].first == 10 && contacts[i].second == 20 + i && contacts[i].result,
					"actual retained-abort fallback inserts distinct contacts in literal discovery order");
			if (!fallbackEnds.empty()) requireNative(fallbackEnds[0].first == wantedContacts &&
				fallbackEnds[0].second == wantedContacts && fallbackEnds[0].result,
				"native fallback terminal reports actual attempted/inserted totals");
			if (call != 1) requireNative(seen.matching(PARTITION_COLLISION_TEST_PREPARE_BEGIN).empty() &&
				seen.matching(PARTITION_COLLISION_TEST_PREPARE_END).empty(),
				"surrounding owner-only bypasses never enter the full kernel");
		}
		const auto &middle = observation.native[1];
		const auto prepared = middle.matching(PARTITION_COLLISION_TEST_PREPARE_END);
		requireNative(middle.matching(PARTITION_COLLISION_TEST_PREPARE_BEGIN).size() == 1 && prepared.size() == 1,
			"actual full native entry calls and returns from core preparation once");
		if (prepared.size() == 1)
			requireNative(prepared[0].first == static_cast<unsigned>(
				(partialAbort || (baseline && validatedOwnerAbort)) ?
					rts::COLLISION_CANDIDATE_SERIAL_FALLBACK :
					rts::COLLISION_CANDIDATE_PARALLEL) &&
				prepared[0].second == (baseline ? 0U : wantedBodies) &&
				prepared[0].third == (!baseline && !partialAbort ? 8U : 0U) &&
				prepared[0].fourth == (partialAbort || (baseline && validatedOwnerAbort) ? 0U : 1535U),
				"actual result and physical job metrics remain distinct from logical inline body counts");
		const auto validated = middle.matching(PARTITION_COLLISION_TEST_LIVE_VALIDATION);
		const auto committed = middle.matching(PARTITION_COLLISION_TEST_AUTHORITATIVE_COMMIT_END);
		const unsigned wantedLiveValidation = validatedOwnerAbort ? (baseline ? 0U : 1U) :
			(abort ? 0U : 1U);
		requireNative(validated.size() == wantedLiveValidation && committed.size() == (abort ? 0U : 1U),
			"native live validation and authoritative commit occur only for actual success");
		if (validatedOwnerAbort && !baseline && validated.size() == 1)
			requireNative(!validated[0].result && observation.drainedBeforeNativeValidation,
				"validated native output is rejected only after drain when live owner identity changes");
		if (!abort && validated.size() == 1 && committed.size() == 1)
			requireNative(validated[0].result && committed[0].first == 1535 && committed[0].result &&
				observation.drainedBeforeNativeValidation && observation.drainedBeforeCommit,
				"successful native live validation and single1535-contact commit follow real group cleanup");
		requireNative(observation.wrongIdentity == 0 &&
			observation.ownerValidation == (partialAbort ? 0U : 1U) &&
			observation.ownerReduction == (partialAbort ? 0U : 1U) &&
			observation.publication == (partialAbort || (baseline && validatedOwnerAbort) ? 0U : 1U),
			"only actual completed full body admits owner reduction and output publication");
		for (unsigned i = 0; i != 8; ++i)
		{
			const unsigned ran = i < wantedBodies ? 1 : 0;
			requireNative(observation.entered[i] == ran && observation.normalized[i] == 192 * ran && observation.sorted[i] == ran &&
				observation.finishes[i] == ran && observation.completed[i] == ran && observation.units[i] == 192 * ran &&
				observation.released[i] == (!baseline || i < wantedBodies ? 1U : 0U),
				"real source/inline bodies retain literal192-unit bounds; storage cleanup does not invent admission");
		}
		requireNative(observation.releaseBeforePrepareReturn && observation.releasedAtPrepareReturn == (baseline ? wantedBodies : 8U) &&
			(!abort || observation.drainedBeforeFallback), "actual storage and group cleanup precedes native fallback or commit");
		requireNative(s_nativeNodes.size() == 1535, "one actual native publication creates exactly1535 real contact nodes");
		if (s_nativeNodes.size() == 1535)
		{
			for (unsigned i = 0; i != 1535; ++i)
			{
				const auto *node = static_cast<const PartitionContactListNode *>(s_nativeNodes[i]);
				requireNative(node->m_obj == &fixture.owner && node->m_other == &fixture.participants[i] &&
					node->m_next == (i == 0 ? nullptr : s_nativeNodes[i - 1]) &&
					fixture.contacts.containsContact(&fixture.owner, &fixture.participants[i]),
					"actual contact allocation/link order is discovery insertion and reverse final list");
			}
		}
	}
	TheGameLogic = nullptr;
	requireNative(s_nativeNodes.empty(), "actual full native contact destructor releases every source-owned node");
	const auto scheduler = rts_test::NativeKernelSchedulerBoundary();
	requireNative(scheduler.outstandingJobs == 0 && scheduler.pendingJobs == 0 &&
		scheduler.submittedJobs == (baseline ? 0U : wantedBodies) && scheduler.executedJobs == (baseline ? 0U : wantedBodies) &&
		(baseline || partialAbort ? scheduler.ownerHelpJobs == 0 : scheduler.ownerHelpJobs <= 8),
		"only source executes actual physical submissions; baseline has no scheduler work or retained job");
	resetFixtureWorkspace();
	const bool sealed = run.reference.sealObservationWindow() && run.reference.sealExecutionClosure();
	const auto snapshot = run.reference.freeze();
	const bool timingClosed = run.close(scheduler);
	requireNative(timingClosed && run.timingSnapshot.errors == 0 && run.timingSnapshot.streamCount == 1 &&
		run.timingSnapshot.streams[0].attemptedBatches == 3 && run.timingSnapshot.streams[0].admittedBatches == 1 &&
		run.timingSnapshot.streams[0].stageSamples[KERNEL_PERFORMANCE_CAPTURE] == 6 &&
		run.timingSnapshot.streams[0].stageSamples[KERNEL_PERFORMANCE_SCHEDULE] == 1,
		"real native timing closes three batches and one true admission independent of inline physical metrics");
	if (baseline && timingClosed)
	{
		const auto &phase = run.timingSnapshot.phaseAccounting.phases[KERNEL_PHASE_SPATIAL_WORK];
		const unsigned expectedPure = abort ? 0U : 195 * wantedBodies;
		if (phase.pureNanoseconds != expectedPure || phase.serialNanoseconds == 0)
		{
			fprintf(stderr, "%s NATIVE_BYPASS FAIL: committed shared bodies are pure while discarded abort work remains serial (pure=%llu expected=%u serial=%llu)\n",
				COLLISION_PERFORMANCE_TITLE, static_cast<unsigned long long>(phase.pureNanoseconds),
				expectedPure, static_cast<unsigned long long>(phase.serialNanoseconds));
			++s_failures;
		}
	}
	const bool controls = s_failures == before;
	const bool closure = sealed && snapshot.trace.complete && snapshot.trace.errors == 0 && snapshot.errors == 0 &&
		snapshot.trace.attemptCount == 3 && snapshot.trace.capturedAttemptCount == 3 && snapshot.trace.capturedOperationCount == 1538 &&
		snapshot.trace.notAdmittedAttemptCount == 2 && snapshot.trace.admittedAttemptCount == 1 &&
		snapshot.trace.abortedAfterAdmissionAttemptCount == static_cast<unsigned>(abort) && snapshot.trace.reapCount == 3 &&
		snapshot.trace.dispatchCount == 1 && snapshot.trace.rangeCount == wantedBodies && snapshot.trace.releasedRangeCount == wantedBodies &&
		snapshot.trace.coalescedSpanCount == 2 && snapshot.trace.coalescedAttemptCount == 2 && snapshot.trace.residentAttemptCount == 0 &&
		snapshot.trace.residentRangeCount == 0 && snapshot.trace.residentAttemptHighWater == 1 &&
		snapshot.trace.residentRangeHighWater == wantedBodies && snapshot.trace.recordCount == 17 + 2 * wantedBodies &&
		snapshot.trace.logicalEventCount == 33 + 2 * wantedBodies &&
		snapshot.complete == !partialAbort &&
		snapshot.streamCount == (partialAbort ? 0U : 1U);
	requireNative(closure, "same native attempt crosses class3 into real capture/dispatch/body/terminal and reap between two spans");
	if (closure && !partialAbort)
		requireNative(snapshot.streams[0].validatedBatchCount == 1 &&
			snapshot.streams[0].committedBatchCount == static_cast<unsigned>(!abort) &&
			snapshot.streams[0].abortedBatchCount == static_cast<unsigned>(abort) &&
			snapshot.streams[0].validatedOperationCount == 1536 &&
			snapshot.streams[0].committedOperationCount == (abort ? 0U : 1536U),
			"one authentic validated token retains its committed or owner-discarded captured1536 operations");
	if (!baseline)
	{
		trace.source = snapshot;
		if (closure) requireNativeFullWire(trace, partialAbort,
			validatedOwnerAbort);
	}
	printf("B_NATIVE_FULL %s END role=%s case=%u controls=%u source_closure=%u failures=%u\n", COLLISION_PERFORMANCE_TITLE,
		baseline ? "consumer" : "source", static_cast<unsigned>(kind), static_cast<unsigned>(controls),
		static_cast<unsigned>(closure), s_failures - before);
	jobs.shutdown(); jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME);
	return controls && closure && s_failures == before;
}

// Call once inside RunCollisionNativeBypassTitleTests(), before its return.
void runNativeFullCases()
{
	for (unsigned kind = NativeRetainedAbort; kind <= NativeValidatedOwnerAbort; ++kind)
	{
#if !defined(RTS_BUILD_CORE_EXTRAS)
		if (kind == NativeRetainedAbort)
		{
			printf("B_NATIVE_FULL %s UNAVAILABLE case=%u requires RTS_BUILD_CORE_EXTRAS\n", COLLISION_PERFORMANCE_TITLE, kind);
			continue; // Explicitly unavailable, never a retained-abort PASS.
		}
#endif
		rts_test::NativeKernelTrace trace(90 + kind);
		alignas(NativeFullFixture) unsigned char sourceStorage[sizeof(NativeFullFixture)];
		alignas(NativeFullFixture) unsigned char consumerStorage[sizeof(NativeFullFixture)];
		if (runNativeFullRole(trace, false, static_cast<NativeFullCase>(kind), sourceStorage))
			runNativeFullRole(trace, true, static_cast<NativeFullCase>(kind), consumerStorage);
	}
}
}

int RunCollisionNativeBypassTitleTests()
{
	// Breaks caught: missing native proof integration; fake reuse/zero capture;
	// an added below-threshold uniqueness query; copied fallback executor;
	// unbound duplicate-false contacts; premature native lifetime closure.
	for (unsigned kind = TrueZero; kind <= GrowthThenReuse; ++kind)
	{
		rts_test::NativeKernelTrace trace(80 + kind);
		// Distinct raw storage survives, but all source objects, contact nodes,
		// vectors and native snapshots are destroyed before consumer creation.
		alignas(NativeFixture) unsigned char sourceStorage[sizeof(NativeFixture)];
		alignas(NativeFixture) unsigned char consumerStorage[sizeof(NativeFixture)];
		if (runNativeRole(trace, false, static_cast<NativeCase>(kind), sourceStorage))
			runNativeRole(trace, true, static_cast<NativeCase>(kind), consumerStorage);
	}
	runNativeFullCases();
	return s_failures == 0 ? 0 : 1;
}
