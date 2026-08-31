#include "Lib/JobSystem.h"
#include "Lib/PipelineExecutionPolicy.h"

#include <fstream>
#include <stdio.h>
#include <string>
#include <string.h>

#if !defined(_MSC_VER) || _MSC_VER >= 1300
#include <atomic>
#include <chrono>
#include <thread>
#endif

typedef int Int;

struct ParserExit
{
	explicit ParserExit(int value) : code(value) {}
	int code;
};

static void RejectPipelineArguments(int code)
{
	throw ParserExit(code);
}

// CMake extracts this exact function from production CommandLine.cpp.
#define exit RejectPipelineArguments
#include "PipelineModeParserUnderTest.h"
#undef exit

namespace
{
int Check(bool condition, const char *message)
{
	if (condition) return 0;
	fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

bool ParserRejects(char *args[], int count)
{
	try { parsePipelineMode(args, count); }
	catch (const ParserExit &error) { return error.code == 1; }
	return false;
}

int TestStartupTable(const char *path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input) return Check(false, "production command-line source opens");
	std::string source;
	char bytes[4096];
	while (input.read(bytes, sizeof(bytes)) || input.gcount() > 0)
		source.append(bytes, static_cast<std::string::size_type>(input.gcount()));
	const std::string::size_type startup = source.find("static CommandLineParam paramsForStartup[]");
	const std::string::size_type engine = source.find("static CommandLineParam paramsForEngineInit[]");
	const std::string::size_type policy = source.find("{ \"-pipelineMode\", parsePipelineMode }");
	return Check(startup != std::string::npos && engine != std::string::npos &&
		policy > startup && policy < engine &&
		source.find("{ \"-pipelineMode\", parsePipelineMode }", policy + 1) == std::string::npos,
		"pipeline mode is parsed exactly once in the early startup table");
}

int TestParser(const char *sourcePath)
{
	int result = TestStartupTable(sourcePath);
	result |= Check(rts::UseParallelPipelines() && !rts::IsPipelineExecutionModeLocked(),
		"fresh process defaults to unlocked parallel pipelines");
	char option[] = "-pipelineMode";
	char serial[] = "serial";
	char parallel[] = "PaRaLlEl";
	char *args[2] = { option, serial };
	result |= Check(parsePipelineMode(args, 2) == 2 &&
		rts::GetPipelineExecutionMode() == rts::PIPELINE_EXECUTION_SERIAL &&
		!rts::UseParallelPipelines(), "production parser selects serial reference mode");
	args[1] = parallel;
	result |= Check(parsePipelineMode(args, 2) == 2 && rts::UseParallelPipelines(),
		"production parser accepts exact ASCII case-insensitive mode names");
	char invalid[][16] = { "", "auto", "parallelism", " serial", "serial ", "-workerCount" };
	for (unsigned index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index)
	{
		args[1] = invalid[index];
		result |= Check(ParserRejects(args, 2) && rts::UseParallelPipelines(),
			"invalid mode fails without mutating the selected policy");
	}
	args[1] = 0;
	result |= Check(ParserRejects(args, 1) && ParserRejects(args, 2) &&
		ParserRejects(0, 2) && rts::UseParallelPipelines(),
		"missing mode is rejected without reading absent arguments");
	result |= Check(!rts::SetPipelineExecutionMode(static_cast<const char *>(0)) &&
		!rts::SetPipelineExecutionMode(static_cast<rts::PipelineExecutionMode>(99)) &&
		rts::SetPipelineExecutionMode(rts::PIPELINE_EXECUTION_SERIAL),
		"typed and string APIs reject malformed inputs and accept valid startup choices");
	rts::LockPipelineExecutionMode();
	args[1] = parallel;
	result |= Check(rts::IsPipelineExecutionModeLocked() && ParserRejects(args, 2) &&
		!rts::UseParallelPipelines() &&
		!rts::SetPipelineExecutionMode(rts::PIPELINE_EXECUTION_SERIAL),
		"locked mode rejects late parsing and setter calls without changing its value");
	return result;
}

int TestOwnerFreeze()
{
	int result = Check(rts::SetPipelineExecutionMode("serial"), "owner fixture selects serial mode");
	rts::JobSystem &system = rts::JobSystem::instance();
	result |= Check(!system.isRunning() && !rts::IsPipelineExecutionModeLocked(),
		"constructing the scheduler alone neither freezes policy nor starts workers");
	result |= Check(system.registerCurrentThread(rts::JOB_OWNER_AUDIO) &&
		rts::IsPipelineExecutionModeLocked() && !system.isRunning(),
		"service registration freezes startup policy without starting compute");
	result |= Check(!rts::SetPipelineExecutionMode("parallel") &&
		!rts::UseParallelPipelines(), "active service owner prevents execution-mode changes");
	result |= Check(system.unregisterCurrentThread(rts::JOB_OWNER_AUDIO) &&
		!rts::SetPipelineExecutionMode("parallel"),
		"teardown does not reopen the process-wide startup policy");
	return result;
}

int TestComputeFreeze()
{
	int result = Check(rts::UseParallelPipelines(), "compute fixture uses default parallel mode");
	rts::JobSystem &system = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 1;
	config.queueCapacity = 8;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	result |= Check(system.start(config) && rts::IsPipelineExecutionModeLocked(),
		"compute startup freezes the process-wide policy");
	result |= Check(!rts::SetPipelineExecutionMode("serial") && rts::UseParallelPipelines(),
		"running compute does not permit a switch to serial mode");
	system.shutdown();
	result |= Check(rts::IsPipelineExecutionModeLocked() &&
		!rts::SetPipelineExecutionMode("serial"),
		"compute shutdown retains the immutable execution policy");
	return result;
}

#if !defined(_MSC_VER) || _MSC_VER >= 1300
int TestConcurrentFreeze()
{
	std::atomic<unsigned> attempts(0);
	std::atomic<bool> stop(false);
	std::thread setter([&]() {
		unsigned mode = 0;
		while (!stop.load(std::memory_order_acquire))
		{
			rts::SetPipelineExecutionMode(mode++ % 2 ? rts::PIPELINE_EXECUTION_SERIAL :
				rts::PIPELINE_EXECUTION_PARALLEL);
			attempts.fetch_add(1, std::memory_order_release);
		}
	});
	const std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (attempts.load(std::memory_order_acquire) < 100 &&
		std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
	rts::LockPipelineExecutionMode();
	const rts::PipelineExecutionMode lockedMode = rts::GetPipelineExecutionMode();
	stop.store(true, std::memory_order_release);
	setter.join();
	return Check(attempts.load() >= 100 && rts::IsPipelineExecutionModeLocked() &&
		rts::GetPipelineExecutionMode() == lockedMode &&
		!rts::SetPipelineExecutionMode("serial") && !rts::SetPipelineExecutionMode("parallel"),
		"mode and freeze bit publish atomically against competing setter calls");
}
#endif
}

int main(int argc, char *argv[])
{
	if (argc == 3 && strcmp(argv[1], "parser") == 0) return TestParser(argv[2]);
	if (argc == 2 && strcmp(argv[1], "owner") == 0) return TestOwnerFreeze();
	if (argc == 2 && strcmp(argv[1], "compute") == 0) return TestComputeFreeze();
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	if (argc == 2 && strcmp(argv[1], "race") == 0) return TestConcurrentFreeze();
#endif
	fprintf(stderr, "Expected parser <source>, owner, compute, or race fixture.\n");
	return 1;
}
