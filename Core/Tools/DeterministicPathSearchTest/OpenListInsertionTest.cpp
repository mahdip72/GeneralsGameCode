// Production bodies/accessors are extracted at configure time. These substitutes
// expose only used storage/services; this is not a full game ABI/runtime fixture.
#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#if RETAIL_COMPATIBLE_PATHFINDING != 0
#error This fixture exercises only fixed-mode insertion.
#endif
using Bool = bool;
using Int = int;
using UnsignedInt = unsigned int;
using UnsignedShort = unsigned short;
constexpr Bool TRUE = true, FALSE = false;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); std::abort(); } } while (false)
#define DEBUG_ASSERTCRASH(condition, ignored) CHECK(condition)
class PathfindCell;
struct PathfindCellInfo
{
#include "OpenListCosts.inc"
    Bool m_open = false, m_closed = false;
    PathfindCellInfo *m_nextOpen = nullptr, *m_prevOpen = nullptr;
    PathfindCell *m_cell = nullptr;
};
class PathfindCellList
{
public: // Fixture visibility only; game class/header is unchanged.
    PathfindCell *m_head = nullptr, *m_tail = nullptr;
    Bool empty() const { return !m_head; }
    void reset() { m_head = m_tail = nullptr; }
    Bool canReverseSort(PathfindCell&) const;
};
class PathfindCell
{
public:
    PathfindCellInfo info{};
    PathfindCellInfo *m_info = &info;
    unsigned id = 0;
    PathfindCell() { info.m_cell = this; }
#include "OpenListAccessors.inc"
    UnsignedInt getTotalCostDifference(PathfindCell&) const;
    void forwardInsertionSort(PathfindCellList&);
    void reverseInsertionSort(PathfindCellList&);
    void putOnSortedOpenList(PathfindCellList&);
    void removeFromOpenList(PathfindCellList&);
    static Int releaseOpenList(PathfindCellList&);
    void releaseInfo() { m_info = nullptr; }
};
#include "OpenListMethods.inc"
struct Fixture
{
    std::array<PathfindCell, 96> cells;
    PathfindCellList list;
    std::vector<unsigned> oracle;
    Fixture() { for (unsigned i = 0; i < cells.size(); ++i) cells[i].id = i; }
    void check()
    {
        // Independent sequence oracle: append identity then stable-sort by cost.
        PathfindCell *cell = list.m_head, *previous = nullptr;
        for (unsigned id : oracle)
        {
            CHECK(cell == &cells[id] && cell->m_info == &cell->info);
            CHECK(cell->info.m_open && !cell->info.m_closed);
            CHECK(cell->getPrevOpen() == previous);
            previous = cell; cell = cell->getNextOpen();
        }
        CHECK(!cell && list.m_tail == previous);
        cell = list.m_tail;
        for (auto it = oracle.rbegin(); it != oracle.rend(); ++it)
        {
            CHECK(cell == &cells[*it]); cell = cell->getPrevOpen();
        }
        CHECK(!cell);
        for (const auto& entry : cells)
        {
            const Bool present = std::find(oracle.begin(), oracle.end(), entry.id) != oracle.end();
            CHECK(entry.info.m_open == present && !entry.info.m_closed);
            if (!present) CHECK(!entry.info.m_nextOpen && !entry.info.m_prevOpen);
        }
    }
    void insert(unsigned id, unsigned input)
    {
        CHECK(!cells[id].info.m_open && cells[id].m_info);
        cells[id].setTotalCost(input);
        cells[id].putOnSortedOpenList(list);
        oracle.push_back(id);
        std::stable_sort(oracle.begin(), oracle.end(), [this](unsigned a, unsigned b) {
            return cells[a].info.m_totalCost < cells[b].info.m_totalCost;
        });
        check();
    }
    void remove(unsigned id, unsigned changedCost)
    {
        CHECK(cells[id].info.m_open);
        cells[id].setTotalCost(changedCost); // Real callbacks can mutate before removal.
        cells[id].removeFromOpenList(list);
        const auto found = std::find(oracle.begin(), oracle.end(), id);
        CHECK(found != oracle.end()); oracle.erase(found); check();
    }
    void release()
    {
        const auto released = oracle;
        CHECK(PathfindCell::releaseOpenList(list) == static_cast<Int>(oracle.size()));
        oracle.clear(); check();
        for (unsigned id : released) CHECK(!cells[id].m_info);
    }
};
int main()
{
    for (unsigned cost : {32767u, 32768u, 32769u})
    {
        Fixture f; CHECK(!f.list.canReverseSort(f.cells[0]));
        f.insert(0, 32768); f.insert(1, 32768); f.cells[2].setTotalCost(cost);
        CHECK(f.list.canReverseSort(f.cells[2]) == (cost >= 32768));
        f.insert(2, cost);
        if (cost < 32768) CHECK(f.list.m_head == &f.cells[2]);
        else CHECK(f.list.m_tail == &f.cells[2]);
        f.remove(1, 65536); f.insert(1, cost); f.release();
    }
    {
        Fixture f;
        const unsigned inputs[] = {65535, 65536, 65537, 0, 65535, 65536, 131073, 1};
        for (unsigned i = 0; i < 8; ++i)
        {
            f.insert(i, inputs[i]);
            CHECK(f.cells[i].getTotalCost() == static_cast<UnsignedShort>(inputs[i]));
        }
        CHECK(f.list.m_head == &f.cells[1]); f.remove(0, 65536); f.insert(0, 65537); f.release();
    }
    for (unsigned seed : {3u, 17u, 97u})
    {
        Fixture f; std::mt19937 random(seed);
        for (unsigned operation = 0; operation < 3000; ++operation)
        {
            const unsigned id = random() % f.cells.size();
            if (f.cells[id].info.m_open) f.remove(id, random());
            if (random() % 4) f.insert(id, operation % 3 ? random() % 8 : random());
        }
        f.release();
    }
    std::puts("PASS: extracted fixed open-list endpoints, stable order, flags and wrap");
}
