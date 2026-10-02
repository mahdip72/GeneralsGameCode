#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

// A bounded native child for the source-connected PowerShell runner tests.
// It does not load game code, touch registry/profile state, or create workers.
static bool parseBoundedUnsigned(const char *text, unsigned long maximum,
    unsigned long &value)
{
    if (text == NULL || *text == '\0' || *text == '-' || *text == '+') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    value = strtoul(text, &end, 10);
    return errno == 0 && end != text && *end == '\0' && value <= maximum;
}

int main(int argc, char **argv)
{
    unsigned long sleepMilliseconds = 0;
    unsigned long exitCode = 0;
    if (argc != 3 || !parseBoundedUnsigned(argv[1], 15000, sleepMilliseconds) ||
        !parseBoundedUnsigned(argv[2], 127, exitCode)) {
        fputs("usage: lifecycle-fixture <sleep-ms:0..15000> <exit-code:0..127>\n", stderr);
        return 64;
    }

    FILETIME creation = {}, exitTime = {}, kernel = {}, user = {};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user)) {
        fputs("lifecycle-fixture GetProcessTimes failed\n", stderr);
        return 65;
    }
    const ULONGLONG creationTime100ns =
        (static_cast<ULONGLONG>(creation.dwHighDateTime) << 32) |
        static_cast<ULONGLONG>(creation.dwLowDateTime);
    char nonce[64] = {};
    const DWORD nonceLength = GetEnvironmentVariableA("RTS_STAGE5_RUN_NONCE",
        nonce, static_cast<DWORD>(sizeof(nonce)));
    if (nonceLength == 0 || nonceLength >= sizeof(nonce)) {
        fputs("lifecycle-fixture run nonce unavailable\n", stderr);
        return 66;
    }

    printf("STAGE5_LIFECYCLE_FIXTURE schema=1 pid=%lu creationTime100ns=%I64u "
        "sleepMilliseconds=%lu exitCode=%lu runNonce=%s\n",
        GetCurrentProcessId(), creationTime100ns, sleepMilliseconds, exitCode, nonce);
    fputs("STAGE5_LIFECYCLE_FIXTURE stderr=ready\n", stderr);
    fflush(stdout);
    fflush(stderr);
    Sleep(static_cast<DWORD>(sleepMilliseconds));
    return static_cast<int>(exitCode);
}
