/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

// Native identity transport, not a baseline executor. The title factory,
// borrow/forwarder and Runtime attempt method are source-generated verbatim.
// Real Runtime observation and the actual Reference ledger remain in use.
#include "Common/GameThreadOwnership.h"
#include "Common/INI.h"
#include "Common/INIException.h"
#include "Lib/PerformanceReceipt.h"
#include "Lib/SimulationPhaseGraphOwnerAdapter.h"
#include <atomic>
#define private protected
#include "Common/PerformanceReceiptRuntime.h"
#undef private
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

namespace performance_receipt_attempt_fixture
{
using namespace rts::performance;
unsigned failures = 0;
void Check(bool condition, const char *message)
{
	if (!condition) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
void UnexpectedFatal(const char *message)
{
	fprintf(stderr, "Unexpected native attempt fatal: %s\n", message);
	exit(2);
}
#undef RELEASE_CRASH
#define RELEASE_CRASH(message) UnexpectedFatal(message)

class PerformanceReceiptRuntime;
class GameLogic;
GameLogic *TheGameLogic = 0;
class GameLogic
{
public:
	GameLogic() : m_stage5PhaseGraph(makeStage5PhaseGraphCallbacks(), this),
		m_stage5PhaseCursor(0), m_stage5PhaseNow(0), m_performanceReceiptRuntime(0),
		m_isInUpdate(FALSE), m_frame(83), updates(0), attempts(0) {}
	static rts::LiveSimulationPhaseOwnerCallbacks makeStage5PhaseGraphCallbacks();
	static bool isStage5PhaseGraphOwner(void *context);
	static bool validateStage5PhaseGraphCommit(unsigned phaseId, unsigned generation,
		unsigned frame, void *context);
	static bool commitStage5PhaseGraphPhase(unsigned phaseId, unsigned generation,
		unsigned frame, void *context);
	static void observeStage5PhaseGraphBoundary(
		rts::LiveSimulationPhaseObservationBoundary boundary,
		rts::SimulationPhaseId phase, unsigned generation, unsigned frame, void *context) noexcept;
	Bool getStage5PhaseAuthorityEvidence(UnsignedInt phase,
		rts::LiveSimulationPhaseAuthorityEvidence &evidence) const;
	bool attachPerformanceReceiptRuntime(PerformanceReceiptRuntime *runtime);
	bool detachPerformanceReceiptRuntime(PerformanceReceiptRuntime *runtime);
	KernelPerformanceAttempt beginPerformanceReceiptAttempt(unsigned workKind,
		unsigned subtype) noexcept;
	bool finishPerformanceReceiptAttempt(KernelPerformanceAttempt attempt,
		KernelPerformanceReferenceBatch validatedBatch,
		KernelPerformanceDisposition disposition, bool fallbackEntered,
		bool fallbackCompleted) noexcept;
	Bool isInGameLogicUpdate() const { return m_isInUpdate; }
	unsigned getFrame() const { return m_frame; }
	void update()
	{
		m_isInUpdate = TRUE;
		m_stage5PhaseCursor = 0;
		Check(m_stage5PhaseGraph.runFrame(m_frame) == rts::LIVE_SIMULATION_PHASE_COMPLETED,
			"real owner factory executes all five ordered phases");
		m_isInUpdate = FALSE;
		++updates;
	}
	Bool runOwnerIntakePhase(UnsignedInt &now)
	{
		now = m_frame;
		if (updates == 0) m_frame = 0;
		attempt();
		attempt();
		return TRUE;
	}
	void runLegacyMutableIslandPhase(UnsignedInt) { attempt(); attempt(); }
	void runSpatialPhase() { attempt(); attempt(); }
	void runOwnerTailPhase() { attempt(); attempt(); }
	void runVerificationAndPublicationPhase() { attempt(); attempt(); ++m_frame; }
	void attempt();
	rts::LiveSimulationPhaseGraphOwnerAdapter m_stage5PhaseGraph;
	UnsignedInt m_stage5PhaseCursor, m_stage5PhaseNow;
	PerformanceReceiptRuntime *m_performanceReceiptRuntime;
	Bool m_isInUpdate;
	unsigned m_frame, updates, attempts;
};
class PerformanceReceiptRuntime : public ::PerformanceReceiptRuntime
{
public:
	KernelPerformanceAttempt beginAttempt(unsigned kind, unsigned subtype) noexcept
	{
		return beginAttemptFromSource(kind, subtype);
	}
private:
	KernelPerformanceAttempt beginAttemptFromSource(unsigned kind, unsigned subtype) noexcept;
};
#include "PerformanceReceiptOwnerBridgeTitleMethods.inc"
#include "PerformanceReceiptAttemptOwnerForwarder.inc"
#include "PerformanceReceiptAttemptRuntime.inc"

void GameLogic::attempt()
{
	++attempts;
	static const unsigned kinds[10] = {
		KERNEL_PERFORMANCE_PHYSICS, KERNEL_PERFORMANCE_STATUS,
		KERNEL_PERFORMANCE_PATH, KERNEL_PERFORMANCE_PATH,
		KERNEL_PERFORMANCE_COLLISION, KERNEL_PERFORMANCE_AI,
		KERNEL_PERFORMANCE_AI, KERNEL_PERFORMANCE_SPATIAL,
		KERNEL_PERFORMANCE_PHYSICS, KERNEL_PERFORMANCE_STATUS
	};
	static const unsigned subtypes[10] = { 0, 0, 0, 1, 0, 0, 1, 0, 0, 0 };
	if (attempts == 1)
	{
		bool foreignAccepted = true;
		std::thread foreign([&]() {
			foreignAccepted = beginPerformanceReceiptAttempt(KERNEL_PERFORMANCE_AI, 1).valid();
		});
		foreign.join();
		Check(!foreignAccepted, "foreign thread cannot mint an attempt during the actual owner phase");
		GameLogic otherOwner;
		Check(otherOwner.attachPerformanceReceiptRuntime(m_performanceReceiptRuntime),
			"second owner uses the actual borrow operation for the identity fault");
		otherOwner.m_isInUpdate = TRUE;
		Check(!otherOwner.beginPerformanceReceiptAttempt(KERNEL_PERFORMANCE_AI, 1).valid(),
			"borrowed second owner cannot mint an attempt inside the genuine owner's phase");
		otherOwner.m_isInUpdate = FALSE;
		Check(otherOwner.detachPerformanceReceiptRuntime(m_performanceReceiptRuntime),
			"second owner identity fault releases its actual expected borrow");
	}
	const KernelPerformanceAttempt token = beginPerformanceReceiptAttempt(
		kinds[attempts - 1], subtypes[attempts - 1]);
	Check(token.valid(), "actual borrowed owner obtains an authentic preflight attempt token");
	if (!token.valid()) return;
	// A real observed preflight refusal closes this transport fixture without
	// inventing captured input, a native job group, or successful pure work.
	KernelPerformanceReferenceLedger &ledger = KernelPerformanceReferenceLedger::instance();
	KernelPerformanceAttemptDecision decision = {};
	decision.decisionOrdinal = 1;
	decision.site = 1;
	decision.reasonSchema = 1;
	decision.reason = 1;
	decision.deterministicEligible = false;
	decision.deterministicFacts.valid = true;
	decision.admission = KERNEL_ADMISSION_NOT_REQUESTED;
	Check(ledger.observeDecision(token, decision), "real attempt records its actual refusal");
	Check(finishPerformanceReceiptAttempt(token, KernelPerformanceReferenceBatch(),
		KERNEL_PERFORMANCE_NOT_ADMITTED, false, false),
		"actual owner closes and reaps a refused attempt without a fabricated batch");
}

struct Transport
{
	Transport() : clocks(0) {}
	static bool Append(void *context, const unsigned char *bytes, unsigned count)
	{
		Transport &transport = *static_cast<Transport *>(context);
		transport.bytes.insert(transport.bytes.end(), bytes, bytes + count);
		return true;
	}
	static rts::JobMetricCounter Clock(void *context)
	{
		Transport &transport = *static_cast<Transport *>(context);
		return 100 + 10 * transport.clocks++;
	}
	std::vector<unsigned char> bytes;
	unsigned clocks;
};
struct RecordedIdentity
{
	RecordedIdentity() : sample(0), phase(0), frame(0), ordinal(0), kind(0), subtype(0) {}
	rts::JobMetricCounter sample, phase, frame, ordinal, kind, subtype;
};
bool ReadAttemptIdentities(const std::vector<unsigned char> &bytes,
	std::vector<RecordedIdentity> &identities)
{
	// Independent reader of the canonical primitive fields; expected identities
	// below are literals derived from the actual owner update sequence.
	const char domain[] = "RTS-KERNEL-FIELDS-v1";
	size_t offset = sizeof(domain) - 1 + 4;
	if (bytes.size() < offset || memcmp(&bytes[0], domain, sizeof(domain) - 1) != 0) return false;
	unsigned recordKind = 0;
	while (offset < bytes.size())
	{
		if (bytes.size() - offset < 5) return false;
		const unsigned type = bytes[offset];
		const unsigned width = type == 3 ? 8 : (type == 5 ? 1 : 4);
		if (type == 0 || type > 6 || bytes.size() - offset < 5 + width) return false;
		unsigned tag = 0;
		for (unsigned i = 0; i != 4; ++i) tag |= static_cast<unsigned>(bytes[offset + 1 + i]) << (8 * i);
		rts::JobMetricCounter value = 0;
		for (unsigned i = 0; i != width; ++i) value |= static_cast<rts::JobMetricCounter>(bytes[offset + 5 + i]) << (8 * i);
		offset += 5 + width;
		if (tag == 1 && type == 1)
		{
			recordKind = static_cast<unsigned>(value);
			if (recordKind == 3) identities.push_back(RecordedIdentity());
		}
		if (recordKind != 3 || identities.empty()) continue;
		RecordedIdentity &identity = identities.back();
		switch (tag)
		{
		case 4: identity.kind = value; break;
		case 5: identity.subtype = value; break;
		case 6: identity.sample = value; break;
		case 7: identity.phase = value; break;
		case 8: identity.frame = value; break;
		case 9: identity.ordinal = value; break;
		}
	}
	return true;
}

class ReceiptEnvironment
{
public:
	void set(const char *name, const char *value)
	{
		Saved saved;
		saved.name = name;
		DWORD size = GetEnvironmentVariableA(name, 0, 0);
		saved.present = size != 0;
		if (saved.present)
		{
			std::vector<char> buffer(size);
			GetEnvironmentVariableA(name, &buffer[0], size);
			saved.value = &buffer[0];
		}
		savedValues.push_back(saved);
		Check(SetEnvironmentVariableA(name, value) != FALSE, "attempt fixture sets process-local input");
	}
	~ReceiptEnvironment()
	{
		for (size_t i = savedValues.size(); i != 0; --i)
			SetEnvironmentVariableA(savedValues[i - 1].name.c_str(),
				savedValues[i - 1].present ? savedValues[i - 1].value.c_str() : 0);
	}
private:
	struct Saved { std::string name, value; bool present; };
	std::vector<Saved> savedValues;
};

void TestActualAttemptIdentity()
{
	PerformanceReceiptRuntime runtime;
	Check(runtime.begin("fresh-ai-map", ""), "native attempt test uses the actual begun runtime");
	Transport transport;
	KernelPerformanceLedger &timing = KernelPerformanceLedger::instance();
	timing.freeze();
	KernelPerformanceTimingRunOptions timingOptions;
	timingOptions.enabled = true;
	timingOptions.role = KERNEL_PERFORMANCE_PIPELINE;
	timingOptions.clock = &Transport::Clock;
	timingOptions.clockContext = &transport;
	Check(timing.beginRun(timingOptions), "ordinary identity uses the existing untimed pipeline role");
	KernelPerformanceReferenceLedger &ledger = KernelPerformanceReferenceLedger::instance();
	ledger.freeze();
	KernelPerformanceReferenceRunOptions options;
	options.mode = KERNEL_REFERENCE_THROUGHPUT_BINDING;
	options.clock = &Transport::Clock;
	options.clockContext = &transport;
	options.trace.mode = KERNEL_TRACE_RECORD;
	options.trace.append = &Transport::Append;
	options.trace.context = &transport;
	options.trace.limits.maximumBytes = 65536;
	options.trace.limits.maximumRecords = 1000;
	options.trace.limits.maximumLogicalEvents = 1000;
	options.trace.limits.maximumAttempts = 100;
	options.trace.limits.maximumRanges = 100;
	options.trace.residentAttemptCapacity = 1;
	options.trace.residentRangeCapacity = 1;
	options.trace.binding.nativeRunIdentity.valid = true;
	options.trace.binding.executable.valid = true;
	options.trace.binding.fixture.valid = true;
	options.trace.binding.sourcePolicy.valid = true;
	options.trace.binding.nativeRunIdentity.bytes[0] = 1;
	options.trace.binding.executable.bytes[0] = 2;
	options.trace.binding.fixture.bytes[0] = 3;
	options.trace.binding.sourcePolicy.bytes[0] = 4;
	Check(ledger.beginRun(options), "actual trace records transport-only binding fixtures");
	GameLogic logic;
	TheGameLogic = &logic;
	Check(!logic.beginPerformanceReceiptAttempt(KERNEL_PERFORMANCE_AI, 1).valid(),
		"unborrowed owner cannot mint an attempt");
	Check(logic.attachPerformanceReceiptRuntime(&runtime), "actual title attaches the test runtime");
	Check(!logic.beginPerformanceReceiptAttempt(KERNEL_PERFORMANCE_AI, 1).valid(),
		"borrow outside UPDATE cannot mint an attempt");
	logic.m_isInUpdate = TRUE;
	Check(!logic.beginPerformanceReceiptAttempt(KERNEL_PERFORMANCE_AI, 1).valid(),
		"in-update flag without a current phase cannot invent phase identity");
	logic.m_isInUpdate = FALSE;
	logic.update();
	Check(logic.attempts == 10 && logic.m_frame == 1,
		"one actual phase graph makes exactly ten native preflight calls");
	Check(transport.clocks == 0, "ordinary owner attempt tracking reads no phase or oracle clocks");
	Check(ledger.sealObservationWindow() && ledger.sealExecutionClosure(),
		"actual no-work attempts close their real trace window and retained slots");
	const KernelPerformanceReferenceSnapshot snapshot = ledger.freeze();
	Check(snapshot.trace.complete && snapshot.trace.attemptCount == 10 &&
		snapshot.trace.notAdmittedAttemptCount == 10 && snapshot.streamCount == 0,
		"all actual preflight refusals are retained without invented successful streams");
	std::vector<RecordedIdentity> identities;
	Check(ReadAttemptIdentities(transport.bytes, identities), "actual trace primitive fields can be read independently");
	Check(identities.size() == 10, "native source trace contains every owner-derived attempt identity");
	for (size_t index = 0; index < identities.size() && index < 10; ++index)
	{
		static const unsigned expectedKinds[10] = {
			KERNEL_PERFORMANCE_PHYSICS, KERNEL_PERFORMANCE_STATUS,
			KERNEL_PERFORMANCE_PATH, KERNEL_PERFORMANCE_PATH,
			KERNEL_PERFORMANCE_COLLISION, KERNEL_PERFORMANCE_AI,
			KERNEL_PERFORMANCE_AI, KERNEL_PERFORMANCE_SPATIAL,
			KERNEL_PERFORMANCE_PHYSICS, KERNEL_PERFORMANCE_STATUS
		};
		static const unsigned expectedSubtypes[10] = {
			0, 0, 0, 1, 0, 0, 1, 0, 0, 0
		};
		const RecordedIdentity &identity = identities[index];
		Check(identity.sample == 1 && identity.phase == index / 2 &&
			identity.frame == 0 && identity.ordinal == index + 1 &&
			identity.kind == expectedKinds[index] &&
			identity.subtype == expectedSubtypes[index],
			"every kernel/subtype attempt uses real sample, phase, world frame and ordinal");
	}
	Check(logic.detachPerformanceReceiptRuntime(&runtime), "actual expected title borrow releases");
	TheGameLogic = 0;
	timing.freeze();
}
} // namespace performance_receipt_attempt_fixture

int RunPerformanceReceiptAttemptTitleTests()
{
	using namespace performance_receipt_attempt_fixture;
	failures = 0;
	const bool attach = !GameThreadOwnership::IsAttached();
	if (attach) GameThreadOwnership::AttachCurrentThread();
	Check(GameThreadOwnership::IsCurrentThread(), "native attempt fixture owns the game thread");
	ReceiptEnvironment environment;
	const char *inputs[][2] = {
		{ "RTS_PERFORMANCE_ROLE", "performance-report" },
		{ "RTS_PERFORMANCE_RUN_ID", "native-attempt-identity-no-publication" },
		{ "RTS_PERFORMANCE_RUN_NONCE", "11111111-1111-4111-8111-111111111111" },
		{ "RTS_PERFORMANCE_COHORT_NONCE", "22222222-2222-4222-8222-222222222222" },
		{ "RTS_PERFORMANCE_COHORT_CREATED_UTC", "2026-01-01T00:00:00Z" },
		{ "RTS_PERFORMANCE_RECEIPT_DIR", "attempt-identity-no-publication" },
		{ "RTS_PERFORMANCE_SOURCE_COMMIT", "0123456789abcdef0123456789abcdef01234567" },
		{ "RTS_PERFORMANCE_ARTIFACT_SET_SHA256", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" },
		{ "RTS_PERFORMANCE_RUNTIME_MANIFEST_SHA256", "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" },
		{ "RTS_PERFORMANCE_RUNTIME_CLOSURE_SHA256", "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC" },
		{ "RTS_PERFORMANCE_FIXTURE_ID", "native-attempt-identity" },
		{ "RTS_PERFORMANCE_RAW_LOG_PATH", "attempt-identity-no-publication/raw.log" },
		{ "RTS_PERFORMANCE_TIMING_PATH", "attempt-identity-no-publication/timing.csv" },
		{ "RTS_PERFORMANCE_VERIFIER_BOUNDARY", "test-only-no-publication" },
		{ "RTS_PERFORMANCE_REFERENCE_MODE", "throughput-binding" },
		{ "RTS_PERFORMANCE_WORKLOAD_QUALIFICATION", "observed-only" },
		{ "RTS_PERFORMANCE_FIXTURE_KIND", "fresh-ai-map" },
		{ "RTS_PERFORMANCE_FIXTURE_SHA256", 0 },
		{ "RTS_PERFORMANCE_PLAYER_COUNT", 0 },
		{ "RTS_PERFORMANCE_UNIT_COUNT", 0 },
		{ "RTS_PERFORMANCE_SEED", 0 },
		{ "RTS_FRAME_TIMING_DIR", 0 }
	};
	for (unsigned i = 0; i != ARRAY_SIZE(inputs); ++i) environment.set(inputs[i][0], inputs[i][1]);
	if (failures == 0) TestActualAttemptIdentity();
	if (attach) GameThreadOwnership::DetachCurrentThread();
	if (failures != 0) return 1;
	printf("Native receipt attempt identity title tests passed.\n");
	return 0;
}
