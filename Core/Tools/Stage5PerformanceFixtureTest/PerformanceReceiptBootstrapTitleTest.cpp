/*
** Scratch source-connected draft; not a substitute producer or game replay.
** The four startup branches, owner adapter and attempt entry are extracted
** from actual source. Asset loading, display state and input message are the
** controlled external dependencies. No fixture emits a normal control event.
*/
#include "Common/GameThreadOwnership.h"
#include "Common/GameCommon.h"
#include "Common/INI.h"
#include "Common/INIException.h"
#include "Lib/PerformanceReceipt.h"
#include "Lib/SimulationPhaseGraphOwnerAdapter.h"
#include <atomic>
#define private protected
#include "Common/PerformanceReceiptRuntime.h"
#undef private
#include <stdio.h>
#include <string.h>
#include <thread>
#include <vector>
#include "NativeReceiptTestEnvironment.h"

namespace performance_receipt_bootstrap_fixture
{
using namespace ::rts::performance;
unsigned failures = 0;
void Check(bool value, const char *message)
{
	if (!value) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
void Fatal(const char *message)
{
	fprintf(stderr, "PREREQUISITE: unexpected native fatal: %s\n", message);
	exit(2);
}
#undef RELEASE_CRASH
#define RELEASE_CRASH(message) Fatal(message)
#undef deleteInstance
#define deleteInstance(instance) delete (instance)

// External installed lockstep proof is inactive in this receipt-only fixture.
// Do not remove its real verification branch from the generated include.
namespace rts
{
using namespace ::rts;
inline bool IsInstalledLockstepV2ProofStarted() { return false; }
inline bool RecordInstalledLockstepV2Frame(unsigned, unsigned) { Fatal("unexpected lockstep proof"); return false; }
inline bool IsInstalledLockstepV2StopRequested() { return false; }
}
#include "PerformanceReceiptBootstrapGameMode.inc"
struct GlobalData
{
	GlobalData() : m_framesPerSecondLimit(30), m_useFpsLimit(false), m_loadScreenRender(FALSE) {}
	std::string m_mapName;
	int m_framesPerSecondLimit;
	bool m_useFpsLimit;
	Bool m_loadScreenRender;
} globalData;
GlobalData *TheGlobalData = &globalData, *TheWritableGlobalData = &globalData;
struct GameState
{
	void setPristineMapName(const std::string &name) { pristine = name; }
	bool isInSaveDirectory(const std::string &) { return false; }
	std::string pristine;
} gameState;
GameState *TheGameState = &gameState;
struct FramePacer { void setFramesPerSecondLimit(int) { Fatal("three-argument replay changed pacing"); } } framePacer;
FramePacer *TheFramePacer = &framePacer;
struct Display { Display() : movie(false) {} bool isMoviePlaying() const { return movie; } bool movie; } display;
Display *TheDisplay = &display;
struct Engine { void setQuitting(Bool) { Fatal("external proof requested quit"); } } engine;
Engine *TheGameEngine = &engine;
struct LoadScreen { void destroyWindows() {} void init(void *) {} };
struct GameMessage
{
	struct Argument { int integer; } arguments[3];
	GameMessage() { arguments[0].integer = GAME_REPLAY; arguments[1].integer = DIFFICULTY_NORMAL; arguments[2].integer = 7; }
	unsigned getArgumentCount() const { return 3; }
	const Argument *getArgument(unsigned i) const { return &arguments[i]; }
};
class PerformanceReceiptRuntime;
class GameLogic;
GameLogic *TheGameLogic = 0;
enum Fault { NoFault, UndeclaredZero, ConsumeWithoutDeclaration, DuplicateDeclaration,
	WrongOwnerTransition, ForeignTransition, ControlOnly };
class GameLogic
{
public:
	GameLogic(Fault fault = NoFault) :
		m_stage5PhaseGraph(makeStage5PhaseGraphCallbacks(), this), m_stage5PhaseCursor(0),
		m_stage5PhaseNow(0), m_performanceReceiptRuntime(0), m_isInUpdate(FALSE),
		m_frame(83), m_hasUpdated(FALSE), m_startNewGame(FALSE), m_gameMode(GAME_NONE),
		m_background(0), m_loadScreen(0), loading(false), updates(0), loads(0),
		prepares(0), attempts(0), selectedFault(fault) {}
	static rts::LiveSimulationPhaseOwnerCallbacks makeStage5PhaseGraphCallbacks();
	static bool isStage5PhaseGraphOwner(void *);
	static bool validateStage5PhaseGraphCommit(unsigned, unsigned, unsigned, void *);
	static bool commitStage5PhaseGraphPhase(unsigned, unsigned, unsigned, void *);
	static void observeStage5PhaseGraphBoundary(rts::LiveSimulationPhaseObservationBoundary,
		rts::SimulationPhaseId, unsigned, unsigned, void *) noexcept;
	Bool getStage5PhaseAuthorityEvidence(UnsignedInt, rts::LiveSimulationPhaseAuthorityEvidence &) const;
	bool attachPerformanceReceiptRuntime(PerformanceReceiptRuntime *);
	bool detachPerformanceReceiptRuntime(PerformanceReceiptRuntime *);
	KernelPerformanceAttempt beginPerformanceReceiptAttempt(unsigned, unsigned) noexcept;
	bool finishPerformanceReceiptAttempt(KernelPerformanceAttempt attempt,
		KernelPerformanceReferenceBatch validatedBatch,
		KernelPerformanceDisposition disposition, bool fallbackEntered,
		bool fallbackCompleted) noexcept;
	Bool isInGameLogicUpdate() const { return m_isInUpdate; }
	unsigned getFrame() const { return m_frame; }
	bool isInGame() const { return false; }
	bool isClearingGameData() const { return false; }
	bool isLoadingMap() const { return loading; }
	void setLoadingMap(Bool value) { loading = value != FALSE; }
	void prepareNewGame(GameMode mode, GameDifficulty difficulty, int rank)
	{
		++prepares; m_gameMode = mode;
		Check(mode == GAME_REPLAY && difficulty == DIFFICULTY_NORMAL && rank == 7,
			"actual onNewGame decodes the real three-argument replay input");
	}
	bool onNewGame(MAYBE_UNUSED GameMessage *);
	void startNewGame(Bool loadingSave) { tryStartNewGame(loadingSave); }
	void tryStartNewGame(Bool loadingSaveGame);
	LoadScreen *getLoadScreen(Bool) { Fatal("replay entered single-player load screen"); return 0; }
	enum { CRC_RECALC = 1 };
	unsigned getCRC(int) { Fatal("external lockstep proof read CRC"); return 0; }
	void update()
	{
		m_isInUpdate = TRUE;
		m_stage5PhaseCursor = 0;
		Check(m_stage5PhaseGraph.runFrame(m_frame) == rts::LIVE_SIMULATION_PHASE_COMPLETED,
			"actual owner graph executes every real phase during startup");
		m_isInUpdate = FALSE;
		++updates;
	}
	Bool runOwnerIntakePhase(UnsignedInt &now);
	void runLegacyMutableIslandPhase(UnsignedInt) {}
	void runSpatialPhase() {}
	void runOwnerTailPhase() {}
	void runVerificationAndPublicationPhase()
	{
#include "PerformanceReceiptBootstrapIncrement.inc"
	}
	void refusedAttempt();
	rts::LiveSimulationPhaseGraphOwnerAdapter m_stage5PhaseGraph;
	UnsignedInt m_stage5PhaseCursor, m_stage5PhaseNow;
	PerformanceReceiptRuntime *m_performanceReceiptRuntime;
	Bool m_isInUpdate;
	unsigned m_frame;
	Bool m_hasUpdated, m_startNewGame;
	GameMode m_gameMode;
	LoadScreen *m_background, *m_loadScreen;
	bool loading;
	unsigned updates, loads, prepares, attempts;
	Fault selectedFault;
};
class PerformanceReceiptRuntime : public ::PerformanceReceiptRuntime
{
public:
	KernelPerformanceAttempt beginAttempt(unsigned kind, unsigned subtype) noexcept
	{ return beginAttemptFromSource(kind, subtype); }
	bool failed() const { return m_phaseObservationFailed.load(std::memory_order_acquire); }
	bool pending() const { return m_pendingOwnerObservation; }
	unsigned pendingFrame() const { return m_pendingFrame; }
	unsigned workloadLast() const { return m_lifecycle.lastCompletedFrame(); }
private:
	KernelPerformanceAttempt beginAttemptFromSource(unsigned, unsigned) noexcept;
};
#include "PerformanceReceiptOwnerBridgeTitleMethods.inc"
#include "PerformanceReceiptAttemptOwnerForwarder.inc"
#include "PerformanceReceiptAttemptRuntime.inc"
#include "PerformanceReceiptBootstrapOnNewGame.inc"

void GameLogic::tryStartNewGame(Bool loadingSaveGame)
{
	// Exact prefix resets frame/hasUpdated/loading, then the whole first
	// loadingSaveGame==FALSE block decides the actual early return.
#include "PerformanceReceiptBootstrapDeferredPrefix.inc"
	++loads; // External asset loader returns, not a simulated defer decision.
	setLoadingMap(FALSE);
}
Bool GameLogic::runOwnerIntakePhase(UnsignedInt &now)
{
	// Complete native pending-start/movie block, including the future hook.
#include "PerformanceReceiptBootstrapResume.inc"
	now = m_frame;
	if (updates != 0) return TRUE;
	if (selectedFault == UndeclaredZero)
	{
		// Deliberate malformed state; no source NEW_GAME branch was executed.
		m_frame = 0; m_startNewGame = TRUE;
		return TRUE;
	}
	if (selectedFault == ConsumeWithoutDeclaration)
		m_performanceReceiptRuntime->observeControlTransition(
			KERNEL_CONTROL_DEFERRED_START_CONSUMED, this, m_frame);
	GameMessage message;
	Check(onNewGame(&message), "native onNewGame reaches the actual deferred start");
	if (selectedFault == DuplicateDeclaration)
		m_performanceReceiptRuntime->observeControlTransition(
			KERNEL_CONTROL_DEFERRED_START_DECLARED, this, m_frame);
	if (selectedFault == WrongOwnerTransition)
	{
		GameLogic other;
		Check(other.attachPerformanceReceiptRuntime(m_performanceReceiptRuntime), "wrong owner fault has a real borrow");
		m_performanceReceiptRuntime->observeControlTransition(
			KERNEL_CONTROL_DEFERRED_START_DECLARED, &other, m_frame);
		Check(other.detachPerformanceReceiptRuntime(m_performanceReceiptRuntime), "wrong owner releases expected borrow");
	}
	if (selectedFault == ForeignTransition)
	{
		std::thread foreign([&]() { m_performanceReceiptRuntime->observeControlTransition(
			KERNEL_CONTROL_DEFERRED_START_DECLARED, this, m_frame); });
		foreign.join();
	}
	if (selectedFault == NoFault || selectedFault == ControlOnly) refusedAttempt();
	return TRUE;
}
void GameLogic::refusedAttempt()
{
	++attempts;
	KernelPerformanceReferenceLedger &ledger = KernelPerformanceReferenceLedger::instance();
	const KernelPerformanceAttempt attempt = beginPerformanceReceiptAttempt(KERNEL_PERFORMANCE_AI, 1);
	Check(attempt.valid(), "actual intake attempt retains actual frame0 inside borrowed frame83");
	if (!attempt.valid()) return;
	KernelPerformanceAttemptDecision decision = {};
	decision.decisionOrdinal = 1; decision.site = 1;
	decision.reasonSchema = 1; decision.reason = 1;
	decision.deterministicEligible = false; decision.deterministicFacts.valid = true;
	decision.admission = KERNEL_ADMISSION_NOT_REQUESTED;
	if (ledger.runMode() == KERNEL_REFERENCE_PHASE_BASELINE_BINDING)
	{
		KernelPerformanceAttemptDecision source = {};
		Check(ledger.replayDecision(attempt, 1, false, decision.deterministicFacts, source),
			"consumer authenticates the actual source refusal in this native control phase");
	}
	else Check(ledger.observeDecision(attempt, decision), "source observes actual no-work refusal");
	KernelPerformanceAttemptFinish finish = {};
	finish.disposition = KERNEL_PERFORMANCE_NOT_ADMITTED; finish.reasonSchema = 1; finish.reason = 1;
	Check(ledger.finishAttempt(attempt, finish), "actual refusal finishes without manufactured batch/body");
	KernelPerformanceAttemptReap reap = {}; reap.reasonSchema = 1; reap.reason = 1;
	Check(ledger.reapAttempt(attempt, reap), "real no-work owner reap releases the attempt");
}
struct Transport
{
	Transport() : clocks(0) {}
	static bool Append(void *context, const unsigned char *bytes, unsigned count)
	{
		Transport &t = *static_cast<Transport *>(context);
		t.bytes.insert(t.bytes.end(), bytes, bytes + count); return true;
	}
	static bool ReadAt(void *context, rts::JobMetricCounter offset, unsigned char *bytes, unsigned count)
	{
		const Transport &t = *static_cast<Transport *>(context);
		if (offset > t.bytes.size() || count > t.bytes.size() - static_cast<size_t>(offset)) return false;
		if (count) memcpy(bytes, &t.bytes[static_cast<size_t>(offset)], count);
		return true;
	}
	static rts::JobMetricCounter Clock(void *context)
	{ return 100 + 10 * static_cast<Transport *>(context)->clocks++; }
	std::vector<unsigned char> bytes;
	unsigned clocks;
};
struct Record
{
	Record() { memset(fields, 0, sizeof(fields)); }
	rts::JobMetricCounter fields[30];
};
bool ReadRecords(const std::vector<unsigned char> &bytes, std::vector<Record> &records)
{
	const char domain[] = "RTS-KERNEL-FIELDS-v1";
	size_t offset = sizeof(domain) - 1 + 4;
	if (bytes.size() < offset || memcmp(&bytes[0], domain, sizeof(domain)-1)) return false;
	while (offset < bytes.size())
	{
		if (bytes.size() - offset < 5) return false;
		const unsigned type = bytes[offset], width = type == 3 ? 8 : (type == 5 ? 1 : 4);
		if (type == 0 || type > 6 || bytes.size() - offset < 5 + width) return false;
		unsigned tag = 0;
		for (unsigned i = 0; i != 4; ++i) tag |= static_cast<unsigned>(bytes[offset+1+i]) << (8*i);
		rts::JobMetricCounter value = 0;
		for (unsigned i = 0; i != width; ++i) value |= static_cast<rts::JobMetricCounter>(bytes[offset+5+i]) << (8*i);
		offset += 5 + width;
		if (type == 1 && tag == 1) records.push_back(Record());
		if (records.empty()) return false;
		if (tag < 30) records.back().fields[tag] = value;
	}
	return true;
}
void CheckBoundaryBytes(const Transport &transport, unsigned controls)
{
	std::vector<Record> records;
	Check(ReadRecords(transport.bytes, records), "native source bytes have readable primitive records");
	unsigned declarations = 0, consumed = 0, controlEnds = 0, worlds = 0, attempts = 0, boundaries = 0;
	for (size_t i = 0; i != records.size(); ++i)
	{
		const rts::JobMetricCounter *f = records[i].fields;
		if (f[1] == 3)
		{
			++attempts;
			Check(f[6] == 1 && f[7] == 0 && f[8] == 0 && f[9] == 1,
				"native attempt bytes are sample1/intake/actual0/ordinal1, never borrowed83");
		}
		// tag2 is the trace record ordinal; the kind2 discriminator is tag3.
		if (f[1] != 2 || f[3] < 3 || f[3] > 9) continue;
		++boundaries;
		if (f[3] == 8)
		{
			++declarations;
			Check(f[4] == 1 && f[5] == 0 && f[6] == 83 && f[7] == 83 && f[8] == 0,
				"declaration preserves actual frame0 and separate entry/borrowed83");
		}
		if (f[3] == 9) { ++consumed; Check(f[4] == controls+1 && f[8] == 0, "consume is at actual full-start return before world increment"); }
		if (f[3] == 7) { ++controlEnds; Check(f[8] == 0, "control byte extent ends at actual world0"); }
		if (f[3] == 6) { ++worlds; Check(f[4] == controls+1 && f[8] == 1, "first real world completion is1 after controls"); }
	}
	Check(declarations == 1 && consumed == 1 && controlEnds == controls && worlds == 1 &&
		attempts == 1 && boundaries == 12*(controls+1)+2,
		"real branches emit every boundary once, including repeated movie controls");
}
KernelPerformanceReferenceRunOptions Options(Transport &transport)
{
	KernelPerformanceReferenceRunOptions options;
	options.mode = KERNEL_REFERENCE_THROUGHPUT_BINDING;
	options.clock = &Transport::Clock; options.clockContext = &transport;
	options.trace.mode = KERNEL_TRACE_RECORD;
	options.trace.append = &Transport::Append; options.trace.context = &transport;
	options.trace.limits.maximumBytes = 65536; options.trace.limits.maximumRecords = 1000;
	options.trace.limits.maximumLogicalEvents = 1000; options.trace.limits.maximumAttempts = 100;
	options.trace.limits.maximumRanges = 100; options.trace.residentAttemptCapacity = 1;
	options.trace.residentRangeCapacity = 1;
	KernelPerformanceDigest *bindings[] = { &options.trace.binding.nativeRunIdentity,
		&options.trace.binding.executable, &options.trace.binding.fixture, &options.trace.binding.sourcePolicy };
	for (unsigned i = 0; i != 4; ++i) { bindings[i]->valid = true; bindings[i]->bytes[0] = static_cast<unsigned char>(i+1); }
	return options; // Explicit transport-only test bindings, never native qualification.
}
#include "PerformanceReceiptBootstrapSchedulerBoundary.inc"
struct Outcome { KernelPerformanceReferenceSnapshot reference; KernelPerformanceSnapshot timing; bool failed; };
Outcome Run(unsigned controls, Fault fault, Transport &transport, const KernelPerformanceTraceSnapshot *source = 0)
{
	PerformanceReceiptRuntime runtime;
	Check(runtime.begin("fresh-ai-map", ""), "bootstrap uses a real begun untraced runtime before test-only transport binding");
	KernelPerformanceLedger &timing = KernelPerformanceLedger::instance();
	KernelPerformanceReferenceLedger &reference = KernelPerformanceReferenceLedger::instance();
	timing.freeze(); reference.freeze();
	KernelPerformanceTimingRunOptions timingOptions;
	timingOptions.enabled = true;
	timingOptions.role = source ? KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE : KERNEL_PERFORMANCE_PIPELINE;
	timingOptions.clock = &Transport::Clock; timingOptions.clockContext = &transport;
	Check(timing.beginRun(timingOptions), "actual existing timing role opens");
	KernelPerformanceReferenceRunOptions options = Options(transport);
	if (source)
	{
		options.mode = KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
		options.trace.mode = KERNEL_TRACE_CONSUME; options.trace.append = 0;
		options.trace.readAt = &Transport::ReadAt; options.trace.sourceByteCount = transport.bytes.size();
		options.trace.sourceTraceDigest = source->digest;
		options.trace.sourceReceiptDigest.valid = true; options.trace.sourceReceiptDigest.bytes[0] = 5;
	}
	Check(reference.beginRun(options), "real bounded Reference record or consume fixture opens");
	GameLogic logic(fault); TheGameLogic = &logic;
	Check(logic.attachPerformanceReceiptRuntime(&runtime), "bootstrap borrows actual runtime through title owner");
	display.movie = false;
	logic.update();
	Check(logic.m_frame == 0 && !logic.m_hasUpdated && !runtime.pending() && runtime.workloadLast() == 0,
		"first original UPDATE ends control0 and cannot supply a workload completion");
	if (fault == NoFault)
	{
		Check(logic.prepares == 1 && logic.loads == 0 && logic.m_startNewGame,
			"actual deferred-return branch, not fixture authority, delays full load");
		for (unsigned i = 1; i < controls; ++i)
		{
			display.movie = true; logic.update();
			Check(logic.m_frame == 0 && !logic.m_hasUpdated && !runtime.pending() && logic.loads == 0,
				"actual movie gate retains another full control extent without re-declaring start");
		}
		display.movie = false; logic.update();
		Check(logic.m_frame == 1 && logic.m_hasUpdated && !logic.m_startNewGame &&
			logic.prepares == 1 && logic.loads == 1 && runtime.pending() && runtime.pendingFrame() == 1,
			"actual resume and native increment yield one pending completed-world1");
		Check(!runtime.failed(), "valid native bootstrap observations stay qualified");
		Check(reference.sealObservationWindow(), "native balanced world/control window seals");
		if (source) Check(timing.sealAdmissions(), "baseline measured admissions seal after actual owner extent");
		Check(reference.sealExecutionClosure(), "real already-reaped refusal permits closure");
		if (source) Check(timing.sealExecutionClosure(capturePerformancePhaseSchedulerBoundary()),
			"baseline closure uses the actual scheduler's drained-work snapshot");
	}
	else if (fault == ControlOnly)
	{
		Check(!reference.sealObservationWindow(), "pending deferred control alone cannot seal a qualified source");
	}
	else Check(runtime.failed(), "invalid transition or undeclared zero remains fail-closed");
	Outcome outcome;
	outcome.failed = runtime.failed(); outcome.reference = reference.freeze(); outcome.timing = timing.freeze();
	if (!source) Check(transport.clocks == 0, "ordinary source role adds no timing clock reads");
	Check(logic.detachPerformanceReceiptRuntime(&runtime), "native expected borrow releases before owner destruction");
	TheGameLogic = 0;
	return outcome;
}
void TestPair(unsigned controls)
{
	Transport transport;
	Outcome source = Run(controls, NoFault, transport);
	Check(source.reference.trace.complete && source.reference.trace.controlWindowCount == controls &&
		source.reference.trace.completedWindowCount == 1, "native source retains control/world coverage separately");
	CheckBoundaryBytes(transport, controls);
	if (!source.reference.trace.complete) return; // Missing source is visible above, not synthetic consume input.
	Outcome baseline = Run(controls, NoFault, transport, &source.reference.trace);
	const KernelPerformancePhaseAccountingSnapshot &a = baseline.timing.phaseAccounting;
	Check(baseline.reference.trace.complete && baseline.reference.trace.digest.equals(source.reference.trace.digest),
		"actual native branch consumer matches the complete source bytes");
	Check(a.complete && a.controlWindowCount == controls && a.completedFrameCount == 1 &&
		a.firstCompletedFrame == 1 && a.lastCompletedFrame == 1 &&
		a.firstControlSampleOrdinal == 1 && a.lastControlSampleOrdinal == controls,
		"native baseline keeps control time and samples outside the world window");
	rts::JobMetricCounter controlTotal = a.controlUnscopedSerialNanoseconds;
	rts::JobMetricCounter worldTotal = a.unscopedSerialNanoseconds;
	for (unsigned i = 0; i != KERNEL_PHASE_COUNT; ++i)
	{
		Check(a.controlPhases[i].samples == controls && a.phases[i].samples == 1,
			"every actual native phase contributes once per control or world extent");
		controlTotal += a.controlPhases[i].totalNanoseconds;
		worldTotal += a.phases[i].totalNanoseconds;
	}
	Check(controlTotal == a.controlNanoseconds && worldTotal == a.frameNanoseconds &&
		a.completionSampleCount == 0, "native control/world phase sums reconcile without inventing completion tail");
}
} // namespace performance_receipt_bootstrap_fixture
int RunPerformanceReceiptBootstrapTitleTests()
{
	using namespace performance_receipt_bootstrap_fixture;
	failures = 0;
	const bool attached = !GameThreadOwnership::IsAttached();
	if (attached) GameThreadOwnership::AttachCurrentThread();
	native_receipt_draft::Environment environment;
	if (!native_receipt_draft::Configure(environment, L"bootstrap-no-publication", L"fresh-ai-map"))
		Fatal("cannot prepare exact W environment");
	globalData.m_mapName = "Maps\\controlled-external-map";
	TestPair(1); TestPair(2);
	const Fault faults[] = { UndeclaredZero, ConsumeWithoutDeclaration, DuplicateDeclaration,
		WrongOwnerTransition, ForeignTransition, ControlOnly };
	for (unsigned i = 0; i != sizeof(faults)/sizeof(faults[0]); ++i)
	{
		Transport transport;
		const Outcome outcome = Run(1, faults[i], transport);
		Check(!outcome.reference.trace.complete, "invalid or control-only native observation cannot become complete evidence");
	}
	if (attached) GameThreadOwnership::DetachCurrentThread();
	if (failures) return 1;
	printf("Native bootstrap source-connected title tests passed.\n");
	return 0;
}
