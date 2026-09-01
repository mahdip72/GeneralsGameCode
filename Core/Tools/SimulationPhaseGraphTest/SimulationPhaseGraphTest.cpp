/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "Lib/SimulationPhaseGraph.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
using namespace rts;

constexpr unsigned MAX_PHASES = SIMULATION_PHASE_GRAPH_MAX_PHASES;
constexpr unsigned MAX_JOBS = 128;
constexpr unsigned JOBS_PER_PHASE = 8;

int g_failures = 0;

void expect(bool condition, const char *message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << '\n';
		++g_failures;
	}
}

struct OwnerHarness
{
	std::thread::id ownerThread = std::this_thread::get_id();
	SimulationPhaseGraph *graph = nullptr;
	std::vector<unsigned> commits;
	bool wrongThreadCommit = false;
	bool rejectOwnerHelp = false;
	bool reenterValidation = false;
	bool reenterCommit = false;
	bool nestedAdvanceResult = true;
	SimulationPhaseId validationFaultPhase = SIMULATION_PHASE_INVALID_ID;
	SimulationPhaseJobKey validationFaultKey =
		SIMULATION_PHASE_INVALID_JOB_KEY;
	SimulationPhaseWorkStatus validationFaultStatus =
		SIMULATION_PHASE_WORK_SUCCEEDED;
	std::atomic<bool> *validationEntered = nullptr;
	std::atomic<bool> *validationRelease = nullptr;
	std::atomic<bool> *commitEntered = nullptr;
	std::atomic<bool> *commitRelease = nullptr;
};

struct PhaseInput
{
	int base = 0;
	unsigned spin = 0;
	SimulationPhaseJobKey executionFaultKey =
		SIMULATION_PHASE_INVALID_JOB_KEY;
	SimulationPhaseWorkStatus executionFaultStatus =
		SIMULATION_PHASE_WORK_SUCCEEDED;
	std::atomic<unsigned> *finishCounter = nullptr;
	SimulationPhaseJobKey cooperativeCancelKey =
		SIMULATION_PHASE_INVALID_JOB_KEY;
	std::atomic<bool> *cooperativeEntered = nullptr;
	std::atomic<bool> *executionFaultEntered = nullptr;
	std::atomic<bool> *executionFaultRelease = nullptr;
};

struct JobOutput
{
	int value = 0;
	unsigned finishOrdinal = 0;
};

struct Fixture
{
	OwnerHarness owner;
	std::array<SimulationPhaseNodeStorage, MAX_PHASES> nodes;
	std::array<SimulationPhaseJobStorage, MAX_JOBS> jobs;
	std::array<SimulationPhaseJobResultStorage, MAX_JOBS> results;
	std::array<PhaseInput, MAX_PHASES> inputs;
	std::array<JobOutput, MAX_JOBS> outputs;
	std::atomic<unsigned> finishCounter{ 0 };
	SimulationPhaseGraph graph;

	Fixture() : graph(nodes.data(), static_cast<unsigned>(nodes.size()),
		jobs.data(), static_cast<unsigned>(jobs.size()), results.data(),
		static_cast<unsigned>(results.size()), &Fixture::isOwner, &owner)
	{
		owner.graph = &graph;
	}

	static bool isOwner(void *context)
	{
		return static_cast<OwnerHarness *>(context)->ownerThread ==
			std::this_thread::get_id();
	}
};

struct ExecuteAdmissionGate
{
	std::atomic<bool> entered{ false };
	std::atomic<bool> release{ false };
};

void waitAtExecuteAdmission(void *context)
{
	auto &gate = *static_cast<ExecuteAdmissionGate *>(context);
	gate.entered.store(true, std::memory_order_seq_cst);
	while (!gate.release.load(std::memory_order_seq_cst))
		std::this_thread::yield();
}

SimulationPhaseWorkStatus executeJob(const SimulationPhaseJobContext &context,
	const void *inputBytes, unsigned inputByteCount, void *outputBytes,
	unsigned outputByteCount)
{
	if (inputByteCount != sizeof(PhaseInput) ||
		outputByteCount != sizeof(JobOutput) || inputBytes == nullptr ||
		outputBytes == nullptr)
	{
		return SIMULATION_PHASE_WORK_FAILED;
	}
	const auto &input = *static_cast<const PhaseInput *>(inputBytes);
	auto &output = *static_cast<JobOutput *>(outputBytes);
	if (context.jobKey() == input.cooperativeCancelKey &&
		input.cooperativeEntered != nullptr)
	{
		input.cooperativeEntered->store(true, std::memory_order_seq_cst);
		while (!context.isCancellationRequested())
			std::this_thread::yield();
		return SIMULATION_PHASE_WORK_CANCELLED;
	}
	if (context.jobKey() == input.executionFaultKey &&
		input.executionFaultEntered != nullptr &&
		input.executionFaultRelease != nullptr)
	{
		input.executionFaultEntered->store(true, std::memory_order_seq_cst);
		while (!input.executionFaultRelease->load(std::memory_order_seq_cst))
			std::this_thread::yield();
		return input.executionFaultStatus;
	}
	if (context.jobKey() == input.executionFaultKey &&
		input.cooperativeEntered != nullptr)
	{
		while (!input.cooperativeEntered->load(std::memory_order_seq_cst))
			std::this_thread::yield();
		return input.executionFaultStatus;
	}
	for (unsigned spin = 0;
		spin < input.spin * (JOBS_PER_PHASE - (context.jobKey() % JOBS_PER_PHASE));
		++spin)
	{
		if ((spin & 255u) == 0) std::this_thread::yield();
	}
	if (context.isCancellationRequested())
		return SIMULATION_PHASE_WORK_CANCELLED;
	output.value = input.base + static_cast<int>(context.phaseId() * 100u) +
		static_cast<int>(context.jobKey());
	output.finishOrdinal = input.finishCounter != nullptr ?
		input.finishCounter->fetch_add(1, std::memory_order_relaxed) : 0;
	if (context.jobKey() == input.executionFaultKey)
		return input.executionFaultStatus;
	return SIMULATION_PHASE_WORK_SUCCEEDED;
}

SimulationPhaseWorkStatus validateJob(
	const SimulationPhaseCommitContext &context, const void *inputBytes,
	unsigned inputByteCount, const void *outputBytes,
	unsigned outputByteCount, void *ownerContext)
{
	auto &owner = *static_cast<OwnerHarness *>(ownerContext);
	if (owner.ownerThread != std::this_thread::get_id() ||
		inputByteCount != sizeof(PhaseInput) ||
		outputByteCount != sizeof(JobOutput) || inputBytes == nullptr ||
		outputBytes == nullptr)
	{
		return SIMULATION_PHASE_WORK_FAILED;
	}
	if (owner.rejectOwnerHelp &&
		context.executionIdentity().kind() ==
			SIMULATION_PHASE_EXECUTION_OWNER_HELP)
	{
		return SIMULATION_PHASE_WORK_FAILED;
	}
	if (owner.reenterValidation && owner.graph != nullptr)
	{
		owner.reenterValidation = false;
		owner.nestedAdvanceResult = owner.graph->advanceOwner();
	}
	if (owner.validationFaultPhase == context.phaseId() &&
		owner.validationFaultKey == context.jobKey())
	{
		return owner.validationFaultStatus;
	}
	if (owner.validationEntered != nullptr &&
		owner.validationRelease != nullptr)
	{
		owner.validationEntered->store(true, std::memory_order_seq_cst);
		while (!owner.validationRelease->load(std::memory_order_seq_cst))
			std::this_thread::yield();
	}
	const auto &input = *static_cast<const PhaseInput *>(inputBytes);
	const auto &output = *static_cast<const JobOutput *>(outputBytes);
	const int expected = input.base +
		static_cast<int>(context.phaseId() * 100u) +
		static_cast<int>(context.jobKey());
	return output.value == expected ? SIMULATION_PHASE_WORK_SUCCEEDED :
		SIMULATION_PHASE_WORK_FAILED;
}

void commitJob(const SimulationPhaseCommitContext &context, const void *,
	unsigned, const void *, unsigned, void *ownerContext)
{
	auto &owner = *static_cast<OwnerHarness *>(ownerContext);
	if (owner.ownerThread != std::this_thread::get_id())
		owner.wrongThreadCommit = true;
	if (owner.reenterCommit && owner.graph != nullptr)
	{
		owner.reenterCommit = false;
		owner.nestedAdvanceResult = owner.graph->advanceOwner();
	}
	if (owner.commitEntered != nullptr && owner.commitRelease != nullptr)
	{
		owner.commitEntered->store(true, std::memory_order_seq_cst);
		while (!owner.commitRelease->load(std::memory_order_seq_cst))
			std::this_thread::yield();
	}
	owner.commits.push_back(context.phaseId() * 1000u + context.jobKey());
}

SimulationPhaseJobDefinition jobDefinition(SimulationPhaseId phaseId,
	SimulationPhaseJobKey key, JobOutput &output)
{
	SimulationPhaseJobDefinition job = {};
	job.phaseId = phaseId;
	job.key = key;
	job.privateOutput = &output;
	job.privateOutputBytes = sizeof(output);
	job.execute = executeJob;
	job.validate = validateJob;
	job.commit = commitJob;
	return job;
}

void initializeInput(Fixture &fixture, unsigned index, int base)
{
	fixture.inputs[index].base = base;
	fixture.inputs[index].spin = 128;
	fixture.inputs[index].executionFaultKey =
		SIMULATION_PHASE_INVALID_JOB_KEY;
	fixture.inputs[index].executionFaultStatus =
		SIMULATION_PHASE_WORK_SUCCEEDED;
	fixture.inputs[index].finishCounter = &fixture.finishCounter;
	fixture.inputs[index].cooperativeCancelKey =
		SIMULATION_PHASE_INVALID_JOB_KEY;
	fixture.inputs[index].cooperativeEntered = nullptr;
	fixture.inputs[index].executionFaultEntered = nullptr;
	fixture.inputs[index].executionFaultRelease = nullptr;
}

SimulationPhaseGraphConfigurationStatus configureCanonicalGraph(Fixture &fixture)
{
	initializeInput(fixture, 0, 11);
	initializeInput(fixture, 1, 22);
	initializeInput(fixture, 2, 33);
	initializeInput(fixture, 3, 44);
	const SimulationPhaseId dependency10[] = { 10 };
	const SimulationPhaseId dependencies20And30[] = { 20, 30 };
	SimulationPhaseDefinition phases[4] = {};
	phases[0] = { 40, dependencies20And30, 2, &fixture.inputs[3],
		sizeof(PhaseInput) };
	phases[1] = { 30, dependency10, 1, &fixture.inputs[2],
		sizeof(PhaseInput) };
	phases[2] = { 10, nullptr, 0, &fixture.inputs[0], sizeof(PhaseInput) };
	phases[3] = { 20, dependency10, 1, &fixture.inputs[1],
		sizeof(PhaseInput) };

	const SimulationPhaseId phaseIds[] = { 10, 20, 30, 40 };
	SimulationPhaseJobDefinition definitions[4 * JOBS_PER_PHASE] = {};
	unsigned source = 0;
	for (unsigned phase = 0; phase < 4; ++phase)
	{
		for (unsigned key = 0; key < JOBS_PER_PHASE; ++key)
		{
			const unsigned destination = 4 * JOBS_PER_PHASE - source - 1;
			definitions[destination] = jobDefinition(phaseIds[phase], key,
				fixture.outputs[source]);
			++source;
		}
	}
	return fixture.graph.configure(phases, 4, definitions,
		static_cast<unsigned>(std::size(definitions)));
}

SimulationPhaseGraphConfigurationStatus configureSinglePhase(Fixture &fixture,
	unsigned jobCount)
{
	initializeInput(fixture, 0, 5);
	SimulationPhaseDefinition phase = { 5, nullptr, 0, &fixture.inputs[0],
		sizeof(PhaseInput) };
	std::array<SimulationPhaseJobDefinition, MAX_JOBS> definitions = {};
	for (unsigned index = 0; index < jobCount; ++index)
	{
		definitions[jobCount - index - 1] = jobDefinition(5, index,
			fixture.outputs[index]);
	}
	return fixture.graph.configure(&phase, 1, definitions.data(), jobCount);
}

SimulationPhaseGraphConfigurationStatus configureDiagnosticShape(
	Fixture &fixture, bool twoPhases)
{
	initializeInput(fixture, 0, 5);
	SimulationPhaseDefinition phases[2] = {
		{ 5, nullptr, 0, &fixture.inputs[0], sizeof(PhaseInput) },
		{}
	};
	SimulationPhaseJobDefinition definitions[2] = {
		jobDefinition(5, 0, fixture.outputs[0]),
		{}
	};
	unsigned phaseCount = 1;
	unsigned jobCount = 1;
	if (twoPhases)
	{
		initializeInput(fixture, 1, 10);
		phases[0] = { 10, nullptr, 0, &fixture.inputs[0],
			sizeof(PhaseInput) };
		phases[1] = { 20, nullptr, 0, &fixture.inputs[1],
			sizeof(PhaseInput) };
		definitions[0] = jobDefinition(10, 0, fixture.outputs[0]);
		definitions[1] = jobDefinition(20, 0, fixture.outputs[1]);
		phaseCount = 2;
		jobCount = 2;
	}
	return fixture.graph.configure(phases, phaseCount, definitions, jobCount);
}

bool executeUntilTerminal(Fixture &fixture, unsigned workerCount)
{
	for (unsigned iteration = 0; iteration < 64; ++iteration)
	{
		if (!fixture.graph.advanceOwner()) return false;
		const SimulationPhaseGraphState state = fixture.graph.state();
		if (state == SIMULATION_PHASE_GRAPH_COMPLETED)
			return true;
		if (state == SIMULATION_PHASE_GRAPH_CANCELLED ||
			state == SIMULATION_PHASE_GRAPH_FAILED ||
			state == SIMULATION_PHASE_GRAPH_STALE_GENERATION)
		{
			return false;
		}

		std::vector<SimulationPhaseJobTicket> tickets;
		SimulationPhaseJobTicket ticket;
		while (fixture.graph.tryClaimReadyJob(ticket))
			tickets.push_back(ticket);
		if (tickets.empty()) return false;

		std::atomic<unsigned> cursor{ 0 };
		std::atomic<unsigned> failed{ 0 };
		std::vector<std::thread> workers;
		workers.reserve(workerCount);
		for (unsigned worker = 0; worker < workerCount; ++worker)
		{
			workers.emplace_back([&, worker]() {
				for (;;)
				{
					const unsigned index = cursor.fetch_add(1,
						std::memory_order_relaxed);
					if (index >= tickets.size()) break;
					if (fixture.graph.executeClaimedJob(tickets[index],
						SimulationPhaseExecutionIdentity::physicalWorker(worker)) !=
						SIMULATION_PHASE_WORK_SUCCEEDED)
					{
						failed.fetch_add(1, std::memory_order_relaxed);
					}
				}
			});
		}
		for (auto &worker : workers) worker.join();
		if (failed.load(std::memory_order_relaxed) != 0) return false;
	}
	return false;
}

void executeAllClaimed(Fixture &fixture,
	const SimulationPhaseExecutionIdentity &identity)
{
	SimulationPhaseJobTicket ticket;
	while (fixture.graph.tryClaimReadyJob(ticket))
		fixture.graph.executeClaimedJob(ticket, identity);
}

void testStableGraphAndWorkerCounts()
{
	const unsigned workerCounts[] = { 1, 2, 4, 8, 16 };
	std::vector<unsigned> baselineCommits;
	for (unsigned workerCount : workerCounts)
	{
		Fixture fixture;
		expect(configureCanonicalGraph(fixture) ==
			SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID,
			"canonical graph configures");
		expect(fixture.graph.phaseIdAt(0) == 10 &&
			fixture.graph.phaseIdAt(1) == 20 &&
			fixture.graph.phaseIdAt(2) == 30 &&
			fixture.graph.phaseIdAt(3) == 40,
			"acyclic graph uses stable phase-ID topological order");
		expect(fixture.graph.reset(100 + workerCount),
			"canonical graph resets for worker-count frame");
		expect(executeUntilTerminal(fixture, workerCount),
			"canonical graph completes on requested physical workers");
		expect(fixture.graph.isQuiescent(),
			"completed graph has no claimed or running work");
		expect(!fixture.owner.wrongThreadCommit,
			"all canonical commits run on the registered owner");

		std::vector<unsigned> expected;
		for (unsigned phase : { 10u, 20u, 30u, 40u })
			for (unsigned key = 0; key < JOBS_PER_PHASE; ++key)
				expected.push_back(phase * 1000u + key);
		expect(fixture.owner.commits == expected,
			"finish order cannot change stable phase/key commit order");
		if (baselineCommits.empty()) baselineCommits = fixture.owner.commits;
		else expect(fixture.owner.commits == baselineCommits,
			"1/2/4/8/16 workers publish identical canonical completion");

		for (unsigned phase : { 10u, 20u, 30u, 40u })
		{
			for (unsigned key = 0; key < JOBS_PER_PHASE; ++key)
			{
				SimulationPhaseJobSnapshot snapshot;
				expect(fixture.graph.jobSnapshot(phase, key, snapshot) &&
					snapshot.state == SIMULATION_PHASE_JOB_COMMITTED &&
					snapshot.executionIdentity.kind() ==
						SIMULATION_PHASE_EXECUTION_PHYSICAL_WORKER &&
					snapshot.executionIdentity.physicalWorkerIndex() < workerCount,
					"committed job retains bounded physical-worker identity");
			}
		}
	}
}

void testDependenciesAreNonblockingTickets()
{
	Fixture fixture;
	expect(configureCanonicalGraph(fixture) ==
		SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID && fixture.graph.reset(1),
		"dependency ticket fixture starts");
	std::vector<SimulationPhaseJobTicket> root;
	SimulationPhaseJobTicket ticket;
	while (fixture.graph.tryClaimReadyJob(ticket)) root.push_back(ticket);
	expect(root.size() == JOBS_PER_PHASE,
		"only the dependency-ready root phase is claimable");
	expect(!fixture.graph.tryClaimReadyJob(ticket),
		"claim returns immediately instead of waiting for children");
	for (auto iterator = root.rbegin(); iterator != root.rend(); ++iterator)
	{
		expect(fixture.graph.executeClaimedJob(*iterator,
			SimulationPhaseExecutionIdentity::physicalWorker(0)) ==
			SIMULATION_PHASE_WORK_SUCCEEDED,
			"root jobs may finish in reverse order");
	}
	expect(!fixture.graph.tryClaimReadyJob(ticket),
		"children remain blocked until owner publication");
	unsigned committed = 0;
	expect(fixture.graph.advanceOwner(&committed) &&
		committed == JOBS_PER_PHASE,
		"owner commits the complete root phase transactionally");
	expect(fixture.graph.tryClaimReadyJob(ticket) && ticket.phaseId() == 20,
		"stable first child becomes claimable after dependency commit");
	fixture.graph.requestCancellation();
	fixture.graph.advanceOwner();
}

void testCancellationFailureAndStaleFaults()
{
	{
		Fixture fixture;
		configureSinglePhase(fixture, 2);
		fixture.inputs[0].executionFaultKey = 1;
		fixture.inputs[0].executionFaultStatus =
			SIMULATION_PHASE_WORK_FAILED;
		fixture.graph.reset(1);
		executeAllClaimed(fixture,
			SimulationPhaseExecutionIdentity::physicalWorker(0));
		fixture.graph.advanceOwner();
		expect(fixture.graph.state() == SIMULATION_PHASE_GRAPH_FAILED &&
			fixture.owner.commits.empty(),
			"injected worker failure rejects the whole phase before commit");
	}
	{
		Fixture fixture;
		configureSinglePhase(fixture, 2);
		fixture.inputs[0].executionFaultKey = 0;
		fixture.inputs[0].executionFaultStatus =
			SIMULATION_PHASE_WORK_STALE_GENERATION;
		fixture.graph.reset(2);
		executeAllClaimed(fixture,
			SimulationPhaseExecutionIdentity::physicalWorker(0));
		fixture.graph.advanceOwner();
		expect(fixture.graph.state() ==
			SIMULATION_PHASE_GRAPH_STALE_GENERATION &&
			fixture.owner.commits.empty(),
			"injected stale worker result is an explicit terminal state");
	}
	{
		Fixture fixture;
		configureSinglePhase(fixture, 2);
		fixture.owner.validationFaultPhase = 5;
		fixture.owner.validationFaultKey = 1;
		fixture.owner.validationFaultStatus =
			SIMULATION_PHASE_WORK_STALE_GENERATION;
		fixture.graph.reset(3);
		executeAllClaimed(fixture,
			SimulationPhaseExecutionIdentity::physicalWorker(0));
		fixture.graph.advanceOwner();
		expect(fixture.graph.state() ==
			SIMULATION_PHASE_GRAPH_STALE_GENERATION &&
			fixture.owner.commits.empty(),
			"owner stale-generation validation occurs before any phase commit");
	}
	{
		Fixture fixture;
		configureSinglePhase(fixture, 2);
		fixture.graph.reset(4);
		SimulationPhaseJobTicket ticket;
		expect(fixture.graph.tryClaimReadyJob(ticket),
			"cancellation fixture claims work");
		fixture.graph.requestCancellation();
		expect(fixture.graph.executeClaimedJob(ticket,
			SimulationPhaseExecutionIdentity::physicalWorker(0)) ==
			SIMULATION_PHASE_WORK_CANCELLED,
			"claimed work observes cancellation without blocking");
		fixture.graph.advanceOwner();
		expect(fixture.graph.state() == SIMULATION_PHASE_GRAPH_CANCELLED &&
			fixture.owner.commits.empty(),
			"cancelled phase publishes no output");
	}
}

void testWorkerFailureCancelsCooperativePeer()
{
	Fixture fixture;
	expect(configureSinglePhase(fixture, 2) ==
		SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID && fixture.graph.reset(50),
		"peer-cancellation fixture starts");
	std::atomic<bool> cooperativeEntered{ false };
	fixture.inputs[0].cooperativeCancelKey = 0;
	fixture.inputs[0].cooperativeEntered = &cooperativeEntered;
	fixture.inputs[0].executionFaultKey = 1;
	fixture.inputs[0].executionFaultStatus = SIMULATION_PHASE_WORK_FAILED;

	SimulationPhaseJobTicket first;
	SimulationPhaseJobTicket second;
	expect(fixture.graph.tryClaimReadyJob(first) &&
		fixture.graph.tryClaimReadyJob(second),
		"both peer jobs are claimed without dependency waits");
	std::atomic<unsigned> firstStatus{ SIMULATION_PHASE_WORK_SUCCEEDED };
	std::atomic<unsigned> secondStatus{ SIMULATION_PHASE_WORK_SUCCEEDED };
	std::thread firstWorker([&]() {
		firstStatus.store(fixture.graph.executeClaimedJob(first,
			SimulationPhaseExecutionIdentity::physicalWorker(0)),
			std::memory_order_seq_cst);
	});
	std::thread secondWorker([&]() {
		secondStatus.store(fixture.graph.executeClaimedJob(second,
			SimulationPhaseExecutionIdentity::physicalWorker(1)),
			std::memory_order_seq_cst);
	});
	firstWorker.join();
	secondWorker.join();

	expect(firstStatus.load(std::memory_order_seq_cst) ==
		SIMULATION_PHASE_WORK_CANCELLED &&
		secondStatus.load(std::memory_order_seq_cst) ==
		SIMULATION_PHASE_WORK_FAILED,
		"first worker failure immediately releases cooperative peer");
	expect(fixture.graph.terminalCause() == SIMULATION_PHASE_WORK_FAILED &&
		fixture.graph.isQuiescent(),
		"atomic first failure cause is retained before owner advancement");
	expect(fixture.graph.advanceOwner() &&
		fixture.graph.state() == SIMULATION_PHASE_GRAPH_FAILED &&
		fixture.owner.commits.empty(),
		"owner closes failed peer phase without publication");
}

void testOutOfOrderRootFailureAttribution()
{
	Fixture fixture;
	initializeInput(fixture, 0, 10);
	initializeInput(fixture, 1, 20);
	std::atomic<bool> earlierRootEntered{ false };
	fixture.inputs[0].cooperativeCancelKey = 0;
	fixture.inputs[0].cooperativeEntered = &earlierRootEntered;
	fixture.inputs[1].executionFaultKey = 0;
	fixture.inputs[1].executionFaultStatus = SIMULATION_PHASE_WORK_FAILED;
	fixture.inputs[1].cooperativeEntered = &earlierRootEntered;
	SimulationPhaseDefinition phases[2] = {
		{ 20, nullptr, 0, &fixture.inputs[1], sizeof(PhaseInput) },
		{ 10, nullptr, 0, &fixture.inputs[0], sizeof(PhaseInput) }
	};
	SimulationPhaseJobDefinition definitions[2] = {
		jobDefinition(20, 0, fixture.outputs[1]),
		jobDefinition(10, 0, fixture.outputs[0])
	};
	expect(fixture.graph.configure(phases, 2, definitions, 2) ==
		SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID && fixture.graph.reset(55),
		"out-of-order root failure fixture starts");
	SimulationPhaseJobTicket earlierRoot;
	SimulationPhaseJobTicket laterRoot;
	expect(fixture.graph.tryClaimReadyJob(earlierRoot) &&
		fixture.graph.tryClaimReadyJob(laterRoot) &&
		earlierRoot.phaseId() == 10 && laterRoot.phaseId() == 20,
		"independent roots are claimed in canonical phase order");
	std::thread earlierWorker([&]() {
		fixture.graph.executeClaimedJob(earlierRoot,
			SimulationPhaseExecutionIdentity::physicalWorker(0));
	});
	std::thread laterWorker([&]() {
		fixture.graph.executeClaimedJob(laterRoot,
			SimulationPhaseExecutionIdentity::physicalWorker(1));
	});
	earlierWorker.join();
	laterWorker.join();
	expect(fixture.graph.advanceOwner() &&
		fixture.graph.state() == SIMULATION_PHASE_GRAPH_FAILED,
		"later root failure terminates after global quiescence");
	SimulationPhaseJobSnapshot earlierSnapshot;
	SimulationPhaseJobSnapshot laterSnapshot;
	expect(fixture.graph.phaseState(10) ==
			SIMULATION_PHASE_NODE_CANCELLED &&
		fixture.graph.phaseState(20) == SIMULATION_PHASE_NODE_FAILED &&
		fixture.graph.jobSnapshot(10, 0, earlierSnapshot) &&
		earlierSnapshot.state == SIMULATION_PHASE_JOB_CANCELLED &&
		fixture.graph.jobSnapshot(20, 0, laterSnapshot) &&
		laterSnapshot.state == SIMULATION_PHASE_JOB_FAILED &&
		fixture.owner.commits.empty(),
		"actual fault phase/job is failed and earlier root is only cancelled");
}

void testCancellationDuringValidationPublishesNothing()
{
	Fixture fixture;
	expect(configureSinglePhase(fixture, 1) ==
		SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID && fixture.graph.reset(60),
		"validation cancellation fixture starts");
	executeAllClaimed(fixture,
		SimulationPhaseExecutionIdentity::physicalWorker(0));
	std::atomic<bool> validationEntered{ false };
	std::atomic<bool> validationRelease{ false };
	fixture.owner.validationEntered = &validationEntered;
	fixture.owner.validationRelease = &validationRelease;
	std::thread canceller([&]() {
		while (!validationEntered.load(std::memory_order_seq_cst))
			std::this_thread::yield();
		fixture.graph.requestCancellation();
		validationRelease.store(true, std::memory_order_seq_cst);
	});
	unsigned committed = 99;
	expect(fixture.graph.advanceOwner(&committed),
		"owner returns from cancellation-gated validation");
	canceller.join();
	expect(committed == 0 && fixture.owner.commits.empty() &&
		fixture.graph.state() == SIMULATION_PHASE_GRAPH_CANCELLED,
		"cancellation before the commit point publishes zero jobs");
}

void testCompletionCancellationOutcomeGate()
{
	Fixture fixture;
	expect(configureSinglePhase(fixture, 1) ==
		SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID && fixture.graph.reset(65),
		"completion race fixture starts");
	executeAllClaimed(fixture,
		SimulationPhaseExecutionIdentity::physicalWorker(0));
	std::atomic<bool> commitEntered{ false };
	std::atomic<bool> commitRelease{ false };
	fixture.owner.commitEntered = &commitEntered;
	fixture.owner.commitRelease = &commitRelease;
	std::thread canceller([&]() {
		while (!commitEntered.load(std::memory_order_seq_cst))
			std::this_thread::yield();
		fixture.graph.requestCancellation();
		commitRelease.store(true, std::memory_order_seq_cst);
	});
	unsigned committed = 0;
	expect(fixture.graph.advanceOwner(&committed),
		"owner finishes after cancellation races final publication");
	canceller.join();
	expect(fixture.graph.state() == SIMULATION_PHASE_GRAPH_COMPLETED &&
		fixture.graph.terminalCause() == SIMULATION_PHASE_WORK_SUCCEEDED &&
		committed == 1 && fixture.owner.commits.size() == 1,
		"completion winner publishes exactly once and cancellation is a no-op");
	fixture.graph.requestCancellation();
	expect(fixture.graph.state() == SIMULATION_PHASE_GRAPH_COMPLETED &&
		fixture.graph.terminalCause() == SIMULATION_PHASE_WORK_SUCCEEDED &&
		fixture.owner.commits.size() == 1,
		"post-completion cancellation is terminally idempotent");
}

void testExecutionIdentityOwnerGateAndReset()
{
	{
		Fixture fixture;
		configureSinglePhase(fixture, 1);
		fixture.graph.reset(7);
		SimulationPhaseJobTicket ticket;
		fixture.graph.tryClaimReadyJob(ticket);
		expect(fixture.graph.executeClaimedJob(ticket,
			SimulationPhaseExecutionIdentity::ownerHelp()) ==
			SIMULATION_PHASE_WORK_SUCCEEDED,
			"owner help is an explicit valid execution lane");
		fixture.graph.advanceOwner();
		SimulationPhaseJobSnapshot snapshot;
		expect(fixture.graph.jobSnapshot(5, 0, snapshot) &&
			snapshot.executionIdentity.kind() ==
				SIMULATION_PHASE_EXECUTION_OWNER_HELP &&
			snapshot.executionIdentity.physicalWorkerIndex() ==
				SIMULATION_PHASE_INVALID_WORKER_INDEX,
			"owner help cannot masquerade as a physical worker");
	}
	{
		Fixture fixture;
		configureSinglePhase(fixture, 1);
		fixture.owner.rejectOwnerHelp = true;
		fixture.graph.reset(71);
		SimulationPhaseJobTicket ticket;
		fixture.graph.tryClaimReadyJob(ticket);
		fixture.graph.executeClaimedJob(ticket,
			SimulationPhaseExecutionIdentity::ownerHelp());
		fixture.graph.advanceOwner();
		expect(fixture.graph.state() == SIMULATION_PHASE_GRAPH_FAILED &&
			fixture.owner.commits.empty(),
			"owner validation can forbid owner-helped authoritative output");
	}
	{
		Fixture fixture;
		configureSinglePhase(fixture, 1);
		fixture.graph.reset(8);
		SimulationPhaseJobTicket ticket;
		fixture.graph.tryClaimReadyJob(ticket);
		expect(fixture.graph.executeClaimedJob(ticket,
			SimulationPhaseExecutionIdentity()) ==
			SIMULATION_PHASE_WORK_FAILED,
			"missing execution identity fails closed");
		fixture.graph.advanceOwner();
		expect(fixture.graph.state() == SIMULATION_PHASE_GRAPH_FAILED,
			"invalid identity reaches explicit graph failure");
	}
	{
		Fixture fixture;
		configureSinglePhase(fixture, 1);
		fixture.graph.reset(9);
		SimulationPhaseJobTicket ticket;
		fixture.graph.tryClaimReadyJob(ticket);
		fixture.graph.executeClaimedJob(ticket,
			SimulationPhaseExecutionIdentity::physicalWorker(0));
		std::atomic<bool> foreignAdvance{ true };
		std::thread foreign([&]() {
			foreignAdvance.store(fixture.graph.advanceOwner(),
				std::memory_order_relaxed);
		});
		foreign.join();
		expect(!foreignAdvance.load(std::memory_order_relaxed) &&
			fixture.owner.commits.empty(),
			"non-owner cannot run validation or commit callbacks");
		expect(fixture.graph.advanceOwner() &&
			fixture.graph.state() == SIMULATION_PHASE_GRAPH_COMPLETED,
			"registered owner performs canonical commit");
		expect(fixture.graph.reset(10),
			"completed caller storage resets for the next frame");
		expect(fixture.graph.executeClaimedJob(ticket,
			SimulationPhaseExecutionIdentity::physicalWorker(0)) ==
			SIMULATION_PHASE_WORK_STALE_GENERATION,
			"old frame ticket is rejected as stale after reset");
		SimulationPhaseJobSnapshot snapshot;
		expect(fixture.graph.jobSnapshot(5, 0, snapshot) &&
			snapshot.state == SIMULATION_PHASE_JOB_PENDING &&
			snapshot.generation == 10,
			"stale ticket cannot corrupt reused result storage");
		executeAllClaimed(fixture,
			SimulationPhaseExecutionIdentity::physicalWorker(0));
		fixture.graph.advanceOwner();
		expect(fixture.graph.state() == SIMULATION_PHASE_GRAPH_COMPLETED &&
			fixture.owner.commits.size() == 2,
			"reused storage completes a second frame generation");
	}
}

void testInternalEpochAndGenerationABA()
{
	{
		Fixture fixture;
		expect(configureSinglePhase(fixture, 1) ==
			SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID &&
			fixture.graph.internalEpoch() == 1 && fixture.graph.reset(100) &&
			fixture.graph.internalEpoch() == 2,
			"configure and reset each advance the private epoch");
		SimulationPhaseJobTicket oldTicket;
		expect(fixture.graph.tryClaimReadyJob(oldTicket) &&
			oldTicket.generation() == 100 && oldTicket.internalEpoch() == 2,
			"ticket carries public generation and private epoch");
		fixture.graph.executeClaimedJob(oldTicket,
			SimulationPhaseExecutionIdentity::physicalWorker(0));
		fixture.graph.advanceOwner();

		expect(configureSinglePhase(fixture, 1) ==
			SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID &&
			fixture.graph.internalEpoch() == 3,
			"reconfigure advances epoch even with identical phase and key");
		expect(fixture.graph.executeClaimedJob(oldTicket,
			SimulationPhaseExecutionIdentity::physicalWorker(0)) ==
			SIMULATION_PHASE_WORK_STALE_GENERATION,
			"pre-reconfigure ticket cannot survive an identical graph ABA");
		expect(!fixture.graph.reset(100) && !fixture.graph.reset(99),
			"public generation reuse and rollback are rejected");
		expect(fixture.graph.reset(101) && fixture.graph.internalEpoch() == 4,
			"strictly newer public generation starts a fresh epoch");
		SimulationPhaseJobSnapshot snapshot;
		expect(fixture.graph.jobSnapshot(5, 0, snapshot) &&
			snapshot.generation == 101 && snapshot.internalEpoch == 4,
			"result slots carry the same fresh internal epoch");
		SimulationPhaseDefinition livePhase = { 5, nullptr, 0,
			&fixture.inputs[0], sizeof(PhaseInput) };
		SimulationPhaseJobDefinition liveJob =
			jobDefinition(5, 0, fixture.outputs[0]);
		expect(fixture.graph.configure(&livePhase, 1, &liveJob, 1) ==
			SIMULATION_PHASE_GRAPH_INVALID_ARGUMENT &&
			fixture.graph.internalEpoch() == 4,
			"live ready storage cannot be reconfigured into an ABA");
	}
	{
		Fixture fixture;
		expect(configureSinglePhase(fixture, 1) ==
			SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID &&
			fixture.graph.forceInternalEpochExhaustionForTest(),
			"epoch wrap fault injection reaches the terminal boundary");
		expect(!fixture.graph.reset(1) &&
			fixture.graph.state() == SIMULATION_PHASE_GRAPH_FAILED &&
			fixture.graph.terminalCause() == SIMULATION_PHASE_WORK_FAILED,
			"reset fails closed instead of wrapping its private epoch");
		expect(configureSinglePhase(fixture, 1) ==
			SIMULATION_PHASE_GRAPH_EPOCH_EXHAUSTED,
			"reconfigure also refuses an exhausted private epoch");
	}
}

void testStaleExecutorCannotConsumeResetClaim()
{
	Fixture fixture;
	expect(configureSinglePhase(fixture, 1) ==
		SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID && fixture.graph.reset(1),
		"stale executor admission fixture starts");
	SimulationPhaseJobTicket oldTicket;
	expect(fixture.graph.tryClaimReadyJob(oldTicket),
		"old generation ticket is claimed");

	ExecuteAdmissionGate gate;
	setSimulationPhaseExecuteAdmissionHookForTest(waitAtExecuteAdmission, &gate);
	std::atomic<unsigned> oldStatus{ SIMULATION_PHASE_WORK_SUCCEEDED };
	std::thread oldExecutor([&]() {
		oldStatus.store(fixture.graph.executeClaimedJob(oldTicket,
			SimulationPhaseExecutionIdentity::physicalWorker(0)),
			std::memory_order_seq_cst);
	});
	while (!gate.entered.load(std::memory_order_seq_cst))
		std::this_thread::yield();

	fixture.graph.requestCancellation();
	expect(fixture.graph.advanceOwner() &&
		fixture.graph.state() == SIMULATION_PHASE_GRAPH_CANCELLED,
		"cancelled unadmitted claim becomes quiescent");
	expect(fixture.graph.reset(2),
		"new generation reuses the caller-owned slot");
	SimulationPhaseJobTicket newTicket;
	expect(fixture.graph.tryClaimReadyJob(newTicket),
		"new generation owns a fresh claim");

	gate.release.store(true, std::memory_order_seq_cst);
	oldExecutor.join();
	setSimulationPhaseExecuteAdmissionHookForTest(nullptr, nullptr);
	SimulationPhaseJobSnapshot snapshot;
	expect(oldStatus.load(std::memory_order_seq_cst) ==
			SIMULATION_PHASE_WORK_STALE_GENERATION &&
		fixture.graph.jobSnapshot(5, 0, snapshot) &&
		snapshot.state == SIMULATION_PHASE_JOB_CLAIMED &&
		snapshot.generation == 2,
		"paused stale executor cannot consume the reset claim");
	expect(fixture.graph.executeClaimedJob(newTicket,
			SimulationPhaseExecutionIdentity::physicalWorker(1)) ==
			SIMULATION_PHASE_WORK_SUCCEEDED &&
		fixture.graph.advanceOwner() &&
		fixture.graph.state() == SIMULATION_PHASE_GRAPH_COMPLETED &&
		fixture.owner.commits.size() == 1,
		"new generation claim still publishes exactly once");
}

void testAdvanceOwnerReentrancyFailsClosed()
{
	{
		Fixture fixture;
		expect(configureSinglePhase(fixture, 1) ==
			SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID && fixture.graph.reset(20),
			"validation reentrancy fixture starts");
		executeAllClaimed(fixture,
			SimulationPhaseExecutionIdentity::ownerHelp());
		fixture.owner.reenterValidation = true;
		expect(fixture.graph.advanceOwner() &&
			!fixture.owner.nestedAdvanceResult &&
			fixture.graph.state() == SIMULATION_PHASE_GRAPH_FAILED &&
			fixture.owner.commits.empty(),
			"validation reentrancy fails before publication");
	}
	{
		Fixture fixture;
		expect(configureSinglePhase(fixture, 1) ==
			SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID && fixture.graph.reset(21),
			"commit reentrancy fixture starts");
		executeAllClaimed(fixture,
			SimulationPhaseExecutionIdentity::ownerHelp());
		fixture.owner.reenterCommit = true;
		expect(fixture.graph.advanceOwner() &&
			!fixture.owner.nestedAdvanceResult &&
			fixture.graph.state() == SIMULATION_PHASE_GRAPH_FAILED &&
			fixture.owner.commits.size() == 1,
			"commit reentrancy fails without duplicate publication");
	}
}

SimulationPhaseWorkStatus runMixedTerminalOrder(bool staleFirst)
{
	Fixture fixture;
	initializeInput(fixture, 0, 10);
	initializeInput(fixture, 1, 20);
	std::atomic<bool> failedEntered{ false };
	std::atomic<bool> staleEntered{ false };
	std::atomic<bool> failedRelease{ false };
	std::atomic<bool> staleRelease{ false };
	fixture.inputs[0].executionFaultKey = 0;
	fixture.inputs[0].executionFaultStatus = SIMULATION_PHASE_WORK_FAILED;
	fixture.inputs[0].executionFaultEntered = &failedEntered;
	fixture.inputs[0].executionFaultRelease = &failedRelease;
	fixture.inputs[1].executionFaultKey = 0;
	fixture.inputs[1].executionFaultStatus =
		SIMULATION_PHASE_WORK_STALE_GENERATION;
	fixture.inputs[1].executionFaultEntered = &staleEntered;
	fixture.inputs[1].executionFaultRelease = &staleRelease;
	SimulationPhaseDefinition phases[2] = {
		{ 10, nullptr, 0, &fixture.inputs[0], sizeof(PhaseInput) },
		{ 20, nullptr, 0, &fixture.inputs[1], sizeof(PhaseInput) }
	};
	SimulationPhaseJobDefinition definitions[2] = {
		jobDefinition(10, 0, fixture.outputs[0]),
		jobDefinition(20, 0, fixture.outputs[1])
	};
	expect(fixture.graph.configure(phases, 2, definitions, 2) ==
		SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID && fixture.graph.reset(30),
		"mixed terminal fixture starts");
	SimulationPhaseJobTicket failedTicket;
	SimulationPhaseJobTicket staleTicket;
	expect(fixture.graph.tryClaimReadyJob(failedTicket) &&
		fixture.graph.tryClaimReadyJob(staleTicket),
		"mixed terminal jobs are both admitted");
	std::atomic<unsigned> failedStatus{ SIMULATION_PHASE_WORK_SUCCEEDED };
	std::atomic<unsigned> staleStatus{ SIMULATION_PHASE_WORK_SUCCEEDED };
	std::thread failedWorker([&]() {
		failedStatus.store(fixture.graph.executeClaimedJob(failedTicket,
			SimulationPhaseExecutionIdentity::physicalWorker(0)),
			std::memory_order_seq_cst);
	});
	std::thread staleWorker([&]() {
		staleStatus.store(fixture.graph.executeClaimedJob(staleTicket,
			SimulationPhaseExecutionIdentity::physicalWorker(1)),
			std::memory_order_seq_cst);
	});
	while (!failedEntered.load(std::memory_order_seq_cst) ||
		!staleEntered.load(std::memory_order_seq_cst))
	{
		std::this_thread::yield();
	}
	if (staleFirst)
	{
		staleRelease.store(true, std::memory_order_seq_cst);
		staleWorker.join();
		failedRelease.store(true, std::memory_order_seq_cst);
		failedWorker.join();
	}
	else
	{
		failedRelease.store(true, std::memory_order_seq_cst);
		failedWorker.join();
		staleRelease.store(true, std::memory_order_seq_cst);
		staleWorker.join();
	}
	expect(failedStatus.load(std::memory_order_seq_cst) ==
			SIMULATION_PHASE_WORK_FAILED &&
		staleStatus.load(std::memory_order_seq_cst) ==
			SIMULATION_PHASE_WORK_STALE_GENERATION,
		"both mixed terminal statuses are retained");
	const SimulationPhaseWorkStatus terminal = fixture.graph.terminalCause();
	expect(fixture.graph.advanceOwner() &&
		fixture.graph.state() == SIMULATION_PHASE_GRAPH_FAILED &&
		terminal == SIMULATION_PHASE_WORK_FAILED,
		"fixed failure precedence closes the mixed terminal graph");
	return terminal;
}

void testMixedTerminalPrecedenceIsCanonical()
{
	expect(runMixedTerminalOrder(false) == SIMULATION_PHASE_WORK_FAILED &&
		runMixedTerminalOrder(true) == SIMULATION_PHASE_WORK_FAILED,
		"terminal status is independent of worker completion order");
}

void testSnapshotResetPublicationIsConsistent()
{
	Fixture fixture;
	expect(configureSinglePhase(fixture, 1) ==
		SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID && fixture.graph.reset(1),
		"snapshot/reset fixture starts");
	executeAllClaimed(fixture, SimulationPhaseExecutionIdentity::ownerHelp());
	fixture.graph.advanceOwner();
	std::atomic<bool> stop{ false };
	std::atomic<bool> readerStarted{ false };
	std::atomic<bool> observeGraphDiagnostics{ false };
	std::atomic<unsigned> invalidSnapshots{ 0 };
	std::atomic<unsigned> graphObservations{ 0 };
	std::thread reader([&]() {
		readerStarted.store(true, std::memory_order_seq_cst);
		while (!stop.load(std::memory_order_seq_cst))
		{
			SimulationPhaseJobSnapshot snapshot;
			if (!fixture.graph.jobSnapshot(5, 0, snapshot))
			{
				invalidSnapshots.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
			if (snapshot.generation == 0 ||
				snapshot.internalEpoch != snapshot.generation + 1)
			{
				invalidSnapshots.fetch_add(1, std::memory_order_relaxed);
			}
			const bool terminal = snapshot.state ==
					SIMULATION_PHASE_JOB_SUCCEEDED ||
				snapshot.state == SIMULATION_PHASE_JOB_COMMITTED;
			if (terminal != snapshot.executionIdentity.isValid())
				invalidSnapshots.fetch_add(1, std::memory_order_relaxed);
			if (observeGraphDiagnostics.load(std::memory_order_seq_cst))
			{
				const unsigned generationBefore = fixture.graph.generation();
				const unsigned epoch = fixture.graph.internalEpoch();
				const unsigned generationAfter = fixture.graph.generation();
				if (generationBefore == 0 || generationBefore > 128 ||
					generationAfter == 0 || generationAfter > 128 ||
					epoch < 2 || epoch > 129 ||
					(generationBefore == generationAfter &&
					 epoch != generationBefore + 1))
				{
					invalidSnapshots.fetch_add(1,
						std::memory_order_relaxed);
				}
				if (fixture.graph.phaseCount() != 1 ||
					fixture.graph.jobCount() != 1 ||
					fixture.graph.phaseIdAt(0) != 5 ||
					fixture.graph.phaseIdAt(1) !=
						SIMULATION_PHASE_INVALID_ID)
				{
					invalidSnapshots.fetch_add(1,
						std::memory_order_relaxed);
				}
				const SimulationPhaseNodeState phaseState =
					fixture.graph.phaseState(5);
				if (phaseState < SIMULATION_PHASE_NODE_BLOCKED ||
					phaseState > SIMULATION_PHASE_NODE_STALE_GENERATION)
				{
					invalidSnapshots.fetch_add(1,
						std::memory_order_relaxed);
				}
				const unsigned quiescentGenerationBefore =
					fixture.graph.generation();
				const SimulationPhaseGraphState stateBefore =
					fixture.graph.state();
				if (stateBefore != SIMULATION_PHASE_GRAPH_READY &&
					stateBefore != SIMULATION_PHASE_GRAPH_RUNNING &&
					stateBefore != SIMULATION_PHASE_GRAPH_COMPLETED)
				{
					invalidSnapshots.fetch_add(1,
						std::memory_order_relaxed);
				}
				const bool quiescent = fixture.graph.isQuiescent();
				const SimulationPhaseGraphState stateAfter =
					fixture.graph.state();
				const unsigned quiescentGenerationAfter =
					fixture.graph.generation();
				if (quiescentGenerationBefore == quiescentGenerationAfter &&
					stateBefore == SIMULATION_PHASE_GRAPH_COMPLETED &&
					stateAfter == SIMULATION_PHASE_GRAPH_COMPLETED &&
					!quiescent)
				{
					invalidSnapshots.fetch_add(1,
						std::memory_order_relaxed);
				}
				graphObservations.fetch_add(1, std::memory_order_relaxed);
			}
		}
	});
	while (!readerStarted.load(std::memory_order_seq_cst))
		std::this_thread::yield();
	for (unsigned generation = 2; generation <= 128; ++generation)
	{
		if (generation == 2)
			observeGraphDiagnostics.store(true, std::memory_order_seq_cst);
		expect(fixture.graph.reset(generation),
			"snapshot stress generation resets");
		SimulationPhaseJobTicket ticket;
		expect(fixture.graph.tryClaimReadyJob(ticket) &&
			fixture.graph.executeClaimedJob(ticket,
				SimulationPhaseExecutionIdentity::ownerHelp()) ==
				SIMULATION_PHASE_WORK_SUCCEEDED &&
			fixture.graph.advanceOwner(),
			"snapshot stress generation completes");
		if (generation == 2)
		{
			while (graphObservations.load(std::memory_order_relaxed) == 0)
				std::this_thread::yield();
		}
	}
	observeGraphDiagnostics.store(false, std::memory_order_seq_cst);
	stop.store(true, std::memory_order_seq_cst);
	reader.join();
	expect(invalidSnapshots.load(std::memory_order_relaxed) == 0,
		"snapshot never mixes reset generations or identity publication");
	expect(graphObservations.load(std::memory_order_relaxed) != 0,
		"graph diagnostics are observed during reset activity");
}

void testGraphDiagnosticsAreSafeDuringResetAndReconfigure()
{
	Fixture fixture;
	expect(configureDiagnosticShape(fixture, false) ==
			SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID &&
		fixture.graph.reset(1),
		"graph diagnostic stress fixture starts");
	executeAllClaimed(fixture, SimulationPhaseExecutionIdentity::ownerHelp());
	expect(fixture.graph.advanceOwner() &&
		fixture.graph.state() == SIMULATION_PHASE_GRAPH_COMPLETED,
		"graph diagnostic stress fixture completes its first generation");

	std::atomic<bool> stop{ false };
	std::atomic<bool> readerStarted{ false };
	std::atomic<bool> observeGraphDiagnostics{ false };
	std::atomic<unsigned> invalidDiagnostics{ 0 };
	std::atomic<unsigned> graphObservations{ 0 };
	std::thread reader([&]() {
		readerStarted.store(true, std::memory_order_seq_cst);
		while (!stop.load(std::memory_order_seq_cst))
		{
			if (!observeGraphDiagnostics.load(std::memory_order_seq_cst))
			{
				std::this_thread::yield();
				continue;
			}
			const unsigned generation = fixture.graph.generation();
			if (generation > 96)
				invalidDiagnostics.fetch_add(1, std::memory_order_relaxed);
			const unsigned epoch = fixture.graph.internalEpoch();
			if (epoch < 2 || epoch > 192)
				invalidDiagnostics.fetch_add(1, std::memory_order_relaxed);
			const unsigned phaseCount = fixture.graph.phaseCount();
			if (phaseCount != 1 && phaseCount != 2)
				invalidDiagnostics.fetch_add(1, std::memory_order_relaxed);
			const unsigned jobCount = fixture.graph.jobCount();
			if (jobCount != 1 && jobCount != 2)
				invalidDiagnostics.fetch_add(1, std::memory_order_relaxed);
			const SimulationPhaseId firstPhase = fixture.graph.phaseIdAt(0);
			if (firstPhase != 5 && firstPhase != 10)
				invalidDiagnostics.fetch_add(1, std::memory_order_relaxed);
			const SimulationPhaseId secondPhase = fixture.graph.phaseIdAt(1);
			if (secondPhase != SIMULATION_PHASE_INVALID_ID && secondPhase != 20)
				invalidDiagnostics.fetch_add(1, std::memory_order_relaxed);
			const SimulationPhaseNodeState firstState =
				fixture.graph.phaseState(firstPhase);
			if (firstState < SIMULATION_PHASE_NODE_BLOCKED ||
				firstState > SIMULATION_PHASE_NODE_STALE_GENERATION)
			{
				invalidDiagnostics.fetch_add(1, std::memory_order_relaxed);
			}
			const unsigned quiescentEpochBefore =
				fixture.graph.internalEpoch();
			const SimulationPhaseGraphState stateBefore =
				fixture.graph.state();
			const bool quiescent = fixture.graph.isQuiescent();
			const SimulationPhaseGraphState stateAfter =
				fixture.graph.state();
			const unsigned quiescentEpochAfter =
				fixture.graph.internalEpoch();
			if (quiescentEpochBefore == quiescentEpochAfter &&
				stateBefore == stateAfter &&
				(stateBefore == SIMULATION_PHASE_GRAPH_CONFIGURED ||
				 stateBefore == SIMULATION_PHASE_GRAPH_COMPLETED) &&
				!quiescent)
			{
				invalidDiagnostics.fetch_add(1, std::memory_order_relaxed);
			}
			graphObservations.fetch_add(1, std::memory_order_relaxed);
			std::this_thread::yield();
		}
	});
	while (!readerStarted.load(std::memory_order_seq_cst))
		std::this_thread::yield();

	for (unsigned generation = 2; generation <= 96; ++generation)
	{
		if (generation == 2)
			observeGraphDiagnostics.store(true, std::memory_order_seq_cst);
		const bool configured = configureDiagnosticShape(fixture,
			(generation & 1u) == 0) ==
				SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID;
		const bool reset = configured && fixture.graph.reset(generation);
		expect(reset, "graph diagnostic stress reconfigures and resets");
		if (!reset) break;
		executeAllClaimed(fixture,
			SimulationPhaseExecutionIdentity::ownerHelp());
		expect(fixture.graph.advanceOwner() &&
			fixture.graph.state() == SIMULATION_PHASE_GRAPH_COMPLETED,
			"graph diagnostic stress generation completes");
		if (generation == 2)
		{
			while (graphObservations.load(std::memory_order_relaxed) == 0)
				std::this_thread::yield();
		}
	}
	observeGraphDiagnostics.store(false, std::memory_order_seq_cst);
	stop.store(true, std::memory_order_seq_cst);
	reader.join();
	expect(invalidDiagnostics.load(std::memory_order_relaxed) == 0,
		"graph diagnostics never expose a partial reset or configuration");
	expect(graphObservations.load(std::memory_order_relaxed) != 0,
		"graph diagnostics are observed during reconfiguration activity");
}

SimulationPhaseGraphConfigurationStatus configureStoragePairAlias(
	unsigned pair)
{
	OwnerHarness owner;
	SimulationPhaseNodeStorage nodes[1];
	SimulationPhaseJobStorage jobs[1];
	SimulationPhaseJobResultStorage results[1];
	SimulationPhaseNodeStorage *nodePointer = nodes;
	SimulationPhaseJobStorage *jobPointer = jobs;
	SimulationPhaseJobResultStorage *resultPointer = results;
	if (pair == 0)
		jobPointer = reinterpret_cast<SimulationPhaseJobStorage *>(nodes);
	else if (pair == 1)
		resultPointer =
			reinterpret_cast<SimulationPhaseJobResultStorage *>(nodes);
	else
		resultPointer =
			reinterpret_cast<SimulationPhaseJobResultStorage *>(jobs);
	SimulationPhaseGraph graph(nodePointer, 1, jobPointer, 1,
		resultPointer, 1, Fixture::isOwner, &owner);
	int input = 0;
	SimulationPhaseDefinition phase =
		{ 1, nullptr, 0, &input, sizeof(input) };
	return graph.configure(&phase, 1, nullptr, 0);
}

SimulationPhaseGraphConfigurationStatus configureStorageInputAlias(
	unsigned storage)
{
	OwnerHarness owner;
	SimulationPhaseNodeStorage nodes[2];
	SimulationPhaseJobStorage jobs[2];
	SimulationPhaseJobResultStorage results[2];
	const void *input = storage == 0 ? static_cast<const void *>(nodes) :
		storage == 1 ? static_cast<const void *>(jobs) :
		static_cast<const void *>(results);
	SimulationPhaseGraph graph(nodes, 2, jobs, 2, results, 2,
		Fixture::isOwner, &owner);
	SimulationPhaseDefinition phase = { 1, nullptr, 0, input, 1 };
	return graph.configure(&phase, 1, nullptr, 0);
}

SimulationPhaseGraphConfigurationStatus configureStorageOutputAlias(
	unsigned storage)
{
	OwnerHarness owner;
	SimulationPhaseNodeStorage nodes[2];
	SimulationPhaseJobStorage jobs[2];
	SimulationPhaseJobResultStorage results[2];
	PhaseInput input;
	void *output = storage == 0 ? static_cast<void *>(nodes) :
		storage == 1 ? static_cast<void *>(jobs) :
		static_cast<void *>(results);
	SimulationPhaseGraph graph(nodes, 2, jobs, 2, results, 2,
		Fixture::isOwner, &owner);
	SimulationPhaseDefinition phase =
		{ 1, nullptr, 0, &input, sizeof(input) };
	SimulationPhaseJobDefinition job = {};
	job.phaseId = 1;
	job.key = 0;
	job.privateOutput = output;
	job.privateOutputBytes = 1;
	job.execute = executeJob;
	job.validate = validateJob;
	job.commit = commitJob;
	return graph.configure(&phase, 1, &job, 1);
}

SimulationPhaseGraphConfigurationStatus configureDependencyStorageAlias(
	unsigned storage)
{
	OwnerHarness owner;
	SimulationPhaseNodeStorage nodes[2];
	SimulationPhaseJobStorage jobs[1];
	SimulationPhaseJobResultStorage results[1];
	void *storagePointer = storage == 0 ? static_cast<void *>(nodes) :
		storage == 1 ? static_cast<void *>(jobs) :
		static_cast<void *>(results);
	const SimulationPhaseId dependencyId = 1;
	std::memcpy(storagePointer, &dependencyId, sizeof(dependencyId));
	SimulationPhaseGraph graph(nodes, 2, jobs, 1, results, 1,
		Fixture::isOwner, &owner);
	const SimulationPhaseId *aliasedDependency =
		static_cast<const SimulationPhaseId *>(storagePointer);
	SimulationPhaseDefinition phases[2] = {
		{ 1, nullptr, 0, nullptr, 0 },
		{ 2, aliasedDependency, 1, nullptr, 0 }
	};
	return graph.configure(phases, 2, nullptr, 0);
}

void testGraphValidation()
{
	int inputs[40] = {};
	JobOutput outputs[4] = {};
	{
		Fixture fixture;
		SimulationPhaseDefinition phases[2] = {
			{ 1, nullptr, 0, &inputs[0], sizeof(int) },
			{ 1, nullptr, 0, &inputs[1], sizeof(int) }
		};
		expect(fixture.graph.configure(phases, 2, nullptr, 0) ==
			SIMULATION_PHASE_GRAPH_DUPLICATE_PHASE_ID,
			"duplicate phase IDs are rejected");
	}
	{
		Fixture fixture;
		const SimulationPhaseId missing[] = { 9 };
		SimulationPhaseDefinition phase =
			{ 1, missing, 1, &inputs[0], sizeof(int) };
		expect(fixture.graph.configure(&phase, 1, nullptr, 0) ==
			SIMULATION_PHASE_GRAPH_UNKNOWN_DEPENDENCY,
			"unknown dependencies are rejected");
	}
	{
		Fixture fixture;
		const SimulationPhaseId duplicate[] = { 1, 1 };
		SimulationPhaseDefinition phases[2] = {
			{ 1, nullptr, 0, &inputs[0], sizeof(int) },
			{ 2, duplicate, 2, &inputs[1], sizeof(int) }
		};
		expect(fixture.graph.configure(phases, 2, nullptr, 0) ==
			SIMULATION_PHASE_GRAPH_DUPLICATE_DEPENDENCY,
			"duplicate dependency edges are rejected");
	}
	{
		Fixture fixture;
		const SimulationPhaseId dependsOn2[] = { 2 };
		const SimulationPhaseId dependsOn1[] = { 1 };
		SimulationPhaseDefinition phases[2] = {
			{ 1, dependsOn2, 1, &inputs[0], sizeof(int) },
			{ 2, dependsOn1, 1, &inputs[1], sizeof(int) }
		};
		expect(fixture.graph.configure(phases, 2, nullptr, 0) ==
			SIMULATION_PHASE_GRAPH_CYCLE,
			"cyclic graphs are rejected");
	}
	{
		Fixture fixture;
		SimulationPhaseDefinition phase =
			{ 1, nullptr, 0, &inputs[0], sizeof(int) };
		auto first = jobDefinition(1, 3, outputs[0]);
		auto second = jobDefinition(1, 3, outputs[1]);
		SimulationPhaseJobDefinition jobs[] = { first, second };
		expect(fixture.graph.configure(&phase, 1, jobs, 2) ==
			SIMULATION_PHASE_GRAPH_DUPLICATE_JOB_KEY,
			"duplicate keys within a phase are rejected");
	}
	{
		Fixture fixture;
		SimulationPhaseDefinition phase =
			{ 1, nullptr, 0, &inputs[0], sizeof(int) };
		auto first = jobDefinition(1, 1, outputs[0]);
		auto second = jobDefinition(1, 2, outputs[0]);
		SimulationPhaseJobDefinition jobs[] = { first, second };
		expect(fixture.graph.configure(&phase, 1, jobs, 2) ==
			SIMULATION_PHASE_GRAPH_INVALID_OUTPUT_OWNERSHIP,
			"private job output slots cannot overlap");
	}
	{
		Fixture fixture;
		SimulationPhaseDefinition phase =
			{ 1, nullptr, 0, &outputs[0], sizeof(outputs[0]) };
		auto job = jobDefinition(1, 1, outputs[0]);
		expect(fixture.graph.configure(&phase, 1, &job, 1) ==
			SIMULATION_PHASE_GRAPH_INVALID_OUTPUT_OWNERSHIP,
			"worker output cannot alias its immutable phase input");
	}
	for (unsigned producer = 0; producer < 2; ++producer)
	{
		Fixture fixture;
		SimulationPhaseDefinition phases[2] = {
			{ 1, nullptr, 0, &inputs[0], sizeof(int) },
			{ 2, nullptr, 0, &inputs[1], sizeof(int) }
		};
		phases[1 - producer].immutableInput = &outputs[producer];
		phases[1 - producer].immutableInputBytes = sizeof(outputs[producer]);
		auto job = jobDefinition(producer + 1, 0, outputs[producer]);
		expect(fixture.graph.configure(phases, 2, &job, 1) ==
			SIMULATION_PHASE_GRAPH_INVALID_OUTPUT_OWNERSHIP,
			"output cannot alias any other concurrently visible phase input");
	}
	{
		bool allStoragePairsRejected = true;
		for (unsigned pair = 0; pair < 3; ++pair)
		{
			allStoragePairsRejected = allStoragePairsRejected &&
				configureStoragePairAlias(pair) ==
					SIMULATION_PHASE_GRAPH_INVALID_STORAGE_OWNERSHIP;
		}
		expect(allStoragePairsRejected,
			"node/job/result storage spans are pairwise disjoint");
	}
	{
		bool allStorageInputAliasesRejected = true;
		bool allStorageOutputAliasesRejected = true;
		for (unsigned storage = 0; storage < 3; ++storage)
		{
			allStorageInputAliasesRejected =
				allStorageInputAliasesRejected &&
				configureStorageInputAlias(storage) ==
					SIMULATION_PHASE_GRAPH_INVALID_STORAGE_OWNERSHIP;
			allStorageOutputAliasesRejected =
				allStorageOutputAliasesRejected &&
				configureStorageOutputAlias(storage) ==
					SIMULATION_PHASE_GRAPH_INVALID_STORAGE_OWNERSHIP;
		}
		expect(allStorageInputAliasesRejected,
			"every storage span is disjoint from every external input");
		expect(allStorageOutputAliasesRejected,
			"every storage span is disjoint from every private output");
	}
	{
		bool allDependencyStorageAliasesRejected = true;
		for (unsigned storage = 0; storage < 3; ++storage)
		{
			allDependencyStorageAliasesRejected =
				allDependencyStorageAliasesRejected &&
				configureDependencyStorageAlias(storage) ==
					SIMULATION_PHASE_GRAPH_INVALID_STORAGE_OWNERSHIP;
		}
		expect(allDependencyStorageAliasesRejected,
			"dependency arrays cannot alias persistent graph storage");
	}
	{
		Fixture fixture;
		SimulationPhaseDefinition phase =
			{ 1, nullptr, 0, &inputs[0], sizeof(int) };
		auto job = jobDefinition(1, 1, outputs[0]);
		job.execute = nullptr;
		expect(fixture.graph.configure(&phase, 1, &job, 1) ==
			SIMULATION_PHASE_GRAPH_MISSING_JOB_CALLBACK,
			"missing worker callback is rejected");
	}
	{
		Fixture fixture;
		SimulationPhaseDefinition phase =
			{ 1, nullptr, 0, &inputs[0], sizeof(int) };
		auto job = jobDefinition(1, SIMULATION_PHASE_INVALID_JOB_KEY,
			outputs[0]);
		expect(fixture.graph.configure(&phase, 1, &job, 1) ==
			SIMULATION_PHASE_GRAPH_INVALID_ARGUMENT,
			"reserved invalid job key is not reported as a callback error");
	}
	{
		Fixture fixture;
		SimulationPhaseDefinition phase =
			{ 1, nullptr, 0, &inputs[0], sizeof(int) };
		auto job = jobDefinition(1, 1, outputs[0]);
		job.validate = nullptr;
		expect(fixture.graph.configure(&phase, 1, &job, 1) ==
			SIMULATION_PHASE_GRAPH_MISSING_JOB_CALLBACK,
			"missing whole-phase validation callback is rejected");
	}
	{
		OwnerHarness owner;
		SimulationPhaseNodeStorage nodes[1];
		SimulationPhaseJobStorage jobs[1];
		SimulationPhaseJobResultStorage results[1];
		SimulationPhaseGraph graph(nodes, 1, jobs, 1, results, 1,
			Fixture::isOwner, &owner);
		SimulationPhaseDefinition phases[2] = {
			{ 1, nullptr, 0, &inputs[0], sizeof(int) },
			{ 2, nullptr, 0, &inputs[1], sizeof(int) }
		};
		expect(graph.configure(phases, 2, nullptr, 0) ==
			SIMULATION_PHASE_GRAPH_STORAGE_TOO_SMALL,
			"caller capacity bounds graph construction");
	}
	{
		Fixture fixture;
		std::array<SimulationPhaseDefinition,
			SIMULATION_PHASE_GRAPH_MAX_PHASES + 1> phases = {};
		for (unsigned index = 0; index < phases.size(); ++index)
			phases[index] = { index, nullptr, 0, &inputs[index], sizeof(int) };
		expect(fixture.graph.configure(phases.data(),
			static_cast<unsigned>(phases.size()), nullptr, 0) ==
			SIMULATION_PHASE_GRAPH_TOO_MANY_PHASES,
			"fixed dependency mask rejects an unbounded phase graph");
	}
	{
		Fixture fixture;
		SimulationPhaseDefinition phase =
			{ 1, nullptr, 0, &inputs[0], sizeof(int) };
		expect(fixture.graph.configure(&phase, 1, nullptr, 0) ==
			SIMULATION_PHASE_GRAPH_CONFIGURATION_VALID &&
			fixture.graph.reset(1) && fixture.graph.advanceOwner() &&
			fixture.graph.state() == SIMULATION_PHASE_GRAPH_COMPLETED,
			"explicit zero-job barrier phase completes without allocation");
	}
}

} // namespace

int main()
{
	testStableGraphAndWorkerCounts();
	testDependenciesAreNonblockingTickets();
	testCancellationFailureAndStaleFaults();
	testWorkerFailureCancelsCooperativePeer();
	testOutOfOrderRootFailureAttribution();
	testCancellationDuringValidationPublishesNothing();
	testCompletionCancellationOutcomeGate();
	testExecutionIdentityOwnerGateAndReset();
	testInternalEpochAndGenerationABA();
	testStaleExecutorCannotConsumeResetClaim();
	testAdvanceOwnerReentrancyFailsClosed();
	testMixedTerminalPrecedenceIsCanonical();
	testSnapshotResetPublicationIsConsistent();
	testGraphDiagnosticsAreSafeDuringResetAndReconfigure();
	testGraphValidation();

	if (g_failures != 0)
	{
		std::cerr << g_failures << " simulation phase graph test(s) failed\n";
		return 1;
	}
	std::cout << "Simulation phase graph tests passed\n";
	return 0;
}
