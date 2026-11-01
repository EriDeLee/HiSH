#ifndef HISH_EPOLL_CORE_H
#define HISH_EPOLL_CORE_H
// Emulated EPOLLONESHOT, for a kernel that ignores the flag (this phone's: see docs/epoll.md).
//
// A registration the program makes with EPOLLONESHOT goes to the kernel as an ordinary level-triggered one. When epoll_wait
// reports it, the registration is deleted from the kernel and remembered here as "fired". Seen from outside that is what Linux's
// one-shot does: the descriptor stays silent until the program registers it again. The program re-arms with EPOLL_CTL_MOD, which
// becomes an EPOLL_CTL_ADD because the kernel no longer has the item; its EPOLL_CTL_DEL of a fired item succeeds without the
// kernel, as it does on Linux for a disarmed item.
//
// This file is bookkeeping only: no system calls, no allocation, no locking. The caller holds one lock around every call that
// takes the table and hands in the epoll_ctl system call as a function, so the host tests run the very same code against a model
// kernel. linux/epoll.c is the caller on the phone. A zero-filled table is a valid empty table.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HE_ONESHOT 0x40000000u
#define HE_CTL_ADD 1
#define HE_CTL_DEL 2
#define HE_CTL_MOD 3
// Kernel error numbers (Linux values) that the bookkeeping reacts to.
#define HE_ENOENT 2
#define HE_EBADF 9
#define HE_EEXIST 17
// Registrations tracked per process. More one-shot registrations than this keep their flag and behave as the kernel does.
#define HE_ITEMS 2048
#define HE_BUCKETS 512

enum { HE_STATE_FREE = 0, HE_STATE_ARMED = 1, HE_STATE_FIRED = 2 };

struct he_item {
    uint64_t data;
    uint32_t events;                 // as the program asked, EPOLLONESHOT included
    int32_t epfd, fd, state;
    int32_t next_fd, next_data, next_free;   // item index + 1, 0 ends a chain
};

struct he_table {
    struct he_item item[HE_ITEMS];
    int32_t by_fd[HE_BUCKETS];       // chain heads, item index + 1, 0 = empty
    int32_t by_data[HE_BUCKETS];
    int32_t free_list;               // item index + 1
    int32_t top;                     // items ever handed out
    int32_t armed;                   // one-shot registrations the kernel holds and the program thinks are armed
    int32_t fired;                   // one-shot registrations the kernel no longer holds
};

// The epoll_ctl system call: 0 or a negative errno. with_event is 0 when the program passed no event structure.
typedef long (*he_ctl_fn)(void* context, int epfd, int op, int fd, uint32_t events, uint64_t data, int with_event);

static inline uint32_t he_bucket_fd(int32_t epfd, int32_t fd)
{
    uint32_t h = (uint32_t)epfd * 0x9E3779B1u + (uint32_t)fd;
    h ^= h >> 15;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    return h & (HE_BUCKETS - 1);
}

static inline uint32_t he_bucket_data(int32_t epfd, uint64_t data)
{
    uint64_t h = data ^ ((uint64_t)(uint32_t)epfd << 40);
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    return (uint32_t)h & (HE_BUCKETS - 1);
}

// Item index + 1 of the registration (epfd, fd), or 0.
static inline int he_find(const struct he_table* t, int32_t epfd, int32_t fd)
{
    for (int32_t i = t->by_fd[he_bucket_fd(epfd, fd)]; i; i = t->item[i - 1].next_fd)
        if (t->item[i - 1].epfd == epfd && t->item[i - 1].fd == fd) return i;
    return 0;
}

// The armed registration of this epoll set whose user data is `data`, or 0.
static inline int he_find_armed_data(const struct he_table* t, int32_t epfd, uint64_t data)
{
    for (int32_t i = t->by_data[he_bucket_data(epfd, data)]; i; i = t->item[i - 1].next_data) {
        const struct he_item* it = &t->item[i - 1];
        if (it->epfd == epfd && it->data == data && it->state == HE_STATE_ARMED) return i;
    }
    return 0;
}

// An event names its registration only by the user data, so two armed registrations of one epoll set must not share it.
static inline int he_data_taken(const struct he_table* t, int32_t epfd, uint64_t data, int32_t fd)
{
    const int i = he_find_armed_data(t, epfd, data);
    return i && t->item[i - 1].fd != fd;
}

static inline int he_has_room(const struct he_table* t)
{
    return t->free_list != 0 || t->top < HE_ITEMS;
}

static inline void he_unlink(struct he_table* t, int index)
{
    struct he_item* it = &t->item[index - 1];
    int32_t* link = &t->by_fd[he_bucket_fd(it->epfd, it->fd)];
    while (*link && *link != index) link = &t->item[*link - 1].next_fd;
    if (*link == index) *link = it->next_fd;
    link = &t->by_data[he_bucket_data(it->epfd, it->data)];
    while (*link && *link != index) link = &t->item[*link - 1].next_data;
    if (*link == index) *link = it->next_data;
    if (it->state == HE_STATE_ARMED) t->armed--;
    else if (it->state == HE_STATE_FIRED) t->fired--;
    it->state = HE_STATE_FREE;
    it->next_free = t->free_list;
    t->free_list = index;
}

// A new armed registration. The caller has made sure there is room.
static inline void he_link(struct he_table* t, int32_t epfd, int32_t fd, uint64_t data, uint32_t events)
{
    int32_t index;
    if (t->free_list) {
        index = t->free_list;
        t->free_list = t->item[index - 1].next_free;
    } else if (t->top < HE_ITEMS) {
        index = ++t->top;
    } else {
        return;
    }
    struct he_item* it = &t->item[index - 1];
    it->epfd = epfd;
    it->fd = fd;
    it->data = data;
    it->events = events;
    it->state = HE_STATE_ARMED;
    uint32_t bucket = he_bucket_fd(epfd, fd);
    it->next_fd = t->by_fd[bucket];
    t->by_fd[bucket] = index;
    bucket = he_bucket_data(epfd, data);
    it->next_data = t->by_data[bucket];
    t->by_data[bucket] = index;
    t->armed++;
}

// The program re-arms with MOD. For a fired registration the kernel has nothing to modify, so the registration is added again;
// if the kernel does still have it (it was added behind our back), it is modified.
static inline long he_send(he_ctl_fn kernel, void* context, int epfd, int op, int fd, uint32_t events, uint64_t data, int fired, int with_event)
{
    if (op == HE_CTL_MOD && fired) {
        long r = kernel(context, epfd, HE_CTL_ADD, fd, events, data, with_event);
        if (r == -HE_EEXIST) r = kernel(context, epfd, HE_CTL_MOD, fd, events, data, with_event);
        return r;
    }
    return kernel(context, epfd, op, fd, events, data, with_event);
}

// The program's epoll_ctl. Returns what the program should see: 0 or a negative errno.
static inline long he_ctl(struct he_table* t, he_ctl_fn kernel, void* context, int epfd, int op, int fd, uint32_t events, uint64_t data, int with_event)
{
    const int current = he_find(t, epfd, fd);
    const int oneshot = with_event && (op == HE_CTL_ADD || op == HE_CTL_MOD) && (events & HE_ONESHOT) != 0;
    if (!current && !oneshot) return kernel(context, epfd, op, fd, events, data, with_event);
    const int fired = current && t->item[current - 1].state == HE_STATE_FIRED;
    long r;
    if (op == HE_CTL_DEL) {
        if (fired) {
            he_unlink(t, current);
            return 0;
        }
        r = kernel(context, epfd, op, fd, events, data, with_event);
        if (r == 0 || r == -HE_ENOENT || r == -HE_EBADF) he_unlink(t, current);
        return r;
    }
    if (oneshot && !he_data_taken(t, epfd, data, fd) && (current || he_has_room(t))) {
        r = he_send(kernel, context, epfd, op, fd, events & ~HE_ONESHOT, data, fired, 1);
        if (r == 0) {
            if (current) he_unlink(t, current);
            he_link(t, epfd, fd, data, events);
        }
        return r;
    }
    // An ordinary registration, or a one-shot one that cannot be tracked (table full, user data shared with another armed
    // registration): it goes to the kernel as asked, and whatever we remembered about this descriptor is stale.
    r = he_send(kernel, context, epfd, op, fd, events, data, fired, with_event);
    if (r == 0 && current) he_unlink(t, current);
    return r;
}

// epoll_wait reported `count` events for `epfd`. `events` is the program's array: `stride` bytes per event, the 64-bit user data
// at `data_offset` in each. Every armed one-shot registration that was reported fires: the kernel's copy is deleted.
static inline void he_wait_done(struct he_table* t, he_ctl_fn kernel, void* context, int epfd, const void* events, size_t stride, size_t data_offset, int count)
{
    for (int i = 0; i < count; i++) {
        uint64_t data;
        memcpy(&data, (const unsigned char*)events + (size_t)i * stride + data_offset, sizeof(data));
        const int index = he_find_armed_data(t, epfd, data);
        if (!index) continue;
        t->item[index - 1].state = HE_STATE_FIRED;
        t->armed--;
        t->fired++;
        kernel(context, epfd, HE_CTL_DEL, t->item[index - 1].fd, 0, 0, 0);
    }
}

// A new epoll descriptor with this number exists, so registrations remembered for an earlier one are stale.
static inline void he_purge_epfd(struct he_table* t, int epfd)
{
    for (int i = 1; i <= t->top; i++)
        if (t->item[i - 1].state != HE_STATE_FREE && t->item[i - 1].epfd == epfd) he_unlink(t, i);
}

#endif
