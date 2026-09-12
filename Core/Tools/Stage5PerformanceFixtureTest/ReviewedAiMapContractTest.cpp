// Native build/run is deferred to the owning integration task.
#include "Common/SkirmishAITestMapBinding.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

int main()
{
    const char *args[] = { "game", "-runSkirmishAITest4v2", "1729",
        "-skirmishAITestReviewedMap", "Maps\\AiProof\\AiProof.map",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "16384", "1234ABCD" };
    rts::ai_fixture::MapRequest request;
    const char *error = 0;
    assert(rts::ai_fixture::ParseMapRequest(8, args, true, &request, &error));
    assert(request.requested && request.byteCount == 16384 && request.crc == 0x1234abcdU);
    assert(strcmp(request.mapKey, "Maps\\AiProof\\AiProof.map") == 0);
    assert(!rts::ai_fixture::ParseMapRequest(8, args, false, &request, &error));
    assert(!rts::ai_fixture::ParseMapRequest(7, args, true, &request, &error));
    args[1] = "-runSkirmishAITest";
    assert(!rts::ai_fixture::ParseMapRequest(8, args, true, &request, &error));
    args[1] = "-runSkirmishAITest4v2";
    args[4] = "Maps\\Twilight Flame\\Twilight Flame.map";
    assert(!rts::ai_fixture::ParseMapRequest(8, args, true, &request, &error));
    args[4] = "Maps\\..\\escape.map";
    assert(!rts::ai_fixture::ParseMapRequest(8, args, true, &request, &error));
    args[4] = "Maps\\AiProof\\different.map";
    assert(!rts::ai_fixture::ParseMapRequest(8, args, true, &request, &error));
    const char *legacy[] = { "game", "-runSkirmishAITest", "1729" };
    assert(rts::ai_fixture::ParseMapRequest(3, legacy, false, &request, &error));
    assert(!request.requested);
    return 0;
}
