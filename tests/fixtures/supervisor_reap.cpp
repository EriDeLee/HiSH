#include "supervisor_reap.h"

constexpr bool noExitedChildren()
{
    int calls = 0;
    const bool done = HishReapExitedChildren(42, [&]() { ++calls; return 0; });
    return !done && calls == 1;
}
constexpr bool adoptedChildrenBeforeShell()
{
    int children[] = {100, 101, 42, 102};
    int at = 0;
    const bool done = HishReapExitedChildren(42, [&]() { return children[at++]; });
    return done && at == 3;
}
constexpr bool repeatedAdoption()
{
    for (int round = 0; round < 10; ++round) {
        int at = 0;
        const bool done = HishReapExitedChildren(42, [&]() { return at++ < 100 ? 1000 + at : 0; });
        if (done || at != 101) return false;
    }
    return true;
}
static_assert(noExitedChildren(), "An idle/live shell must not be shut down");
static_assert(adoptedChildrenBeforeShell(), "Reap adopted exits and stop exactly at shell exit");
static_assert(repeatedAdoption(), "Drain every adopted exit in all ten rounds");
static_assert(!HishReapExitedChildren(42, []() { return -1; }), "An unavailable exit is not a child PID");
