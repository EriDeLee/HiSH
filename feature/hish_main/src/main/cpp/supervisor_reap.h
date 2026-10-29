#pragma once

// Only the supervisor calls this. Its direct children are the main shell and
// orphaned descendants adopted by PR_SET_CHILD_SUBREAPER, not the shell's jobs.
// wait() returns one exited child, 0 when none is ready, or a negative error.
template<class Wait>
constexpr bool HishReapExitedChildren(int shell, Wait wait)
{
    for (;;) {
        const int child = wait();
        if (child == shell) return true;
        if (child <= 0) return false;
    }
}
