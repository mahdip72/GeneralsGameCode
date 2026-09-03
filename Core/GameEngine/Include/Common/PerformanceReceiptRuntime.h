/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

#if defined(_WIN64)
#include "Lib/PerformanceReceipt.h"
#include "Lib/SimulationPhaseGraphOwnerAdapter.h"
#include <atomic>

struct OrdinaryPathRuntimeMetrics;
namespace rts
{
struct CollisionCandidateRuntimeMetrics;
struct PhysicsIntegrationRuntimeMetrics;
struct ObjectStatusTimerRuntimeMetrics;
struct ImmutableSpatialRuntimeMetrics;
}

// Run-scoped diagnostics, independent of resettable game data. These guards
// govern evidence only; their results must never select simulation behavior.
class PerformanceReceiptOwnerLifecycle
{
public:
	PerformanceReceiptOwnerLifecycle();
	bool begin();
	bool observeCompletedFrame(unsigned frame);
	bool captureTerminalResult(unsigned actualFrame, unsigned crc);
	bool finish(unsigned outstandingJobs, unsigned pendingOwnerCompletions);

	bool begun() const { return m_begun; }
	bool finalized() const { return m_finalized; }
	bool terminalResultKnown() const { return m_terminalResultKnown; }
	unsigned lastCompletedFrame() const { return m_lastCompletedFrame; }
	unsigned terminalFrame() const { return m_terminalFrame; }
	unsigned terminalCrc() const { return m_terminalCrc; }

private:
	bool m_begun;
	bool m_finalized;
	bool m_terminalResultKnown;
	bool m_contiguous;
	unsigned m_lastCompletedFrame;
	unsigned m_terminalFrame;
	unsigned m_terminalCrc;
	PerformanceReceiptOwnerLifecycle(const PerformanceReceiptOwnerLifecycle &);
	PerformanceReceiptOwnerLifecycle &operator=(const PerformanceReceiptOwnerLifecycle &);
};

// One instance belongs to one real replay/fresh-match owner loop. All game
// observations are copied before teardown; finish only uses retained snapshots,
// persistent diagnostic ledgers and the scheduler's drained-work counters.
class PerformanceReceiptRuntime
{
public:
	PerformanceReceiptRuntime();
	bool begin(const char *fixtureKind, const char *replayPath);
	bool active() const { return m_active && !m_lifecycle.finalized(); }
	void invalidate(const char *reason);
	void observePhaseBoundary(
		rts::LiveSimulationPhaseObservationBoundary boundary,
		rts::SimulationPhaseId phaseId, unsigned generation,
		unsigned authorityFrame, unsigned actualOwnerFrame) noexcept;
	void bindFixture(const char *kind, const char *contentPath,
		const char *sha256, unsigned seed);
	void captureCompletedFrame(unsigned previousFrame,
		const rts::CollisionCandidateRuntimeMetrics &collision,
		const rts::PhysicsIntegrationRuntimeMetrics &physics,
		const rts::ObjectStatusTimerRuntimeMetrics &status,
		const rts::ImmutableSpatialRuntimeMetrics &spatial,
		const OrdinaryPathRuntimeMetrics &path);
	void captureTerminalResult(unsigned actualFrame, unsigned crc,
		bool crcKnown, bool clean);
	void captureSchedulerBeforeTeardown();
	void retainClosedReplay(const char *path, const char *sha256);
	void finish(int exitCode, const char *boundary);

private:
	rts::performance::PerformanceReceipt m_receipt;
	PerformanceReceiptOwnerLifecycle m_lifecycle;
	bool m_active;
	rts::performance::KernelPerformanceFrame m_phaseFrame;
	rts::JobMetricCounter m_phaseSampleOrdinal;
	unsigned m_phaseGeneration;
	unsigned m_phaseAuthorityFrame;
	std::atomic<bool> m_phaseObservationFailed;
	std::string m_failure;
	PerformanceReceiptRuntime(const PerformanceReceiptRuntime &);
	PerformanceReceiptRuntime &operator=(const PerformanceReceiptRuntime &);
};
#endif
