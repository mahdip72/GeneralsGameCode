#pragma once

namespace rts
{
enum PipelineExecutionMode
{
	PIPELINE_EXECUTION_PARALLEL = 0,
	PIPELINE_EXECUTION_SERIAL = 1
};

// Process-wide startup policy. Once any execution owner or compute scheduler
// starts, the selected mode remains immutable through teardown and restart.
PipelineExecutionMode GetPipelineExecutionMode();
bool SetPipelineExecutionMode(PipelineExecutionMode mode);
bool SetPipelineExecutionMode(const char *mode);
bool UseParallelPipelines();
void LockPipelineExecutionMode();
bool IsPipelineExecutionModeLocked();
}
