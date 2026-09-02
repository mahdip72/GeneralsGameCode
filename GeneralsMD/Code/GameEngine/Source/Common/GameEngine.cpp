/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// GameEngine.cpp /////////////////////////////////////////////////////////////////////////////////
// Implementation of the Game Engine singleton
// Author: Michael S. Booth, April 2001

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/ActionManager.h"
#include "Common/AudioAffect.h"
#include "Common/BuildAssistant.h"
#include "Common/CRCDebug.h"
#include "Common/FramePacer.h"
#include "Lib/FrameTimingDiagnostics.h"
#include "Common/GameThreadOwnership.h"
#include "Common/Radar.h"
#include "Common/PlayerTemplate.h"
#include "Common/Team.h"
#include "Common/PlayerList.h"
#include "Common/GameAudio.h"
#include "Common/GameEngine.h"
#include "Common/INI.h"
#include "Common/INIException.h"
#include "Common/MessageStream.h"
#include "Common/ThingFactory.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/ArchiveFileSystem.h"
#include "Common/LocalFileSystem.h"
#include "Common/GlobalData.h"
#include "Common/PerfTimer.h"
#include "Common/RandomValue.h"
#include "Common/NameKeyGenerator.h"
#include "Common/ModuleFactory.h"
#include "Common/Debug.h"
#include "Common/GameState.h"
#include "Common/GameStateMap.h"
#include "Common/Science.h"
#include "Common/FunctionLexicon.h"
#include "Common/CommandLine.h"
#if defined(_WIN64)
#include "Lib/CollisionCandidateKernel.h"
#include "Lib/DeterministicAIPlanning.h"
#endif
#include "Lib/ObjectStatusTimerKernel.h"
#include "Lib/PhysicsIntegrationKernel.h"
#include "Lib/JobSystem.h"
#include "Lib/PipelineExecutionPolicy.h"
#include "Lib/SimulationExecutionPolicy.h"

#include "rts/profile.h"
#include "Common/DamageFX.h"
#include "Common/MultiplayerSettings.h"
#include "Common/Recorder.h"
#include "Common/SkirmishAITestRunner.h"
#include "Common/SpecialPower.h"
#include "Common/TerrainTypes.h"
#include "Common/Upgrade.h"
#include "Common/OptionPreferences.h"
#include "Common/Xfer.h"
#include "Common/XferCRC.h"
#include "Common/GameLOD.h"
#include "Common/Registry.h"
#include "Common/GameCommon.h"	// FOR THE ALLOW_DEBUG_CHEATS_IN_RELEASE #define

#include "GameLogic/Armor.h"
#include "GameLogic/AI.h"
#include "GameLogic/CaveSystem.h"
#include "GameLogic/CrateSystem.h"
#include "GameLogic/Damage.h"
#include "GameLogic/VictoryConditions.h"
#include "GameLogic/ObjectCreationList.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Locomotor.h"
#include "GameLogic/RankInfo.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/SidesList.h"

#include "GameClient/ClientInstance.h"
#include "GameClient/FXList.h"
#include "GameClient/GameClient.h"
#include "GameClient/Keyboard.h"
#include "GameClient/Shell.h"
#include "GameClient/GameText.h"
#include "GameClient/ParticleSys.h"
#include "GameClient/Water.h"
#include "GameClient/TerrainRoads.h"
#include "GameClient/MetaEvent.h"
#include "GameClient/MapUtil.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/Drawable.h"
#include "GameClient/GUICallbacks.h"

#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/WOLBrowser/WebBrowser.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/NAT.h"
#include "GameNetwork/GameSpy/GameResultsThread.h"

#include "Common/version.h"


//-------------------------------------------------------------------------------------------------

namespace
{
rts::SimulationExecutionMode s_requestedHeadlessSimulationMode =
	rts::SIMULATION_EXECUTION_SERIAL;
rts::PipelineExecutionMode s_requestedHeadlessPipelineMode =
	rts::PIPELINE_EXECUTION_PARALLEL;
Bool s_headlessSimulationJobSystemStartAttempted = FALSE;
Bool s_headlessSimulationJobSystemStarted = FALSE;
unsigned s_headlessSimulationWorkerCount = 0;

bool ensureSimulationJobsStarted(void *context)
{
	return static_cast<rts::JobSystem *>(context)->ensureStarted();
}

const char *headlessSimulationModeName(rts::SimulationExecutionMode mode)
{
	switch (mode)
	{
		case rts::SIMULATION_EXECUTION_PARALLEL: return "parallel";
		case rts::SIMULATION_EXECUTION_SHADOW: return "shadow";
		default: return "serial";
	}
}

const char *headlessPipelineModeName(rts::PipelineExecutionMode mode)
{
	return mode == rts::PIPELINE_EXECUTION_SERIAL ? "serial" : "parallel";
}

void startHeadlessSimulationJobsAfterUnsafeInitialization()
{
	if (!TheGlobalData->m_headless ||
		s_headlessSimulationJobSystemStartAttempted) return;
	s_headlessSimulationJobSystemStartAttempted = TRUE;
	s_requestedHeadlessSimulationMode = rts::GetSimulationExecutionMode();
	s_requestedHeadlessPipelineMode = rts::GetPipelineExecutionMode();
	rts::JobSystem &jobs = rts::JobSystem::instance();
	jobs.resetMetrics();
#if defined(_WIN64)
	rts::ResetAIPlanningRuntimeMetrics();
	rts::ResetPhysicsIntegrationRuntimeMetrics();
	rts::ResetObjectStatusTimerRuntimeMetrics();
#endif

	if (rts::GetPipelineExecutionMode() != rts::PIPELINE_EXECUTION_SERIAL &&
		!rts::SetPipelineExecutionMode(rts::PIPELINE_EXECUTION_SERIAL))
	{
		rts::SetSimulationExecutionMode(rts::SIMULATION_EXECUTION_SERIAL);
		rts::LockSimulationExecutionMode();
		printf("SIMULATION_JOB_SYSTEM_FALLBACK requested_mode=%s reason=pipeline_mode_locked\n",
			headlessSimulationModeName(s_requestedHeadlessSimulationMode));
		fflush(stdout);
		return;
	}
	rts::LockPipelineExecutionMode();

#if defined(_MSC_VER) && _MSC_VER < 1300
	if (s_requestedHeadlessSimulationMode != rts::SIMULATION_EXECUTION_SERIAL)
		rts::SetSimulationExecutionMode(rts::SIMULATION_EXECUTION_SERIAL);
	rts::LockSimulationExecutionMode();
	return;
#else
	if (rts::GetSimulationExecutionMode() == rts::SIMULATION_EXECUTION_SERIAL)
	{
		rts::LockSimulationExecutionMode();
		return;
	}

	if (!jobs.start(rts::JobSystem::startupConfig()))
	{
		rts::SetSimulationExecutionMode(rts::SIMULATION_EXECUTION_SERIAL);
		rts::LockSimulationExecutionMode();
		printf("SIMULATION_JOB_SYSTEM_FALLBACK requested_mode=%s reason=start_failed\n",
			headlessSimulationModeName(s_requestedHeadlessSimulationMode));
		fflush(stdout);
		return;
	}
	if (!jobs.registerCurrentThread(rts::JOB_OWNER_GAME))
	{
		jobs.shutdown();
		rts::SetSimulationExecutionMode(rts::SIMULATION_EXECUTION_SERIAL);
		rts::LockSimulationExecutionMode();
		printf("SIMULATION_JOB_SYSTEM_FALLBACK requested_mode=%s reason=owner_registration_failed\n",
			headlessSimulationModeName(s_requestedHeadlessSimulationMode));
		fflush(stdout);
		return;
	}
	rts::LockSimulationExecutionMode();
	s_headlessSimulationJobSystemStarted = TRUE;
	s_headlessSimulationWorkerCount = jobs.workerCount();
	printf("SIMULATION_JOB_SYSTEM_START requested_mode=%s effective_mode=%s requested_pipeline=%s effective_pipeline=serial workers=%u\n",
		headlessSimulationModeName(s_requestedHeadlessSimulationMode),
		headlessSimulationModeName(rts::GetSimulationExecutionMode()),
		headlessPipelineModeName(s_requestedHeadlessPipelineMode),
		s_headlessSimulationWorkerCount);
	fflush(stdout);
#endif
}

void printHeadlessSimulationJobMetrics(const rts::JobSystemMetrics &metrics,
	const rts::PhysicsIntegrationRuntimeMetrics &physics,
	const rts::ObjectStatusTimerRuntimeMetrics &status)
{
#if defined(_MSC_VER) && _MSC_VER < 1300
	printf("SIMULATION_JOB_METRICS requested_mode=%s effective_mode=serial requested_pipeline=%s effective_pipeline=serial scheduler_started=0 workers=0\n",
		headlessSimulationModeName(s_requestedHeadlessSimulationMode),
		headlessPipelineModeName(s_requestedHeadlessPipelineMode));
#else
	printf("SIMULATION_JOB_METRICS requested_mode=%s effective_mode=%s requested_pipeline=%s effective_pipeline=serial scheduler_started=%u workers=%u submitted=%llu executed=%llu steals=%llu owner_help=%llu waits=%llu worker_wait_rejections=%llu failures=%llu cancelled=%llu fallback=%llu queue_latency_ns=%llu max_queue_latency_ns=%llu sleeps=%llu wakes=%llu affinity_failures=%llu queue_high_water=%u peak_active_workers=%u available_cpus=%u reserved_owner_cpus=%u selected_worker_cpus=%u\n",
		headlessSimulationModeName(s_requestedHeadlessSimulationMode),
		s_headlessSimulationJobSystemStarted ?
			headlessSimulationModeName(rts::GetSimulationExecutionMode()) : "serial",
		headlessPipelineModeName(s_requestedHeadlessPipelineMode),
		s_headlessSimulationJobSystemStarted ? 1u : 0u,
		s_headlessSimulationWorkerCount,
		static_cast<unsigned long long>(metrics.submittedJobCount),
		static_cast<unsigned long long>(metrics.executedJobCount),
		static_cast<unsigned long long>(metrics.stealCount),
		static_cast<unsigned long long>(metrics.ownerHelpCount),
		static_cast<unsigned long long>(metrics.waitCount),
		static_cast<unsigned long long>(metrics.workerWaitRejectionCount),
		static_cast<unsigned long long>(metrics.failedJobCount),
		static_cast<unsigned long long>(metrics.cancelledJobCount),
		static_cast<unsigned long long>(metrics.serialFallbackCount),
		static_cast<unsigned long long>(metrics.totalQueueLatencyNanoseconds),
		static_cast<unsigned long long>(metrics.maximumQueueLatencyNanoseconds),
		static_cast<unsigned long long>(metrics.workerSleepCount),
		static_cast<unsigned long long>(metrics.workerWakeCount),
		static_cast<unsigned long long>(metrics.affinityFailureCount),
		metrics.injectionHighWater, metrics.maximumActiveWorkers,
		metrics.availableLogicalCpuCount, metrics.reservedOwnerCpuCount,
		metrics.selectedWorkerCpuCount);
#endif
	#if defined(_WIN64)
	const rts::JobSystemConfig config = rts::JobSystem::startupConfig();
	const char *workerPolicy = config.workerPolicy == rts::JOB_WORKER_POLICY_ALL ?
		"all" : (config.workerPolicy == rts::JOB_WORKER_POLICY_AUTO ? "auto" : "unknown");
	printf("SIMULATION_JOB_TOPOLOGY worker_policy=%s pin_workers=%u "
		"selected_worker_physical_cores=%u selected_worker_physical_mask=%llu "
		"selected_worker_physical_mask_complete=%u cpu_set_count=%u "
		"selected_worker_cpu_set_count=%u owner_cpu_set_count=%u\n",
		workerPolicy, config.pinWorkers ? 1u : 0u,
		metrics.selectedWorkerPhysicalCoreCount,
		static_cast<unsigned long long>(metrics.selectedWorkerPhysicalCoreMask),
		metrics.selectedWorkerPhysicalCoreMaskComplete ? 1u : 0u,
		rts::JobSystem::instance().cpuSetCount(),
		rts::JobSystem::instance().selectedWorkerCpuSetCount(),
		rts::JobSystem::instance().ownerCpuSetCount());
	fflush(stdout);
	#endif
#if defined(_WIN64)
	const rts::AIPlanningRuntimeMetrics ai = rts::GetAIPlanningRuntimeMetrics();
	printf("AI_PLANNING_MANIFEST epoch=%u captured_snapshots=%llu captured_candidates=%llu requested_batches=%llu submitted_jobs=%llu completed_jobs=%llu serial_fallbacks=%llu shadow_matches=%llu shadow_mismatches=%llu validation_failures=%llu committed_batches=%llu parallel_authoritative_commits=%llu rejected_commits=%llu\n",
		(unsigned)SKIRMISH_AI_REPLAY_EPOCH_COUNTER_RNG,
		static_cast<unsigned long long>(ai.capturedSnapshots),
		static_cast<unsigned long long>(ai.capturedCandidates),
		static_cast<unsigned long long>(ai.requestedBatches),
		static_cast<unsigned long long>(ai.submittedJobs),
		static_cast<unsigned long long>(ai.completedJobs),
		static_cast<unsigned long long>(ai.serialFallbacks),
		static_cast<unsigned long long>(ai.shadowMatches),
		static_cast<unsigned long long>(ai.shadowMismatches),
		static_cast<unsigned long long>(ai.validationFailures),
		static_cast<unsigned long long>(ai.committedBatches),
		static_cast<unsigned long long>(ai.parallelAuthoritativeCommits),
		static_cast<unsigned long long>(ai.rejectedCommits));
	const rts::CollisionCandidateRuntimeMetrics collision =
		rts::GetCollisionCandidateRuntimeMetrics();
	printf("COLLISION_CANDIDATE_MANIFEST authoritative_commits=%llu shadow_executions=%llu shadow_compared_candidates=%llu shadow_mismatches=%llu owner_fallbacks=%llu unexpected_fallbacks=%llu ineligible_slices=%llu stale_rejections=%llu committed_candidates=%llu prepared_pairs=%llu unique_candidates=%llu submitted_jobs=%llu completed_jobs=%llu physical_worker_jobs=%llu owner_helped_jobs=%llu physical_worker_mask=%llu distinct_physical_workers=%u physical_worker_mask_complete=%u\n",
		static_cast<unsigned long long>(collision.authoritativeCommits),
		static_cast<unsigned long long>(collision.shadowExecutions),
		static_cast<unsigned long long>(collision.shadowComparedCandidates),
		static_cast<unsigned long long>(collision.shadowMismatches),
		static_cast<unsigned long long>(collision.ownerFallbacks),
		static_cast<unsigned long long>(collision.unexpectedFallbacks),
		static_cast<unsigned long long>(collision.ineligibleSlices),
		static_cast<unsigned long long>(collision.staleRejections),
		static_cast<unsigned long long>(collision.committedCandidates),
		static_cast<unsigned long long>(collision.preparedPairs),
		static_cast<unsigned long long>(collision.uniqueCandidates),
		static_cast<unsigned long long>(collision.submittedJobs),
		static_cast<unsigned long long>(collision.completedJobs),
		static_cast<unsigned long long>(collision.physicalWorkerJobs),
		static_cast<unsigned long long>(collision.ownerHelpedJobs),
		static_cast<unsigned long long>(collision.physicalWorkerMask),
		collision.distinctPhysicalWorkers,
		collision.physicalWorkerMaskComplete ? 1U : 0U);
	printf("PHYSICS_INTEGRATION_MANIFEST authoritative_batches=%llu committed_prefixes=%llu ranges=%llu submitted_jobs=%llu completed_jobs=%llu physical_worker_jobs=%llu owner_helped_jobs=%llu physical_worker_mask=%llu distinct_physical_workers=%u physical_worker_mask_complete=%u peak_concurrent_physical_workers=%u allocated_bytes=%llu capture_ns=%llu prepare_ns=%llu wait_ns=%llu commit_ns=%llu storage_bytes=%llu storage_capacity_bytes=%llu storage_allocations=%llu shadow_executions=%llu shadow_prefixes=%llu shadow_ranges=%llu shadow_submitted_jobs=%llu shadow_completed_jobs=%llu shadow_matches=%llu shadow_mismatches=%llu owner_fallbacks=%llu ineligible_slices=%llu unexpected_fallbacks=%llu stale_rejections=%llu circuit_breaker_trips=%llu\n",
		static_cast<unsigned long long>(physics.acceptedBatches),
		static_cast<unsigned long long>(physics.acceptedPrefixes),
		static_cast<unsigned long long>(physics.acceptedRanges),
		static_cast<unsigned long long>(physics.acceptedSubmittedJobs),
		static_cast<unsigned long long>(physics.acceptedCompletedJobs),
		static_cast<unsigned long long>(physics.acceptedPhysicalWorkerJobs),
		static_cast<unsigned long long>(physics.acceptedOwnerHelpedJobs),
		static_cast<unsigned long long>(physics.acceptedPhysicalWorkerMask),
		physics.maximumAcceptedDistinctPhysicalWorkers,
		physics.acceptedPhysicalWorkerMaskComplete ? 1U : 0U,
		physics.maximumAcceptedPeakConcurrentPhysicalWorkers,
		static_cast<unsigned long long>(physics.acceptedAllocatedBytes),
		static_cast<unsigned long long>(physics.acceptedCaptureNanoseconds),
		static_cast<unsigned long long>(physics.acceptedPrepareNanoseconds),
		static_cast<unsigned long long>(physics.acceptedWaitNanoseconds),
		static_cast<unsigned long long>(physics.acceptedCommitNanoseconds),
		static_cast<unsigned long long>(physics.acceptedStorageBytes),
		static_cast<unsigned long long>(physics.acceptedStorageCapacityBytes),
		static_cast<unsigned long long>(physics.acceptedStorageAllocations),
		static_cast<unsigned long long>(physics.shadowBatches),
		static_cast<unsigned long long>(physics.shadowPrefixes),
		static_cast<unsigned long long>(physics.shadowRanges),
		static_cast<unsigned long long>(physics.shadowSubmittedJobs),
		static_cast<unsigned long long>(physics.shadowCompletedJobs),
		static_cast<unsigned long long>(physics.shadowMatches),
		static_cast<unsigned long long>(physics.shadowMismatches),
		static_cast<unsigned long long>(physics.ownerFallbacks),
		static_cast<unsigned long long>(physics.ineligibleSlices),
		static_cast<unsigned long long>(physics.unexpectedFallbacks),
		static_cast<unsigned long long>(physics.staleRejections),
		static_cast<unsigned long long>(physics.circuitBreakerTrips));
	printf("OBJECT_STATUS_TIMER_MANIFEST authoritative_batches=%llu committed_commands=%llu submitted_jobs=%llu completed_jobs=%llu physical_worker_jobs=%llu owner_helped_jobs=%llu physical_worker_mask=%llu distinct_physical_workers=%u physical_worker_mask_complete=%u peak_concurrent_physical_workers=%u shadow_executions=%llu shadow_commands=%llu shadow_matches=%llu shadow_mismatches=%llu owner_fallbacks=%llu stale_rejections=%llu\n",
		static_cast<unsigned long long>(status.authoritativeBatches),
		static_cast<unsigned long long>(status.committedCommands),
		static_cast<unsigned long long>(status.submittedJobs),
		static_cast<unsigned long long>(status.completedJobs),
		static_cast<unsigned long long>(status.physicalWorkerJobs),
		static_cast<unsigned long long>(status.ownerHelpedJobs),
		static_cast<unsigned long long>(status.physicalWorkerMask),
		status.maximumDistinctPhysicalWorkers,
		status.physicalWorkerMaskComplete ? 1U : 0U,
		status.maximumPeakConcurrentPhysicalWorkers,
		static_cast<unsigned long long>(status.shadowExecutions),
		static_cast<unsigned long long>(status.shadowCommands),
		static_cast<unsigned long long>(status.shadowMatches),
		static_cast<unsigned long long>(status.shadowMismatches),
		static_cast<unsigned long long>(status.ownerFallbacks),
		static_cast<unsigned long long>(status.staleRejections));
#endif
	fflush(stdout);
}
}

//-------------------------------------------------------------------------------------------------

#ifdef DEBUG_CRC
class DeepCRCSanityCheck : public SubsystemInterface
{
public:
	DeepCRCSanityCheck() {}
	virtual ~DeepCRCSanityCheck() {}

	virtual void init() {}
	virtual void reset();
	virtual void update() {}

protected:
};

DeepCRCSanityCheck *TheDeepCRCSanityCheck = nullptr;

void DeepCRCSanityCheck::reset()
{
	static Int timesThrough = 0;
	static UnsignedInt lastCRC = 0;

	AsciiString fname;
	fname.format("%sCRCAfter%dMaps.dat", TheGlobalData->getPath_UserData().str(), timesThrough);
	UnsignedInt thisCRC = TheGameLogic->getCRC( CRC_RECALC, fname );

	DEBUG_LOG(("DeepCRCSanityCheck: CRC is %X", thisCRC));
	DEBUG_ASSERTCRASH(timesThrough == 0 || thisCRC == lastCRC,
		("CRC after reset did not match beginning CRC!\nNetwork games won't work after this.\nOld: 0x%8.8X, New: 0x%8.8X",
		lastCRC, thisCRC));
	lastCRC = thisCRC;

	timesThrough++;
}
#endif // DEBUG_CRC

//-------------------------------------------------------------------------------------------------
/// The GameEngine singleton instance
GameEngine *TheGameEngine = nullptr;

//-------------------------------------------------------------------------------------------------
SubsystemInterfaceList* TheSubsystemList = nullptr;

//-------------------------------------------------------------------------------------------------
template<class SUBSYSTEM>
void initSubsystem(
	SUBSYSTEM*& sysref,
	AsciiString name,
	SUBSYSTEM* sys,
	Xfer *pXfer,
	const char* path1 = nullptr,
	const char* path2 = nullptr)
{
	sysref = sys;
	TheSubsystemList->initSubsystem(sys, path1, path2, pXfer, name);
}

//-------------------------------------------------------------------------------------------------
extern HINSTANCE ApplicationHInstance;  ///< our application instance
extern CComModule _Module;
extern LANAPI *TheLAN;

//-------------------------------------------------------------------------------------------------
static void updateTGAtoDDS();

//-------------------------------------------------------------------------------------------------
static void updateWindowTitle()
{
	// TheSuperHackers @tweak Now prints product and version information in the Window title.

	DEBUG_ASSERTCRASH(TheVersion != nullptr, ("TheVersion is null"));
	DEBUG_ASSERTCRASH(TheGameText != nullptr, ("TheGameText is null"));

	UnicodeString title;

	if (rts::ClientInstance::getInstanceId() > 1u)
	{
		UnicodeString str;
		str.format(L"Instance:%.2u", rts::ClientInstance::getInstanceId());
		title.concat(str);
	}

	UnicodeString productString = TheVersion->getUnicodeProductString();

	if (!productString.isEmpty())
	{
		if (!title.isEmpty())
			title.concat(L" ");
		title.concat(productString);
	}

#if RTS_GENERALS
	const WideChar* defaultGameTitle = L"Command and Conquer Generals";
#elif RTS_ZEROHOUR
	const WideChar* defaultGameTitle = L"Command and Conquer Generals Zero Hour";
#endif
	UnicodeString gameTitle = TheGameText->FETCH_OR_SUBSTITUTE("GUI:Command&ConquerGenerals", defaultGameTitle);

	if (!gameTitle.isEmpty())
	{
		UnicodeString gameTitleFinal;
		UnicodeString gameVersion = TheVersion->getUnicodeVersion();

		if (productString.isEmpty())
		{
			gameTitleFinal = gameTitle;
		}
		else
		{
			UnicodeString gameTitleFormat = TheGameText->FETCH_OR_SUBSTITUTE("Version:GameTitle", L"for %ls");
			gameTitleFinal.format(gameTitleFormat.str(), gameTitle.str());
		}

		if (!title.isEmpty())
			title.concat(L" ");
		title.concat(gameTitleFinal.str());
		title.concat(L" ");
		title.concat(gameVersion.str());
	}

	if (!title.isEmpty())
	{
		AsciiString titleA;
		titleA.translate(title);	//get ASCII version for Win 9x

		extern HWND ApplicationHWnd;  ///< our application window handle
		if (ApplicationHWnd) {
			//Set it twice because Win 9x does not support SetWindowTextW.
			::SetWindowText(ApplicationHWnd, titleA.str());
			::SetWindowTextW(ApplicationHWnd, title.str());
		}
	}
}

//-------------------------------------------------------------------------------------------------
GameEngine::GameEngine()
{
	// initialize to non garbage values
	m_logicTimeAccumulator = 0.0f;
	m_quitting = FALSE;
	m_isActive = FALSE;

	_Module.Init(nullptr, ApplicationHInstance, nullptr);
}

//-------------------------------------------------------------------------------------------------
GameEngine::~GameEngine()
{
	rts::JobSystem &jobSystem = rts::JobSystem::instance();
	const Bool releaseJobOwner =
		jobSystem.isCurrentThread(rts::JOB_OWNER_GAME);
	const Bool reportHeadlessMetrics = TheGlobalData->m_headless &&
		s_headlessSimulationJobSystemStartAttempted &&
		TheGlobalData->m_simulateReplays.empty();
	if (TheGlobalData->m_headless && s_headlessSimulationJobSystemStarted)
		jobSystem.shutdown();
	const rts::JobSystemMetrics headlessMetrics = jobSystem.metrics();
#if defined(_WIN64)
	const rts::PhysicsIntegrationRuntimeMetrics headlessPhysicsMetrics =
		rts::GetPhysicsIntegrationRuntimeMetrics();
	const rts::ObjectStatusTimerRuntimeMetrics headlessStatusMetrics =
		rts::GetObjectStatusTimerRuntimeMetrics();
#else
	const rts::PhysicsIntegrationRuntimeMetrics headlessPhysicsMetrics;
	const rts::ObjectStatusTimerRuntimeMetrics headlessStatusMetrics;
#endif
	//extern std::vector<std::string>	preloadTextureNamesGlobalHack;
	//preloadTextureNamesGlobalHack.clear();

	// These globals own live transports which use the shared network owner.
	// Release them while game globals and JobSystem are still available.
	delete TheLAN;
	TheLAN = nullptr;
	delete TheNAT;
	TheNAT = nullptr;

	delete TheMapCache;
	TheMapCache = nullptr;

//	delete TheShell;
//	TheShell = nullptr;

	TheGameResultsQueue->endThreads();

	// TheSuperHackers @fix helmutbuhler 03/06/2025
	// Reset all subsystems before deletion to prevent crashing due to cross dependencies.
	reset();
	if (!TheGlobalData->m_headless)
		jobSystem.shutdown();

	TheSubsystemList->shutdownAll();
	delete TheSubsystemList;
	TheSubsystemList = nullptr;

	delete TheSkirmishGameInfo;
	TheSkirmishGameInfo = nullptr;

	delete TheChallengeGameInfo;
	TheChallengeGameInfo = nullptr;

	delete TheNetwork;
	TheNetwork = nullptr;

	delete TheCommandList;
	TheCommandList = nullptr;

	delete TheNameKeyGenerator;
	TheNameKeyGenerator = nullptr;

	delete TheFileSystem;
	TheFileSystem = nullptr;

	delete TheGameLODManager;
	TheGameLODManager = nullptr;

	Drawable::killStaticImages();

	_Module.Term();
	if (releaseJobOwner)
	{
		if (!jobSystem.unregisterCurrentThread(rts::JOB_OWNER_GAME))
			DEBUG_LOG(("JobSystem game-owner registration could not be released."));
	}
	if (reportHeadlessMetrics)
		printHeadlessSimulationJobMetrics(headlessMetrics, headlessPhysicsMetrics,
			headlessStatusMetrics);

#ifdef PERF_TIMERS
	PerfGather::termPerfDump();
#endif
}

//-------------------------------------------------------------------------------------------------
Bool GameEngine::isTimeFrozen()
{
	// TheSuperHackers @fix The time can no longer be frozen in Network games. It would disconnect the player.
	if (TheNetwork != nullptr)
		return false;

	if (TheTacticalView != nullptr)
	{
		if (TheTacticalView->isTimeFrozen() && !TheTacticalView->isCameraMovementFinished())
			return true;
	}

	if (TheScriptEngine != nullptr)
	{
		if (TheScriptEngine->isTimeFrozenDebug() || TheScriptEngine->isTimeFrozenScript())
			return true;
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
Bool GameEngine::isGameHalted()
{
	if (TheNetwork != nullptr)
	{
		if (TheNetwork->isStalling())
			return true;
	}
	else
	{
		if (TheGameLogic != nullptr && TheGameLogic->isGamePaused())
			return true;
	}

	return false;
}

/** -----------------------------------------------------------------------------------------------
 * Initialize the game engine by initializing the GameLogic and GameClient.
 */
void GameEngine::init()
{
	ASSERT_GAME_THREAD("GameEngine::init");
	try {
		// Headless model/pose queries can reach lazy compute consumers too.
		// Disable lazy startup before any subsystem loads assets; shutdown on a
		// never-started scheduler creates no workers and preserves serial paths.
		if (TheGlobalData->m_headless)
			rts::JobSystem::instance().shutdown();
		if (!TheGlobalData->m_headless)
		{
			rts::JobSystem &jobSystem = rts::JobSystem::instance();
			const rts::SimulationExecutionStartupResult startupResult =
				rts::PrepareSimulationExecutionStartup(
					ensureSimulationJobsStarted, &jobSystem);
			if (startupResult == rts::SIMULATION_EXECUTION_STARTUP_POLICY_FAILURE)
			{
				RELEASE_CRASH(("JobSystem startup failed and simulation policy could not fall back to serial."));
			}
			else if (startupResult ==
				rts::SIMULATION_EXECUTION_STARTUP_SERIAL_FALLBACK)
			{
				DEBUG_LOG(("JobSystem startup failed; parallel consumers will use their serial fallback."));
			}
			else if (!jobSystem.registerCurrentThread(rts::JOB_OWNER_GAME))
			{
				RELEASE_CRASH(("JobSystem was initialized by a different game owner."));
			}
		}

		//create an INI object to use for loading stuff
		INI ini;

#ifdef DEBUG_LOGGING
		if (TheVersion)
		{
			DEBUG_LOG(("================================================================================"));
			DEBUG_LOG(("Generals version %s", TheVersion->getAsciiVersion().str()));
			DEBUG_LOG(("Build date: %s", TheVersion->getAsciiBuildTime().str()));
			DEBUG_LOG(("Build location: %s", TheVersion->getAsciiBuildLocation().str()));
			DEBUG_LOG(("Build user: %s", TheVersion->getAsciiBuildUser().str()));
			DEBUG_LOG(("Build git revision: %s", TheVersion->getAsciiGitCommitCount().str()));
			DEBUG_LOG(("Build git version: %s", TheVersion->getAsciiGitTagOrHash().str()));
			DEBUG_LOG(("Build git commit time: %s", TheVersion->getAsciiGitCommitTime().str()));
			DEBUG_LOG(("Build git commit author: %s", Version::getGitCommitAuthorName()));
			DEBUG_LOG(("================================================================================"));
		}
#endif

	#if defined(PERF_TIMERS) || defined(DUMP_PERF_STATS)
		DEBUG_LOG(("Calculating CPU frequency for performance timers."));
		InitPrecisionTimer();
	#endif
	#ifdef PERF_TIMERS
		PerfGather::initPerfDump("AAAPerfStats", PerfGather::PERF_NETTIME);
	#endif




	#ifdef DUMP_PERF_STATS////////////////////////////////////////////////////////////
	__int64 startTime64;//////////////////////////////////////////////////////////////
	__int64 endTime64,freq64;///////////////////////////////////////////////////////////
	GetPrecisionTimerTicksPerSec(&freq64);///////////////////////////////////////////////
	GetPrecisionTimer(&startTime64);////////////////////////////////////////////////////
  char Buf[256];//////////////////////////////////////////////////////////////////////
	#endif//////////////////////////////////////////////////////////////////////////////


		TheSubsystemList = MSGNEW("GameEngineSubsystem") SubsystemInterfaceList;

		TheSubsystemList->addSubsystem(this);

		// initialize the random number system
		InitRandom();

		// Create the low-level file system interface
		TheFileSystem = createFileSystem();

		// not part of the subsystem list, because it should normally never be reset!
		TheNameKeyGenerator = MSGNEW("GameEngineSubsystem") NameKeyGenerator;
		TheNameKeyGenerator->init();


    	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheNameKeyGenerator  = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		// not part of the subsystem list, because it should normally never be reset!
		TheCommandList = MSGNEW("GameEngineSubsystem") CommandList;
		TheCommandList->init();

    	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheCommandList  = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		XferCRC xferCRC;
		xferCRC.open("lightCRC");


		initSubsystem(TheLocalFileSystem, "TheLocalFileSystem", createLocalFileSystem(), nullptr);


    	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheLocalFileSystem  = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		initSubsystem(TheArchiveFileSystem, "TheArchiveFileSystem", createArchiveFileSystem(), nullptr); // this MUST come after TheLocalFileSystem creation

    	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheArchiveFileSystem  = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		DEBUG_ASSERTCRASH(TheWritableGlobalData,("TheWritableGlobalData expected to be created"));
		initSubsystem(TheWritableGlobalData, "TheWritableGlobalData", TheWritableGlobalData, &xferCRC, "Data\\INI\\Default\\GameData", "Data\\INI\\GameData");
		TheWritableGlobalData->parseCustomDefinition();


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After  TheWritableGlobalData = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////



	#if defined(RTS_DEBUG)
		// If we're in Debug, load the Debug settings as well.
		ini.loadFileDirectory( "Data\\INI\\GameDataDebug", INI_LOAD_OVERWRITE, nullptr );
	#endif

		// special-case: parse command-line parameters after loading global data
		CommandLine::parseCommandLineForEngineInit();

		TheArchiveFileSystem->loadMods();

		// doesn't require resets so just create a single instance here.
		TheGameLODManager = MSGNEW("GameEngineSubsystem") GameLODManager;
		TheGameLODManager->init();

		// after parsing the command line, we may want to perform dds stuff. Do that here.
		if (TheGlobalData->m_shouldUpdateTGAToDDS) {
			// update any out of date targas here.
			updateTGAtoDDS();
		}

		// read the water settings from INI (must do prior to initing GameClient, apparently)
		ini.loadFileDirectory( "Data\\INI\\Default\\Water", INI_LOAD_OVERWRITE, &xferCRC );
		ini.loadFileDirectory( "Data\\INI\\Water", INI_LOAD_OVERWRITE, &xferCRC );
		ini.loadFileDirectory( "Data\\INI\\Default\\Weather", INI_LOAD_OVERWRITE, &xferCRC );
		ini.loadFileDirectory( "Data\\INI\\Weather", INI_LOAD_OVERWRITE, &xferCRC );



	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After water INI's = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


#ifdef DEBUG_CRC
		initSubsystem(TheDeepCRCSanityCheck, "TheDeepCRCSanityCheck", MSGNEW("GameEngineSubystem") DeepCRCSanityCheck, nullptr);
#endif // DEBUG_CRC
		initSubsystem(TheGameText, "TheGameText", CreateGameTextInterface(), nullptr);
		updateWindowTitle();

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheGameText = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


#if RETAIL_COMPATIBLE_CRC
		if (xferCRC.getCRC() == 0xA1E7F8E6)
			TheNameKeyGenerator->verifyNameKeyID(1);
#endif

		initSubsystem(TheScienceStore,"TheScienceStore", MSGNEW("GameEngineSubsystem") ScienceStore(), &xferCRC, "Data\\INI\\Default\\Science", "Data\\INI\\Science");
		initSubsystem(TheMultiplayerSettings,"TheMultiplayerSettings", MSGNEW("GameEngineSubsystem") MultiplayerSettings(), &xferCRC, "Data\\INI\\Default\\Multiplayer", "Data\\INI\\Multiplayer");
		initSubsystem(TheTerrainTypes,"TheTerrainTypes", MSGNEW("GameEngineSubsystem") TerrainTypeCollection(), &xferCRC, "Data\\INI\\Default\\Terrain", "Data\\INI\\Terrain");
		initSubsystem(TheTerrainRoads,"TheTerrainRoads", MSGNEW("GameEngineSubsystem") TerrainRoadCollection(), &xferCRC, "Data\\INI\\Default\\Roads", "Data\\INI\\Roads");
		initSubsystem(TheGlobalLanguageData,"TheGlobalLanguageData",MSGNEW("GameEngineSubsystem") GlobalLanguage, nullptr); // must be before the game text
		TheGlobalLanguageData->parseCustomDefinition();
	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheGlobalLanguageData = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////
		initSubsystem(TheAudio,"TheAudio", createAudioManager(TheGlobalData->m_headless), nullptr);

#if RTS_ZEROHOUR && RETAIL_COMPATIBLE_CRC
		TheNameKeyGenerator->syncNameKeyID();
#endif

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheAudio = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		initSubsystem(TheFunctionLexicon,"TheFunctionLexicon", createFunctionLexicon(), nullptr);
		initSubsystem(TheModuleFactory,"TheModuleFactory", createModuleFactory(), nullptr);
		initSubsystem(TheMessageStream,"TheMessageStream", createMessageStream(), nullptr);
		initSubsystem(TheSidesList,"TheSidesList", MSGNEW("GameEngineSubsystem") SidesList(), nullptr);
		initSubsystem(TheCaveSystem,"TheCaveSystem", MSGNEW("GameEngineSubsystem") CaveSystem(), nullptr);
		initSubsystem(TheRankInfoStore,"TheRankInfoStore", MSGNEW("GameEngineSubsystem") RankInfoStore(), &xferCRC, nullptr, "Data\\INI\\Rank");
		initSubsystem(ThePlayerTemplateStore,"ThePlayerTemplateStore", MSGNEW("GameEngineSubsystem") PlayerTemplateStore(), &xferCRC, "Data\\INI\\Default\\PlayerTemplate", "Data\\INI\\PlayerTemplate");
		initSubsystem(TheParticleSystemManager,"TheParticleSystemManager", createParticleSystemManager(TheGlobalData->m_headless), nullptr);

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheParticleSystemManager = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		initSubsystem(TheFXListStore,"TheFXListStore", MSGNEW("GameEngineSubsystem") FXListStore(), &xferCRC, "Data\\INI\\Default\\FXList", "Data\\INI\\FXList");
		initSubsystem(TheWeaponStore,"TheWeaponStore", MSGNEW("GameEngineSubsystem") WeaponStore(), &xferCRC, nullptr, "Data\\INI\\Weapon");
		initSubsystem(TheObjectCreationListStore,"TheObjectCreationListStore", MSGNEW("GameEngineSubsystem") ObjectCreationListStore(), &xferCRC, "Data\\INI\\Default\\ObjectCreationList", "Data\\INI\\ObjectCreationList");
		initSubsystem(TheLocomotorStore,"TheLocomotorStore", MSGNEW("GameEngineSubsystem") LocomotorStore(), &xferCRC, nullptr, "Data\\INI\\Locomotor");
		initSubsystem(TheSpecialPowerStore,"TheSpecialPowerStore", MSGNEW("GameEngineSubsystem") SpecialPowerStore(), &xferCRC, "Data\\INI\\Default\\SpecialPower", "Data\\INI\\SpecialPower");
		initSubsystem(TheDamageFXStore,"TheDamageFXStore", MSGNEW("GameEngineSubsystem") DamageFXStore(), &xferCRC, nullptr, "Data\\INI\\DamageFX");
		initSubsystem(TheArmorStore,"TheArmorStore", MSGNEW("GameEngineSubsystem") ArmorStore(), &xferCRC, nullptr, "Data\\INI\\Armor");
		initSubsystem(TheBuildAssistant,"TheBuildAssistant", MSGNEW("GameEngineSubsystem") BuildAssistant, nullptr);


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheBuildAssistant = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////



		initSubsystem(TheThingFactory,"TheThingFactory", createThingFactory(), &xferCRC, "Data\\INI\\Default\\Object", "Data\\INI\\Object");

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheThingFactory = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


#if RETAIL_COMPATIBLE_CRC
		if (xferCRC.getCRC() == 0x6209AF6E)
			TheNameKeyGenerator->verifyNameKeyID(2265);
#endif

		initSubsystem(TheUpgradeCenter,"TheUpgradeCenter", MSGNEW("GameEngineSubsystem") UpgradeCenter, &xferCRC, "Data\\INI\\Default\\Upgrade", "Data\\INI\\Upgrade");
		initSubsystem(TheGameClient,"TheGameClient", createGameClient(), nullptr);


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheGameClient = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		initSubsystem(TheAI,"TheAI", MSGNEW("GameEngineSubsystem") AI(), &xferCRC,  "Data\\INI\\Default\\AIData", "Data\\INI\\AIData");
		initSubsystem(TheGameLogic,"TheGameLogic", createGameLogic(), nullptr);
		initSubsystem(TheTeamFactory,"TheTeamFactory", MSGNEW("GameEngineSubsystem") TeamFactory(), nullptr);
		initSubsystem(TheCrateSystem,"TheCrateSystem", MSGNEW("GameEngineSubsystem") CrateSystem(), &xferCRC, "Data\\INI\\Default\\Crate", "Data\\INI\\Crate");
		initSubsystem(ThePlayerList,"ThePlayerList", MSGNEW("GameEngineSubsystem") PlayerList(), nullptr);
		initSubsystem(TheRecorder,"TheRecorder", createRecorder(), nullptr);
		initSubsystem(TheRadar,"TheRadar", createRadar(TheGlobalData->m_headless), nullptr);
		initSubsystem(TheVictoryConditions,"TheVictoryConditions", createVictoryConditions(), nullptr);



	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheVictoryConditions = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		AsciiString fname;
		fname.format("Data\\%s\\CommandMap", GetRegistryLanguage().str());
		initSubsystem(TheMetaMap,"TheMetaMap", MSGNEW("GameEngineSubsystem") MetaMap(), nullptr, fname.str(), "Data\\INI\\CommandMap");

#if defined(RTS_DEBUG)
		ini.loadFileDirectory("Data\\INI\\CommandMapDebug", INI_LOAD_MULTIFILE, nullptr);
#endif

#if defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
		ini.loadFileDirectory("Data\\INI\\CommandMapDemo", INI_LOAD_MULTIFILE, nullptr);
#endif

		TheMetaMap->generateMetaMap();
		TheMetaMap->verifyMetaMap();


		initSubsystem(TheActionManager,"TheActionManager", MSGNEW("GameEngineSubsystem") ActionManager(), nullptr);
		//initSubsystem((CComObject<WebBrowser> *)TheWebBrowser,"(CComObject<WebBrowser> *)TheWebBrowser", (CComObject<WebBrowser> *)createWebBrowser(), nullptr);
		initSubsystem(TheGameStateMap,"TheGameStateMap", MSGNEW("GameEngineSubsystem") GameStateMap, nullptr );
		initSubsystem(TheGameState,"TheGameState", MSGNEW("GameEngineSubsystem") GameState, nullptr );

		// Create the interface for sending game results
		initSubsystem(TheGameResultsQueue,"TheGameResultsQueue", GameResultsInterface::createNewGameResultsInterface(), nullptr);


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheGameResultsQueue = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		xferCRC.close();
		TheWritableGlobalData->m_iniCRC = xferCRC.getCRC();
		DEBUG_LOG(("INI CRC is 0x%8.8X", TheGlobalData->m_iniCRC));

		TheSubsystemList->postProcessLoadAll();

		TheFramePacer->setFramesPerSecondLimit(TheGlobalData->m_framesPerSecondLimit);

		TheAudio->setOn(TheGlobalData->m_audioOn && TheGlobalData->m_musicOn, AudioAffect_Music);
		TheAudio->setOn(TheGlobalData->m_audioOn && TheGlobalData->m_soundsOn, AudioAffect_Sound);
		TheAudio->setOn(TheGlobalData->m_audioOn && TheGlobalData->m_sounds3DOn, AudioAffect_Sound3D);
		TheAudio->setOn(TheGlobalData->m_audioOn && TheGlobalData->m_speechOn, AudioAffect_Speech);

		// We're not in a network game yet, so set the network singleton to nullptr.
		TheNetwork = nullptr;

		//Create a default ini file for options if it doesn't already exist.
		//OptionPreferences prefs( TRUE );

		// If we turn m_quitting to FALSE here, then we throw away any requests to quit that
		// took place during loading. :-\ - jkmcd
		// If this really needs to take place, please make sure that pressing cancel on the audio
		// load music dialog will still cause the game to quit.
		// m_quitting = FALSE;

		// initialize the MapCache
		TheMapCache = MSGNEW("GameEngineSubsystem") MapCache;
		TheMapCache->updateCache();


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheMapCache->updateCache = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		if (TheGlobalData->m_buildMapCache)
		{
			// just quit, since the map cache has already updated
			//populateMapListbox(nullptr, true, true);
			m_quitting = TRUE;
		}

		// load the initial shell screen
		//TheShell->push( "Menus/MainMenu.wnd" );

		// This allows us to run a map from the command line
		if (TheGlobalData->m_initialFile.isEmpty() == FALSE)
		{
			AsciiString fname = TheGlobalData->m_initialFile;
			fname.toLower();

			if (fname.endsWithNoCase(".map"))
			{
				TheWritableGlobalData->m_shellMapOn = FALSE;
				TheWritableGlobalData->m_playIntro = FALSE;
				TheWritableGlobalData->m_pendingFile = TheGlobalData->m_initialFile;

				// shutdown the top, but do not pop it off the stack
	//			TheShell->hideShell();

				// send a message to the logic for a new game
				GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_NEW_GAME );
				msg->appendIntegerArgument(GAME_SINGLE_PLAYER);
				msg->appendIntegerArgument(DIFFICULTY_NORMAL);
				msg->appendIntegerArgument(0);
				InitRandom(0);
			}
		}

		//
		if (TheMapCache && TheGlobalData->m_shellMapOn)
		{
			AsciiString lowerName = TheGlobalData->m_shellMapName;
			lowerName.toLower();

			MapCache::const_iterator it = TheMapCache->find(lowerName);
			if (it == TheMapCache->end())
			{
				TheWritableGlobalData->m_shellMapOn = FALSE;
			}
		}

	}
	catch (ErrorCode ec)
	{
		if (ec == ERROR_INVALID_D3D)
		{
			RELEASE_CRASHLOCALIZED("ERROR:D3DFailurePrompt", "ERROR:D3DFailureMessage");
		}
	}
	catch (INIException e)
	{
		if (e.mFailureMessage)
			RELEASE_CRASH((e.mFailureMessage));
		else
			RELEASE_CRASH(("Uncaught Exception during initialization."));

	}
	catch (...)
	{
		RELEASE_CRASH(("Uncaught Exception during initialization."));
	}

	resetSubsystems();

	HideControlBar();
}

/** -----------------------------------------------------------------------------------------------
	* Reset all necessary parts of the game engine to be ready to accept new game data
	*/
void GameEngine::reset()
{
	ASSERT_GAME_THREAD("GameEngine::reset");

	WindowLayout *background = TheWindowManager->winCreateLayout("Menus/BlankWindow.wnd");
	DEBUG_ASSERTCRASH(background,("We Couldn't Load Menus/BlankWindow.wnd"));
	background->hide(FALSE);
	background->bringForward();
	background->getFirstWindow()->winClearStatus(WIN_STATUS_IMAGE);
	Bool deleteNetwork = false;
	if (TheGameLogic->isInMultiplayerGame())
		deleteNetwork = true;

	resetSubsystems();

	if (deleteNetwork)
	{
		DEBUG_ASSERTCRASH(TheNetwork, ("Deleting null TheNetwork!"));
		delete TheNetwork;
		TheNetwork = nullptr;
	}
	if(background)
	{
		background->destroyWindows();
		deleteInstance(background);
		background = nullptr;
	}
}

/// -----------------------------------------------------------------------------------------------
void GameEngine::resetSubsystems()
{
	// TheSuperHackers @fix xezon 09/06/2025 Reset GameLogic first to purge all world objects early.
	// This avoids potentially catastrophic issues when objects and subsystems have cross dependencies.
	TheGameLogic->reset();

	TheSubsystemList->resetAll();
}

/// -----------------------------------------------------------------------------------------------
Bool GameEngine::canUpdateGameLogic(UnsignedInt logicTimeQueryFlags)
{
	// This updates the paused game status of the game logic.
	TheGameLogic->preUpdate();

	TheFramePacer->setTimeFrozen(isTimeFrozen());
	TheFramePacer->setGameHalted(isGameHalted());

	if (TheNetwork != nullptr)
	{
		return canUpdateNetworkGameLogic();
	}
	else
	{
		return canUpdateRegularGameLogic(logicTimeQueryFlags);
	}
}

/// -----------------------------------------------------------------------------------------------
Bool GameEngine::canUpdateNetworkGameLogic()
{
	DEBUG_ASSERTCRASH(TheNetwork != nullptr, ("TheNetwork is null"));

	if (TheNetwork->isFrameDataReady())
	{
		// Important: The Network is definitely no longer stalling.
		TheFramePacer->setGameHalted(false);

		return true;
	}

	return false;
}

/// -----------------------------------------------------------------------------------------------
Bool GameEngine::canUpdateRegularGameLogic(UnsignedInt logicTimeQueryFlags)
{
	const Int logicTimeScaleFps = TheFramePacer->getActualLogicTimeScaleFps(logicTimeQueryFlags);

	if (logicTimeScaleFps <= 0)
	{
		return false;
	}

	const Int maxRenderFps = TheFramePacer->getActualFramesPerSecondLimit();

#if defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
	const Bool useFastMode = TheGlobalData->m_TiVOFastMode;
#else	//always allow this cheat key if we're in a replay game.
	const Bool useFastMode = TheGlobalData->m_TiVOFastMode && TheGameLogic->isInReplayGame();
#endif

	if (useFastMode || logicTimeScaleFps >= maxRenderFps)
	{
		// Logic time scale is uncapped or larger equal Render FPS. Update straight away.
		return true;
	}
	else
	{
		// TheSuperHackers @tweak xezon 06/08/2025
		// The logic time step is now decoupled from the render update.
		const Real targetFrameTime = 1.0f / logicTimeScaleFps;
		m_logicTimeAccumulator += min(TheFramePacer->getUpdateTime(), targetFrameTime);

		if (m_logicTimeAccumulator >= targetFrameTime)
		{
			m_logicTimeAccumulator -= targetFrameTime;
			return true;
		}
	}

	return false;
}

/// -----------------------------------------------------------------------------------------------
DECLARE_PERF_TIMER(GameEngine_update)

/** -----------------------------------------------------------------------------------------------
 * Update the game engine by updating the GameClient and GameLogic singletons.
 */
void GameEngine::update()
{
	ASSERT_GAME_THREAD("GameEngine::update");
	PROFILER_SECTION_NAME("Engine.Update");
	USE_PERF_TIMER(GameEngine_update)
	{
		{
			// VERIFY CRC needs to be in this code block.  Please to not pull TheGameLogic->update() inside this block.
			VERIFY_CRC

			{
				PROFILER_SECTION_NAME("Engine.Update.Radar");
				rts::frame_timing::Scope frameTiming(rts::frame_timing::Radar);
				TheRadar->UPDATE();
			}

			/// @todo Move audio init, update, etc, into GameClient update

			{
				PROFILER_SECTION_NAME("Engine.Update.Audio");
				rts::frame_timing::Scope frameTiming(rts::frame_timing::Audio);
				TheAudio->UPDATE();
			}
			{
				PROFILER_SECTION_NAME("Engine.Update.Client");
				rts::frame_timing::Scope frameTiming(rts::frame_timing::Client);
				TheGameClient->UPDATE();
			}
			{
				PROFILER_SECTION_NAME("Engine.Update.MessageStream");
				rts::frame_timing::Scope frameTiming(rts::frame_timing::Messages);
				TheMessageStream->propagateMessages();
			}

			{
				PROFILER_SECTION_NAME("Engine.Update.Network");
				rts::frame_timing::Scope frameTiming(rts::frame_timing::Network);
				if (TheNetwork != nullptr)
				{
					TheNetwork->UPDATE();
				}
			}
		}

		// TheSuperHackers @info Ignores frozen time because the script engine needs updating in the logic update regardless.
		if (canUpdateGameLogic(FramePacer::IgnoreFrozenTime))
		{
			{
				PROFILER_SECTION_NAME("Engine.Update.GameLogic");
				rts::frame_timing::Scope frameTiming(rts::frame_timing::Logic);
				TheGameLogic->UPDATE();
			}

			if (!TheFramePacer->isTimeFrozen())
			{
				PROFILER_SECTION_NAME("Engine.Update.ClientStep");
				rts::frame_timing::Scope frameTiming(rts::frame_timing::ClientStep);
				TheGameClient->step();
			}
			startHeadlessSimulationJobsAfterUnsafeInitialization();
		}
	}
}

extern HWND ApplicationHWnd;

/** -----------------------------------------------------------------------------------------------
 * The "main loop" of the game engine. It will not return until the game exits.
 */
Bool GameEngine::prepareHeadlessSimulationJobsForInstalledQualification()
{
	startHeadlessSimulationJobsAfterUnsafeInitialization();
	rts::JobSystem &jobs = rts::JobSystem::instance();
	return TheGlobalData->m_headless && jobs.isRunning() &&
		jobs.isCurrentThread(rts::JOB_OWNER_GAME) && jobs.workerCount() >= 2U &&
		rts::GetSimulationExecutionMode() == rts::SIMULATION_EXECUTION_PARALLEL;
}

void GameEngine::execute()
{
	ASSERT_GAME_THREAD("GameEngine::execute");
	rts::frame_timing::Session frameTimingSession(
		TheGlobalData != 0 && TheGlobalData->m_headless ? "headless" : "interactive");
#if defined(RTS_DEBUG)
	DWORD startTime = timeGetTime() / 1000;
#endif

	// pretty basic for now
	while( !m_quitting )
	{
		rts::frame_timing::BeginFrame(TheGameLogic->getFrame());

		//if (TheGlobalData->m_vTune)
		{
#ifdef PERF_TIMERS
			PerfGather::resetAll();
#endif
		}

		{

#if defined(RTS_DEBUG)
			{
				// enter only if in benchmark mode
				if (TheGlobalData->m_benchmarkTimer > 0)
				{
					DWORD currentTime = timeGetTime() / 1000;
					if (TheGlobalData->m_benchmarkTimer < currentTime - startTime)
					{
						if (TheGameLogic->isInGame())
						{
							if (TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
							{
								TheRecorder->stopRecording();
							}
							TheGameLogic->clearGameData();
						}
						TheGameEngine->setQuitting(TRUE);
					}
				}
			}
#endif

			{
				try
				{
					// compute a frame
					update();
					UpdateSkirmishAITestRunner();
				}
				catch (INIException e)
				{
					// Release CRASH doesn't return, so don't worry about executing additional code.
					if (e.mFailureMessage)
						RELEASE_CRASH((e.mFailureMessage));
					else
						RELEASE_CRASH(("Uncaught Exception in GameEngine::update"));
				}
				catch (...)
				{
					// try to save info off
					try
					{
						if (TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_RECORD && TheRecorder->isMultiplayer())
							TheRecorder->cleanUpReplayFile();
					}
					catch (...)
					{
					}
					RELEASE_CRASH(("Uncaught Exception in GameEngine::update"));
				}
			}

			{
				PROFILER_SECTION_NAME("Engine.FramePacer");
				rts::frame_timing::Scope frameTiming(rts::frame_timing::Wait);
				TheFramePacer->update();
			}
		}
		rts::frame_timing::EndFrame(TheGameLogic->getFrame());

#ifdef PERF_TIMERS
		if (!m_quitting && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame() && !TheGameLogic->isGamePaused())
		{
			PerfGather::dumpAll(TheGameLogic->getFrame());
			PerfGather::displayGraph(TheGameLogic->getFrame());
			PerfGather::resetAll();
		}
#endif

	}
}

/** -----------------------------------------------------------------------------------------------
	* Factory for the message stream
	*/
MessageStream *GameEngine::createMessageStream()
{
	// if you change this update the tools that use the engine systems
	// like GUIEdit, it creates a message stream to run in "test" mode
	return MSGNEW("GameEngineSubsystem") MessageStream;
}

//-------------------------------------------------------------------------------------------------
FileSystem *GameEngine::createFileSystem()
{
	return MSGNEW("GameEngineSubsystem") FileSystem;
}

//-------------------------------------------------------------------------------------------------
Bool GameEngine::isMultiplayerSession()
{
	return TheRecorder->isMultiplayer();
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
#define CONVERT_EXEC1	"..\\Build\\nvdxt -list buildDDS.txt -dxt5 -full -outdir Art\\Textures > buildDDS.out"

void updateTGAtoDDS()
{
	// Here's the scoop. We're going to traverse through all of the files in the Art\Textures folder
	// and determine if there are any .tga files that are newer than associated .dds files. If there
	// are, then we will re-run the compression tool on them.

	File *fp = TheLocalFileSystem->openFile("buildDDS.txt", File::WRITE | File::CREATE | File::TRUNCATE | File::TEXT);
	if (!fp) {
		return;
	}

	FilenameList files;
	TheLocalFileSystem->getFileListInDirectory("Art\\Textures\\", "", "*.tga", files, TRUE);
	FilenameList::iterator it;
	for (it = files.begin(); it != files.end(); ++it) {
		AsciiString filenameTGA = *it;
		AsciiString filenameDDS = *it;
		FileInfo infoTGA;
		TheLocalFileSystem->getFileInfo(filenameTGA, &infoTGA);

		// skip the water textures, since they need to be NOT compressed
		filenameTGA.toLower();
		if (strstr(filenameTGA.str(), "caust"))
		{
			continue;
		}
		// and the recolored stuff.
		if (strstr(filenameTGA.str(), "zhca"))
		{
			continue;
		}

		// replace tga with dds
		filenameDDS.truncateBy(3); // tga
		filenameDDS.concat("dds");

		Bool needsToBeUpdated = FALSE;
		FileInfo infoDDS;
		if (TheFileSystem->doesFileExist(filenameDDS.str())) {
			TheFileSystem->getFileInfo(filenameDDS, &infoDDS);
			if (infoTGA.timestampHigh > infoDDS.timestampHigh ||
					(infoTGA.timestampHigh == infoDDS.timestampHigh &&
					 infoTGA.timestampLow > infoDDS.timestampLow)) {
				needsToBeUpdated = TRUE;
			}
		} else {
			needsToBeUpdated = TRUE;
		}

		if (!needsToBeUpdated) {
			continue;
		}

		filenameTGA.concat("\n");
		fp->write(filenameTGA.str(), filenameTGA.getLength());
	}

	fp->close();

	system(CONVERT_EXEC1);
}
