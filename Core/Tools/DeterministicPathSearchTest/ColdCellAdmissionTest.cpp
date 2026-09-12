// The owner admission predicates and optional-info accessors are extracted
// from production source by ColdCellAdmission.cmake. Only surrounding title
// services are substituted; no game executable or world construction is needed.
#include <cstdio>
#include <initializer_list>
#if defined(_WIN32)
#include <windows.h>
#undef FALSE
#endif
using Bool = bool;
using Int = int;
using UnsignedShort = unsigned short;
constexpr Bool FALSE = false;
constexpr int LAYER_GROUND = 1, LAYER_INVALID = 0;
struct ICoord2D { int x, y; };
struct Coord3D { float x, y, z; };
struct PathfindCellInfo { ICoord2D m_pos; };
struct PathfindZoneManager { enum { UNINITIALIZED_ZONE = 0 }; };
struct PathfindCell
{
    enum { CELL_CLEAR = 0 };
    PathfindCellInfo *m_info = nullptr;
    int type = CELL_CLEAR, zone = 7, connection = LAYER_INVALID;
    bool pinched = false, open = false, closed = false;
#include "CellInfoAccessors.inc"
    int getType() const { return type; }
    int getZone() const { return zone; }
    bool getPinched() const { return pinched; }
    int getConnectLayer() const { return connection; }
    bool getOpen() const { return open; }
    bool getClosed() const { return closed; }
};
struct List { bool empty() const { return true; } };
struct CaptureFixture
{
    PathfindCell start, goal, other;
    PathfindCellInfo info{{155,79}};
    PathfindCell *clipped = &start, *indexed = &start, *destination = &goal;
    ICoord2D startIndex{155,79}, goalIndex{176,64};
    Coord3D clippedFrom{}, adjustedTo{};
    List m_openList, m_closedList;
    int obj = 0, locomotorSet = 0, radius = 1;
    bool centerInCell = false, destinationValid = true, movementValid = true;
    PathfindCell *getClippedCell(int, const Coord3D *) { return clipped; }
    PathfindCell *getCell(int, int x, int y)
    {
        return x == startIndex.x && y == startIndex.y ? indexed : destination;
    }
    bool checkDestination(int, int, int, int, int, bool) { return destinationValid; }
    bool validMovementPosition(bool, int, int, const Coord3D *) { return movementValid; }
    bool direct()
    {
#include "DirectCellAdmission.inc"
    }
    bool ordinary()
    {
#include "OrdinaryCellAdmission.inc"
    }
};
static int failures = 0;
static void check(bool ok, const char *label)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
}
int main()
{
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    for (bool ordinary : {false, true})
    {
        const auto admit = [ordinary](CaptureFixture &f) { return ordinary ? f.ordinary() : f.direct(); };
        CaptureFixture f;
        check(admit(f), "cold clear grid cell is eligible");
        check(!f.start.m_info && !f.goal.m_info, "capture does not allocate A-star metadata");
        f.start.m_info = &f.info;
        check(admit(f), "warm matching cell is eligible");
        f.info.m_pos.x++;
        check(!admit(f), "warm x mismatch rejects");
        f.info.m_pos.x--;
        f.info.m_pos.y++;
        check(!admit(f), "warm y mismatch rejects");
        f.start.m_info = nullptr;
        f.indexed = &f.other;
        check(!admit(f), "cold grid pointer mismatch rejects");
        f.indexed = &f.start;
        f.clipped = nullptr;
        check(!admit(f), "null start rejects");
        f.clipped = &f.start;
        f.destination = nullptr;
        check(!admit(f), "null goal rejects");
        f.destination = &f.goal;
        f.start.zone = 0;
        check(!admit(f), "uninitialized zone rejects");
        f.start.zone = 7;
        f.goal.zone = 8;
        check(!admit(f), "cold different-zone request from crash rejects safely");
        f.goal.zone = 7;
        f.start.type = 1;
        check(!admit(f), "blocked start rejects");
        f.start.type = 0;
        f.goal.type = 1;
        check(!admit(f), "blocked goal rejects");
        f.goal.type = 0;
        f.start.pinched = true;
        check(!admit(f), "pinched start rejects");
        f.start.pinched = false;
        f.goal.pinched = true;
        check(!admit(f), "pinched goal rejects");
        f.goal.pinched = false;
        f.start.connection = 2;
        check(!admit(f), "connected start rejects");
        f.start.connection = 0;
        f.goal.connection = 2;
        check(!admit(f), "connected goal rejects");
        f.goal.connection = 0;
        f.destinationValid = false;
        check(!admit(f), "destination validation preserved");
        f.destinationValid = true;
        f.movementValid = false;
        check(!admit(f), "movement validation preserved");
        f.movementValid = true;
        if (ordinary)
        {
            f.start.m_info = &f.info;
            f.info.m_pos = f.startIndex;
            f.start.open = true;
            check(!admit(f), "ordinary warm open start rejects");
            f.start.open = false;
            f.start.closed = true;
            check(!admit(f), "ordinary warm closed start rejects");
            f.start.closed = false;
            f.goal.m_info = &f.info;
            f.goal.open = true;
            check(!admit(f), "ordinary warm open goal rejects");
            f.goal.open = false;
            f.goal.closed = true;
            check(!admit(f), "ordinary warm closed goal rejects");
        }
    }
    if (!failures) std::puts("PASS: direct and ordinary cold-cell admission");
    return failures ? 1 : 0;
}
