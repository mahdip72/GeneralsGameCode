#include "process.h"
#include "Lib/JobSystem.h"
#include "Lib/SimulationExecutionPolicy.h"

#include <cstdio>
#include <cstring>

typedef int Int;

struct ParserExit
{
    explicit ParserExit(int value) : code(value) {}
    int code;
};

static void RejectParserExit(int code)
{
    throw ParserExit(code);
}

// The test extracts these exact startup parser functions from the production
// game source at configure time.  The simulation parser records an optional
// skirmish-test field as a side effect; the contract test only needs to prove
// the option policy, so keep that callback device-free.
static bool SetSkirmishAITestSimulationModeInput(const char *)
{
    return true;
}

static bool SetSkirmishAITestExecutableHashInput(const char *sha256)
{
    if (sha256 == nullptr || std::strlen(sha256) != 64) {
        return false;
    }
    for (int index = 0; index < 64; ++index) {
        const char value = sha256[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f') ||
              (value >= 'A' && value <= 'F'))) {
            return false;
        }
    }
    return true;
}

#define exit RejectParserExit
#include "LauncherGameStartupParsers.h"
#undef exit

namespace
{
bool Expect(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        return false;
    }
    return true;
}

bool ReadLauncherConfig(const char *path, Process &process)
{
    FILE *stream = std::fopen(path, "rb");
    if (stream == nullptr) {
        return false;
    }

    ConfigFile config;
    const bool parsed = config.readFile(stream) != FALSE &&
        Read_Process_Info(config, process) != FALSE;
    std::fclose(stream);
    return parsed;
}

bool ParseProductionStartupArguments(char *arguments)
{
    char *tokens[32];
    int tokenCount = 0;
    for (char *token = std::strtok(arguments, " \t\r\n");
         token != nullptr && tokenCount < 32;
         token = std::strtok(nullptr, " \t\r\n")) {
        tokens[tokenCount++] = token;
    }

    for (int index = 0; index < tokenCount; ++index) {
        if (std::strcmp(tokens[index], "-simulationMode") == 0 ||
            std::strcmp(tokens[index], "-workerPolicy") == 0) {
            if (index + 1 >= tokenCount) {
                return false;
            }
            char *optionArguments[] = {tokens[index], tokens[index + 1]};
            try {
                if (std::strcmp(tokens[index], "-simulationMode") == 0) {
                    if (parseSimulationMode(optionArguments, 2) != 2) {
                        return false;
                    }
                } else if (parseWorkerPolicy(optionArguments, 2) != 2) {
                    return false;
                }
            } catch (const ParserExit &) {
                return false;
            }
            ++index;
        }
    }
    return true;
}

bool TestLauncherConfigArgumentContract(const char *generatedLcfPath)
{
    bool ok = true;
    char simulationModeOption[] = "-simulationMode";
    char serialMode[] = "serial";
    char workerPolicyOption[] = "-workerPolicy";
    char allWorkers[] = "all";
    char *userArguments[] = {
        simulationModeOption, serialMode, workerPolicyOption, allWorkers};

    Process process;
    ok &= Expect(ReadLauncherConfig(generatedLcfPath, process),
        "launcher LCF must parse through the production config reader");
    ok &= Expect(std::strcmp(process.command, "launcher-contract-target.exe") == 0,
        "generated launcher LCF must preserve the target executable name");
    ok &= Expect(std::strcmp(process.directory, ".") == 0,
        "generated launcher LCF must preserve the working directory");

#if RTS_LAUNCHER_POINTER_BYTES == 8
    const char *expectedDefaults =
        " -simulationMode parallel -workerPolicy auto";
#elif RTS_LAUNCHER_POINTER_BYTES == 4
    const char *expectedDefaults = "";
#else
#error Unsupported launcher pointer width
#endif
    ok &= Expect(std::strcmp(process.args, expectedDefaults) == 0,
        "generated launcher LCF must select defaults only for native x64");

    ok &= Expect(Append_Process_Arguments(process, 4, userArguments) == TRUE,
        "launcher argv must append when the fixed buffer has capacity");
#if RTS_LAUNCHER_POINTER_BYTES == 8
    const char *expectedArguments =
        " -simulationMode parallel -workerPolicy auto -simulationMode serial -workerPolicy all";
#elif RTS_LAUNCHER_POINTER_BYTES == 4
    const char *expectedArguments = " -simulationMode serial -workerPolicy all";
#else
#error Unsupported launcher pointer width
#endif
    ok &= Expect(std::strcmp(process.args, expectedArguments) == 0,
        "user launcher argv must follow generated LCF defaults in order");

    char parsedArguments[sizeof(process.args)];
    std::strcpy(parsedArguments, process.args);
    ok &= Expect(ParseProductionStartupArguments(parsedArguments),
        "composed launcher arguments must pass the production game startup parsers");
    ok &= Expect(rts::GetSimulationExecutionMode() ==
        rts::SIMULATION_EXECUTION_SERIAL,
        "the user simulation mode must take precedence over the generated default");
    ok &= Expect(rts::JobSystem::startupConfig().workerPolicy ==
        rts::JOB_WORKER_POLICY_ALL,
        "the user worker policy must take precedence using a game-supported value");

    return ok;
}

bool TestLauncherArgumentCapacityContract()
{
    bool ok = true;
    Process process;
    std::strcpy(process.args, "seed");
    const char before[] = "seed";

    char oversizedArgument[sizeof(process.args)];
    for (size_t index = 0; index < sizeof(oversizedArgument) - 1; ++index) {
        oversizedArgument[index] = 'x';
    }
    oversizedArgument[sizeof(oversizedArgument) - 1] = 0;
    char *oversizedArguments[] = {oversizedArgument};
    ok &= Expect(Append_Process_Arguments(process, 1, oversizedArguments) == FALSE,
        "launcher argv must reject an argument that exceeds the fixed buffer");
    ok &= Expect(std::strcmp(process.args, before) == 0,
        "a rejected launcher argv append must leave the existing command line unchanged");

    Process boundary;
    char maximumArgument[sizeof(boundary.args) - 1];
    for (size_t index = 0; index < sizeof(maximumArgument) - 1; ++index) {
        maximumArgument[index] = 'y';
    }
    maximumArgument[sizeof(maximumArgument) - 1] = 0;
    char *maximumArguments[] = {maximumArgument};
    ok &= Expect(Append_Process_Arguments(boundary, 1, maximumArguments) == TRUE,
        "launcher argv must accept the largest argument that fits with its separator and terminator");
    ok &= Expect(std::strlen(boundary.args) == sizeof(boundary.args) - 1,
        "the accepted launcher argv must consume exactly the available command-line capacity");

    return ok;
}

bool TestValidationExecutableHashParserContract()
{
    bool ok = true;
    char option[] = "-validationExecutableSha256";
    char shortHash[] = "0123456789abcdef";
    char malformedHash[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg";
    char validHash[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    char *shortArguments[] = {option, shortHash};
    bool shortRejected = false;
    try {
        parseValidationExecutableSha256(shortArguments, 2);
    }
    catch (const ParserExit &) {
        shortRejected = true;
    }
    ok &= Expect(shortRejected,
        "short validation executable hashes must fail before fixed-width indexing");

    char *malformedArguments[] = {option, malformedHash};
    bool malformedRejected = false;
    try {
        parseValidationExecutableSha256(malformedArguments, 2);
    }
    catch (const ParserExit &) {
        malformedRejected = true;
    }
    ok &= Expect(malformedRejected,
        "non-hex validation executable hashes must be rejected");

    char *validArguments[] = {option, validHash};
    bool validAccepted = false;
    try {
        validAccepted = parseValidationExecutableSha256(validArguments, 2) == 2;
    }
    catch (const ParserExit &) {
        validAccepted = false;
    }
    ok &= Expect(validAccepted,
        "exactly 64 hexadecimal validation executable hashes must be accepted");
    return ok;
}
} // namespace

int main(int argc, char *argv[])
{
    if (argc == 2 && std::strcmp(argv[1], "--child") == 0) {
        return 7;
    }

    bool ok = true;
    if (argc != 2) {
        std::fprintf(stderr, "Expected generated launcher LCF path.\n");
        return 1;
    }
    ok &= TestLauncherConfigArgumentContract(argv[1]);
    ok &= TestLauncherArgumentCapacityContract();
    ok &= TestValidationExecutableHashParserContract();

    Process missing;
    std::strcpy(missing.command, "stage3_launcher_process_that_does_not_exist.exe");
    ok &= Expect(Create_Process(missing) == FALSE, "missing executable must report creation failure");
    ok &= Expect(missing.hProcess == nullptr && missing.hThread == nullptr,
        "failed creation must not publish handles");
    ok &= Expect(missing.dwProcessID == 0 && missing.dwThreadID == 0,
        "failed creation must not publish process identifiers");

    Process child;
    std::snprintf(child.command, sizeof(child.command), "\"%s\"", argv[0]);
    std::strcpy(child.args, " --child");
    ok &= Expect(Create_Process(child) == TRUE, "contract child must start");
    ok &= Expect(child.hProcess != nullptr, "successful creation must publish a process handle");

    DWORD exitCode = 0;
    ok &= Expect(Wait_Process(child, &exitCode) == TRUE, "contract child must be waitable");
    ok &= Expect(exitCode == 7, "wait must publish the child exit code");
    ok &= Expect(child.hProcess == nullptr && child.hThread == nullptr,
        "wait must release process and thread handles");
    ok &= Expect(child.dwProcessID == 0 && child.dwThreadID == 0,
        "wait must clear released process identifiers");

    return ok ? 0 : 1;
}
