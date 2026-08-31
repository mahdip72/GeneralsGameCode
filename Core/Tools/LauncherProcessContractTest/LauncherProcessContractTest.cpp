#include "process.h"

#include <cstdio>
#include <cstring>

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
} // namespace

int main(int argc, char *argv[])
{
    if (argc == 2 && std::strcmp(argv[1], "--child") == 0) {
        return 7;
    }

    bool ok = true;

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
