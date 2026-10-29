#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <spawn.h>

// Compiled against the pinned musl headers, then placed in checked, unused
// executable-segment padding of that same DSO. No external calls or relocations.
int posix_spawnattr_getschedparam(const posix_spawnattr_t* attributes, struct sched_param* parameter)
{
    *parameter = (struct sched_param){.sched_priority = attributes->__prio};
    return 0;
}
int posix_spawnattr_setschedparam(posix_spawnattr_t* attributes, const struct sched_param* parameter)
{
    attributes->__prio = parameter->sched_priority;
    return 0;
}
int posix_spawnattr_getschedpolicy(const posix_spawnattr_t* attributes, int* policy)
{
    *policy = attributes->__pol;
    return 0;
}
int posix_spawnattr_setschedpolicy(posix_spawnattr_t* attributes, int policy)
{
    if (policy != SCHED_OTHER && policy != SCHED_FIFO && policy != SCHED_RR) return EINVAL;
    attributes->__pol = policy;
    return 0;
}
