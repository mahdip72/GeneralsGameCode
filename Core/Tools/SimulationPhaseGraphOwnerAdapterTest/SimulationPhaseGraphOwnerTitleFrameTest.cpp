#include "Lib/SimulationPhaseGraphOwnerAdapter.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef RTS_PHASE_GRAPH_TITLE_NAME
#define RTS_PHASE_GRAPH_TITLE_NAME "title"
#endif

typedef int Bool;
const Bool TRUE = 1;
const Bool FALSE = 0;

// The extracted title callback catches this type but none of these frame
// fixtures throws it. No game initialization, assets, or runtime is required.
struct INIException
{
	const char *mFailureMessage;
};

void unexpectedFatal(const char *reason)
{
	fprintf(stderr, "%s unexpected fatal: %s\n", RTS_PHASE_GRAPH_TITLE_NAME, reason);
	exit(2);
}

#define RELEASE_CRASH(message) unexpectedFatal(message)

class GameLogic
{
public:
	enum IntakeMode
	{
		NORMAL,
		RESET_BEFORE_LEGACY_CAPTURE,
		RESET_AFTER_LEGACY_CAPTURE,
		TIME_FROZEN,
		RESET_AFTER_MUTABLE_PHASE
	};

	GameLogic(unsigned frame, IntakeMode mode)
		: m_stage5PhaseCursor(0), m_stage5PhaseNow(0),
		  m_stage5PhaseGraph(makeCallbacks(), this), owner(true),
		  currentFrame(frame), intakeMode(mode), phaseCalls(0), phaseTrace(0),
		  mutableLegacyFrame(~0u)
	{
	}

	static rts::LiveSimulationPhaseOwnerCallbacks makeCallbacks()
	{
		rts::LiveSimulationPhaseOwnerCallbacks callbacks;
		callbacks.isOwner = &isStage5PhaseGraphOwner;
		callbacks.validate = &validateStage5PhaseGraphCommit;
		callbacks.commit = &commitStage5PhaseGraphPhase;
		return callbacks;
	}

	static bool isStage5PhaseGraphOwner(void *context)
	{
		return context != 0 && static_cast<GameLogic *>(context)->owner;
	}

	// Definitions below are extracted from the selected title's actual source.
	static bool validateStage5PhaseGraphCommit(unsigned phaseId,
		unsigned generation, unsigned frame, void *ownerContext);
	static bool commitStage5PhaseGraphPhase(unsigned phaseId,
		unsigned generation, unsigned frame, void *ownerContext);

	unsigned getFrame() const { return currentFrame; }

	rts::LiveSimulationPhaseRunResult update()
	{
		m_stage5PhaseCursor = 0;
		return m_stage5PhaseGraph.runFrame(getFrame());
	}

	Bool runOwnerIntakePhase(unsigned &now)
	{
		recordPhase(1);
		if (intakeMode == RESET_BEFORE_LEGACY_CAPTURE)
			currentFrame = 0;
		// Legacy intake captures scheduling time before scripts/commands.
		now = getFrame();
		if (intakeMode == TIME_FROZEN)
			return FALSE;
		if (intakeMode == RESET_AFTER_LEGACY_CAPTURE)
			currentFrame = 0;
		return TRUE;
	}

	void runLegacyMutableIslandPhase(unsigned now)
	{
		recordPhase(2);
		mutableLegacyFrame = now;
		if (intakeMode == RESET_AFTER_MUTABLE_PHASE)
			currentFrame = 0;
	}

	void runSpatialPhase() { recordPhase(3); }
	void runOwnerTailPhase() { recordPhase(4); }
	void runVerificationAndPublicationPhase()
	{
		recordPhase(5);
		++currentFrame;
	}

	void recordPhase(unsigned phaseId)
	{
		++phaseCalls;
		phaseTrace = phaseTrace * 10u + phaseId;
	}

	unsigned m_stage5PhaseCursor;
	unsigned m_stage5PhaseNow;
	rts::LiveSimulationPhaseGraphOwnerAdapter m_stage5PhaseGraph;
	bool owner;
	unsigned currentFrame;
	IntakeMode intakeMode;
	unsigned phaseCalls;
	unsigned phaseTrace;
	unsigned mutableLegacyFrame;
};

#include "SimulationPhaseGraphOwnerTitleMethods.inc"

namespace
{
unsigned failures = 0;

void check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "%s FAIL: %s\n", RTS_PHASE_GRAPH_TITLE_NAME, message);
		++failures;
	}
}

void testNormalAndEarlyReset()
{
	GameLogic normal(17, GameLogic::NORMAL);
	check(normal.update() == rts::LIVE_SIMULATION_PHASE_COMPLETED &&
		normal.currentFrame == 18 && normal.mutableLegacyFrame == 17 &&
		normal.phaseCalls == 5 && normal.phaseTrace == 12345,
		"normal frame must retain five ordered callbacks and captured legacy time");
	GameLogic early(83, GameLogic::RESET_BEFORE_LEGACY_CAPTURE);
	check(early.update() == rts::LIVE_SIMULATION_PHASE_COMPLETED &&
		early.currentFrame == 1 && early.mutableLegacyFrame == 0 &&
		early.m_stage5PhaseNow == 0 && early.phaseCalls == 5 && early.phaseTrace == 12345,
		"early start-game reset must retain legacy capture zero and complete once");
	rts::LiveSimulationPhaseAuthorityEvidence intake;
	rts::LiveSimulationPhaseAuthorityEvidence later;
	check(early.m_stage5PhaseGraph.authorityEvidence(1, intake) && intake.frame == 83 &&
		early.m_stage5PhaseGraph.authorityEvidence(2, later) && later.frame == 0 &&
		intake.generation == later.generation && intake.internalEpoch == later.internalEpoch,
		"early reset must retarget only blocked authority within the same generation");
	early.intakeMode = GameLogic::NORMAL;
	early.currentFrame = 0;
	check(early.update() == rts::LIVE_SIMULATION_PHASE_COMPLETED &&
		early.m_stage5PhaseGraph.generation() == 2 && early.currentFrame == 1,
		"between-frame rollback must not reuse the graph generation");
}

void testLateIntakeResetPreservesLegacyTimeAndRetargetsAuthority()
{
	GameLogic logic(64144, GameLogic::RESET_AFTER_LEGACY_CAPTURE);
	check(logic.update() == rts::LIVE_SIMULATION_PHASE_COMPLETED,
		"late clear-game reset must complete the already-entered owner update");
	check(logic.m_stage5PhaseNow == 64144 && logic.mutableLegacyFrame == 64144,
		"late clear-game reset must not rewrite captured legacy scheduling time");
	check(logic.phaseCalls == 5 && logic.phaseTrace == 12345 &&
		logic.m_stage5PhaseGraph.committedPhaseCount() == 5 && logic.currentFrame == 1,
		"late reset must run each remaining callback once without legacy replay or early stop");
	rts::LiveSimulationPhaseAuthorityEvidence intake;
	rts::LiveSimulationPhaseAuthorityEvidence later;
	check(logic.m_stage5PhaseGraph.authorityEvidence(1, intake) && intake.frame == 64144 &&
		logic.m_stage5PhaseGraph.authorityEvidence(2, later) && later.frame == 0 &&
		later.committed && intake.generation == later.generation &&
		intake.internalEpoch == later.internalEpoch,
		"late reset must give blocked authority the post-command owner frame only");
}

void testTimeFreezeAndLaterUnexpectedReset()
{
	GameLogic frozen(27, GameLogic::TIME_FROZEN);
	check(frozen.update() == rts::LIVE_SIMULATION_PHASE_STOPPED_BY_OWNER &&
		frozen.phaseCalls == 1 && frozen.phaseTrace == 1 && frozen.currentFrame == 27 &&
		frozen.m_stage5PhaseGraph.committedPhaseCount() == 1,
		"time-frozen intake must still stop without dispatching later phases");
	GameLogic drift(29, GameLogic::RESET_AFTER_MUTABLE_PHASE);
	check(drift.update() == rts::LIVE_SIMULATION_PHASE_FAILED_AFTER_MUTATION &&
		drift.phaseCalls == 2 && drift.phaseTrace == 12 &&
		drift.m_stage5PhaseGraph.committedPhaseCount() == 2,
		"unexpected post-intake owner-frame drift must still fail closed before the next phase");
}

void testActualTitleValidationGuards()
{
	GameLogic logic(0, GameLogic::NORMAL);
	logic.m_stage5PhaseNow = 64144;
	logic.m_stage5PhaseCursor = 1;
	check(GameLogic::validateStage5PhaseGraphCommit(2, 7, 0, &logic),
		"later authority must validate current world identity independently of legacy scheduling time");
	check(!GameLogic::validateStage5PhaseGraphCommit(2, 7, 1, &logic),
		"wrong authority frame must still be rejected");
	check(!GameLogic::validateStage5PhaseGraphCommit(2, 0, 0, &logic),
		"zero generation must still be rejected");
	check(!GameLogic::validateStage5PhaseGraphCommit(3, 7, 0, &logic),
		"wrong phase cursor must still be rejected");
	logic.owner = false;
	check(!GameLogic::validateStage5PhaseGraphCommit(2, 7, 0, &logic) &&
		!GameLogic::validateStage5PhaseGraphCommit(2, 7, 0, 0),
		"false or absent owner must still be rejected without dereference");
	check(logic.update() == rts::LIVE_SIMULATION_PHASE_FALLBACK_BEFORE_MUTATION &&
		logic.phaseCalls == 0,
		"owner loss must still reject the actual graph before mutation");
}
} // namespace

int main()
{
	testNormalAndEarlyReset();
	testLateIntakeResetPreservesLegacyTimeAndRetargetsAuthority();
	testTimeFreezeAndLaterUnexpectedReset();
	testActualTitleValidationGuards();
	if (failures != 0)
		return 1;
	printf("%s actual-source phase frame tests passed.\n", RTS_PHASE_GRAPH_TITLE_NAME);
	return 0;
}
