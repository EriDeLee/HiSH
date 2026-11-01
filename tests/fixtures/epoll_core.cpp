// Host fixture for feature/hish_main/src/main/cpp/hish_epoll_core.h.
// Two model kernels: one behaves as Linux does (EPOLLONESHOT disarms a registration once it has been reported), the other as this
// phone's does (the flag is ignored). The real bookkeeping sits on the second one and must look like the first. Directed cases pin
// the corner rules; a random walk compares the two step by step.
#include "hish_epoll_core.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <map>
#include <set>
#include <utility>
#include <vector>

static int failures;
static void Expect(bool condition, const char* name)
{
    if (condition) { printf("ok %s\n", name); return; }
    ++failures;
    printf("FAIL %s\n", name);
}

static const uint32_t IN = 0x1, OUT = 0x4, ERR = 0x8, HUP = 0x10;
using Reported = std::vector<std::pair<uint64_t, uint32_t>>;   // (user data, events), sorted

struct Registration { uint32_t events; uint64_t data; bool disarmed; };

struct Kernel {
    bool honours;                                       // does it implement EPOLLONESHOT?
    std::set<int> fds;                                  // descriptors that exist
    std::map<std::pair<int, int>, Registration> regs;   // (epoll set, descriptor)
    std::map<int, uint32_t> ready;                      // descriptor -> what is ready on it
    unsigned calls = 0;
    explicit Kernel(bool honour) : honours(honour) {}
    void Close(int fd)
    {
        fds.erase(fd);
        for (auto it = regs.begin(); it != regs.end();) it = it->first.second == fd ? regs.erase(it) : std::next(it);
    }
    long Ctl(int epfd, int op, int fd, uint32_t events, uint64_t data, int with_event)
    {
        ++calls;
        if (!fds.count(fd)) return -HE_EBADF;
        const auto key = std::make_pair(epfd, fd);
        const auto found = regs.find(key);
        if (op == HE_CTL_DEL) {
            if (found == regs.end()) return -HE_ENOENT;
            regs.erase(found);
            return 0;
        }
        if (!with_event) return -14;
        if (op == HE_CTL_ADD) {
            if (found != regs.end()) return -HE_EEXIST;
            regs[key] = Registration{events | ERR | HUP, data, false};
            return 0;
        }
        if (op == HE_CTL_MOD) {
            if (found == regs.end()) return -HE_ENOENT;
            found->second = Registration{events | ERR | HUP, data, false};
            return 0;
        }
        return -22;
    }
    Reported Wait(int epfd)
    {
        Reported out;
        for (auto& entry : regs) {
            if (entry.first.first != epfd || entry.second.disarmed) continue;
            const uint32_t revents = ready[entry.first.second] & ((entry.second.events & 0xFFFFu) | ERR | HUP);
            if (!revents) continue;
            out.emplace_back(entry.second.data, revents);
            if (honours && (entry.second.events & HE_ONESHOT)) entry.second.disarmed = true;
        }
        std::sort(out.begin(), out.end());
        return out;
    }
};

// The program's array of epoll_event on AArch64: 4 bytes of events, 4 of padding, 8 of user data.
struct Event { uint32_t events; uint32_t pad; uint64_t data; };

struct Shim {
    Kernel kernel;
    he_table* table;
    Shim() : kernel(false), table(static_cast<he_table*>(calloc(1, sizeof(he_table)))) {}
    ~Shim() { free(table); }
    Shim(const Shim&) = delete;
    Shim& operator=(const Shim&) = delete;
    static long Callback(void* context, int epfd, int op, int fd, uint32_t events, uint64_t data, int with_event)
    {
        return static_cast<Kernel*>(context)->Ctl(epfd, op, fd, events, data, with_event);
    }
    long Ctl(int epfd, int op, int fd, uint32_t events, uint64_t data, int with_event = 1)
    {
        return he_ctl(table, Callback, &kernel, epfd, op, fd, events, data, with_event);
    }
    Reported Wait(int epfd)
    {
        const Reported got = kernel.Wait(epfd);
        std::vector<Event> events;
        for (const auto& entry : got) events.push_back(Event{entry.second, 0, entry.first});
        he_wait_done(table, Callback, &kernel, epfd, events.data(), sizeof(Event), offsetof(Event, data), static_cast<int>(events.size()));
        return got;
    }
    int Armed() const { return table->armed; }
    int Fired() const { return table->fired; }
};

static void Open(Kernel& kernel, std::initializer_list<int> fds, uint32_t ready)
{
    for (int fd : fds) { kernel.fds.insert(fd); kernel.ready[fd] = ready; }
}

// xorshift64*: the same walk on every run.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint32_t Next(uint32_t bound)
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return static_cast<uint32_t>((state * 0x2545F4914F6CDD1Dull) >> 33) % bound;
    }
};

int main()
{
    // The model kernels themselves.
    {
        Kernel linux_(true), phone(false);
        Open(linux_, {5}, OUT);
        Open(phone, {5}, OUT);
        linux_.Ctl(10, HE_CTL_ADD, 5, OUT | HE_ONESHOT, 7, 1);
        phone.Ctl(10, HE_CTL_ADD, 5, OUT | HE_ONESHOT, 7, 1);
        const Reported a = linux_.Wait(10), b = linux_.Wait(10), c = phone.Wait(10), d = phone.Wait(10);
        Expect(a.size() == 1 && b.empty(), "model: Linux reports a one-shot registration once");
        Expect(c.size() == 1 && d.size() == 1, "model: the phone reports it every time");
    }

    // A one-shot registration is reported once, and re-arming with MOD reports it once more.
    {
        Shim s;
        Open(s.kernel, {5}, OUT);
        Expect(s.Ctl(10, HE_CTL_ADD, 5, OUT | HE_ONESHOT, 7) == 0, "ADD with EPOLLONESHOT succeeds");
        Expect(s.kernel.regs[{10, 5}].events == (OUT | ERR | HUP), "the kernel is told an ordinary level-triggered registration");
        const Reported first = s.Wait(10), second = s.Wait(10), third = s.Wait(10);
        Expect(first.size() == 1 && first[0].first == 7 && first[0].second == OUT, "the first wait reports the registration with its user data");
        Expect(second.empty() && third.empty(), "later waits are silent");
        Expect(s.Armed() == 0 && s.Fired() == 1 && s.kernel.regs.empty(), "the kernel's copy is gone and the table remembers it as fired");
        Expect(s.Ctl(10, HE_CTL_MOD, 5, OUT | HE_ONESHOT, 7) == 0, "MOD re-arms it");
        Expect(s.Armed() == 1 && s.Fired() == 0, "armed again");
        const Reported again = s.Wait(10), after = s.Wait(10);
        Expect(again.size() == 1 && after.empty(), "reported once more, then silent");
        Expect(s.Ctl(10, HE_CTL_MOD, 5, OUT | HE_ONESHOT, 7) == 0 && s.Wait(10).size() == 1 && s.Wait(10).empty(), "and again, any number of times");
    }

    // Ordinary registrations are not touched.
    {
        Shim s;
        Open(s.kernel, {5}, OUT);
        Expect(s.Ctl(10, HE_CTL_ADD, 5, OUT, 7) == 0 && s.Armed() + s.Fired() == 0, "a level-triggered registration is not tracked");
        Expect(s.Wait(10).size() == 1 && s.Wait(10).size() == 1 && s.Wait(10).size() == 1, "it keeps being reported");
        Expect(s.kernel.calls == 1, "and cost one kernel call");
        Expect(s.Ctl(10, HE_CTL_DEL, 5, 0, 0, 0) == 0 && s.kernel.regs.empty(), "DEL goes to the kernel");
    }

    // Switching between one-shot and ordinary.
    {
        Shim s;
        Open(s.kernel, {5}, OUT);
        s.Ctl(10, HE_CTL_ADD, 5, OUT | HE_ONESHOT, 7);
        Expect(s.Ctl(10, HE_CTL_MOD, 5, OUT, 7) == 0 && s.Armed() + s.Fired() == 0, "MOD to an ordinary registration forgets the one-shot");
        Expect(s.Wait(10).size() == 1 && s.Wait(10).size() == 1, "which is then reported every time");
        Expect(s.Ctl(10, HE_CTL_MOD, 5, OUT | HE_ONESHOT, 7) == 0 && s.Armed() == 1, "MOD back to one-shot tracks it");
        s.Wait(10);
        Expect(s.Fired() == 1 && s.Ctl(10, HE_CTL_MOD, 5, IN, 7) == 0 && s.Armed() + s.Fired() == 0 && s.kernel.regs.count({10, 5}) == 1, "MOD of a fired one to an ordinary registration adds it to the kernel");
    }

    // DEL.
    {
        Shim s;
        Open(s.kernel, {5}, OUT);
        s.Ctl(10, HE_CTL_ADD, 5, OUT | HE_ONESHOT, 7);
        s.Wait(10);
        Expect(s.Ctl(10, HE_CTL_DEL, 5, 0, 0, 0) == 0 && s.Fired() == 0, "DEL of a fired registration succeeds, as it does for a disarmed one on Linux");
        Expect(s.Ctl(10, HE_CTL_DEL, 5, 0, 0, 0) == -HE_ENOENT, "a second DEL is ENOENT");
        Expect(s.Ctl(10, HE_CTL_MOD, 5, OUT | HE_ONESHOT, 7) == -HE_ENOENT && s.Armed() == 0, "MOD of a deleted registration is ENOENT and tracks nothing");
        s.Ctl(10, HE_CTL_ADD, 5, OUT | HE_ONESHOT, 7);
        Expect(s.Ctl(10, HE_CTL_DEL, 5, 0, 0, 0) == 0 && s.Armed() == 0 && s.kernel.regs.empty(), "DEL of an armed registration removes it from the kernel and the table");
        s.Ctl(10, HE_CTL_ADD, 5, OUT | HE_ONESHOT, 7);
        s.kernel.regs.clear();
        Expect(s.Ctl(10, HE_CTL_DEL, 5, 0, 0, 0) == -HE_ENOENT && s.Armed() == 0, "if the kernel lost an armed registration, DEL reports it and the table forgets it");
    }

    // The descriptor number is reused after a close.
    {
        Shim s;
        Open(s.kernel, {5}, OUT);
        s.Ctl(10, HE_CTL_ADD, 5, OUT | HE_ONESHOT, 7);
        s.Wait(10);
        s.kernel.Close(5);
        Open(s.kernel, {5}, IN);
        Expect(s.Ctl(10, HE_CTL_ADD, 5, IN, 9) == 0 && s.Armed() + s.Fired() == 0, "ADD for a reused descriptor number works and clears the old record");
        Open(s.kernel, {6}, OUT);
        s.Ctl(10, HE_CTL_ADD, 6, OUT | HE_ONESHOT, 8);
        s.kernel.Close(6);
        Open(s.kernel, {6}, OUT);
        Expect(s.Ctl(10, HE_CTL_ADD, 6, OUT | HE_ONESHOT, 8) == 0 && s.Armed() == 1, "so does ADD over a record that was still armed");
    }

    // The kernel still holds a registration the table thinks is gone.
    {
        Shim s;
        Open(s.kernel, {5}, OUT);
        s.Ctl(10, HE_CTL_ADD, 5, OUT | HE_ONESHOT, 7);
        s.Wait(10);
        s.kernel.Ctl(10, HE_CTL_ADD, 5, OUT, 7, 1);
        Expect(s.Ctl(10, HE_CTL_MOD, 5, OUT | HE_ONESHOT, 7) == 0 && s.Armed() == 1, "re-arming falls back to MOD when ADD says EEXIST");
    }

    // Errors leave the table alone.
    {
        Shim s;
        Expect(s.Ctl(10, HE_CTL_ADD, 99, OUT | HE_ONESHOT, 7) == -HE_EBADF && s.Armed() == 0, "ADD of a closed descriptor fails and tracks nothing");
        Open(s.kernel, {5}, OUT);
        s.Ctl(10, HE_CTL_ADD, 5, OUT | HE_ONESHOT, 7);
        Expect(s.Ctl(10, HE_CTL_ADD, 5, OUT | HE_ONESHOT, 8) == -HE_EEXIST && s.Armed() == 1, "ADD of an armed registration is EEXIST and keeps the record");
        Expect(s.Wait(10)[0].first == 7, "with its old user data");
        Expect(s.Ctl(10, HE_CTL_ADD, 5, 0, 0, 0) == -14 && s.Fired() == 1, "an ADD without an event structure gets the kernel's error and changes nothing");
    }

    // User data changes on MOD.
    {
        Shim s;
        Open(s.kernel, {5}, OUT);
        s.Ctl(10, HE_CTL_ADD, 5, OUT | HE_ONESHOT, 1);
        s.Ctl(10, HE_CTL_MOD, 5, OUT | HE_ONESHOT, 2);
        Expect(!he_find_armed_data(s.table, 10, 1) && he_find_armed_data(s.table, 10, 2), "the record follows the new user data");
        const Reported got = s.Wait(10);
        Expect(got.size() == 1 && got[0].first == 2 && s.Wait(10).empty(), "and fires on it");
    }

    // Two armed registrations of one epoll set with the same user data cannot be told apart by an event.
    {
        Shim s;
        Open(s.kernel, {5, 6}, OUT);
        s.Ctl(10, HE_CTL_ADD, 5, OUT | HE_ONESHOT, 42);
        Expect(s.Ctl(10, HE_CTL_ADD, 6, OUT | HE_ONESHOT, 42) == 0 && s.Armed() == 1, "the second is not tracked");
        Expect((s.kernel.regs[{10, 6}].events & HE_ONESHOT) != 0, "it goes to the kernel as asked, flag included");
        s.Wait(10);
        Expect(s.kernel.regs.count({10, 5}) == 0 && s.kernel.regs.count({10, 6}) == 1, "an event removes only the tracked one");
        Expect(s.Ctl(11, HE_CTL_ADD, 5, OUT | HE_ONESHOT, 42) == 0 && s.Armed() == 1, "the same user data in another epoll set is tracked");
    }

    // Different epoll sets are independent.
    {
        Shim s;
        Open(s.kernel, {5}, OUT);
        s.Ctl(10, HE_CTL_ADD, 5, OUT | HE_ONESHOT, 1);
        s.Ctl(11, HE_CTL_ADD, 5, OUT | HE_ONESHOT, 2);
        const Reported got = s.Wait(10);
        Expect(got.size() == 1 && got[0].first == 1 && s.Armed() == 1 && s.kernel.regs.count({11, 5}) == 1, "an event in one set leaves the same descriptor in the other alone");
        he_purge_epfd(s.table, 10);
        Expect(s.Armed() == 1 && s.Fired() == 0, "purging a set forgets its records only");
    }

    // More one-shot registrations than the table holds.
    {
        Shim s;
        const int total = HE_ITEMS + 10;
        for (int fd = 0; fd < total; fd++) Open(s.kernel, {fd}, OUT);
        for (int fd = 0; fd < total; fd++) s.Ctl(10, HE_CTL_ADD, fd, OUT | HE_ONESHOT, 1000 + fd);
        Expect(s.Armed() == HE_ITEMS, "the table fills up");
        Expect((s.kernel.regs[{10, total - 1}].events & HE_ONESHOT) != 0, "the rest keep their flag");
        Expect(s.Wait(10).size() == static_cast<size_t>(total) && s.Wait(10).size() == 10, "after one wait only the untracked ones are still reported");
        s.Ctl(10, HE_CTL_DEL, 0, 0, 0, 0);
        Expect(s.Ctl(10, HE_CTL_MOD, total - 1, OUT | HE_ONESHOT, 1000 + total - 1) == 0 && s.Armed() == 1, "a freed slot is used again");
    }

    // A random walk: every answer and every report must match the Linux model.
    {
        long steps = 0, mismatches = 0;
        for (uint64_t seed = 1; seed <= 400; seed++) {
            Rng rng(seed);
            Kernel reference(true);
            Shim shim;
            for (int fd = 3; fd <= 8; fd++) { reference.fds.insert(fd); shim.kernel.fds.insert(fd); }
            std::set<std::pair<int, int>> stale;   // registrations that a close removed behind the table's back: only ADD is allowed
            uint64_t next_data = 1;
            const uint32_t kinds[] = {IN, OUT, IN | OUT};
            for (int step = 0; step < 300; step++) {
                ++steps;
                const int epfd = 10 + static_cast<int>(rng.Next(2)), fd = 3 + static_cast<int>(rng.Next(6));
                const auto key = std::make_pair(epfd, fd);
                const uint32_t events = kinds[rng.Next(3)] | (rng.Next(3) ? HE_ONESHOT : 0);
                const uint32_t what = rng.Next(100);
                if (what < 28) {
                    if (reference.regs.count(key)) continue;
                    const uint64_t data = next_data++;
                    const long a = reference.Ctl(epfd, HE_CTL_ADD, fd, events, data, 1), b = shim.Ctl(epfd, HE_CTL_ADD, fd, events, data);
                    stale.erase(key);
                    if (a != b) { ++mismatches; printf("seed %llu step %d: ADD %ld vs %ld\n", static_cast<unsigned long long>(seed), step, a, b); }
                } else if (what < 55) {
                    if (stale.count(key)) continue;
                    const uint64_t data = next_data++;
                    const long a = reference.Ctl(epfd, HE_CTL_MOD, fd, events, data, 1), b = shim.Ctl(epfd, HE_CTL_MOD, fd, events, data);
                    if (a != b) { ++mismatches; printf("seed %llu step %d: MOD %ld vs %ld\n", static_cast<unsigned long long>(seed), step, a, b); }
                } else if (what < 65) {
                    if (stale.count(key)) continue;
                    const long a = reference.Ctl(epfd, HE_CTL_DEL, fd, 0, 0, 0), b = shim.Ctl(epfd, HE_CTL_DEL, fd, 0, 0, 0);
                    if (a != b) { ++mismatches; printf("seed %llu step %d: DEL %ld vs %ld\n", static_cast<unsigned long long>(seed), step, a, b); }
                } else if (what < 80) {
                    const uint32_t masks[] = {0, IN, OUT, IN | OUT, ERR, HUP};
                    const uint32_t mask = masks[rng.Next(6)];
                    reference.ready[fd] = mask;
                    shim.kernel.ready[fd] = mask;
                } else if (what < 96) {
                    const Reported a = reference.Wait(epfd), b = shim.Wait(epfd);
                    if (a != b) { ++mismatches; printf("seed %llu step %d: WAIT reported %llu vs %llu events\n", static_cast<unsigned long long>(seed), step, static_cast<unsigned long long>(a.size()), static_cast<unsigned long long>(b.size())); }
                } else {
                    reference.Close(fd);
                    shim.kernel.Close(fd);
                    reference.fds.insert(fd);
                    shim.kernel.fds.insert(fd);
                    for (int set = 10; set <= 11; set++) stale.insert({set, fd});
                }
                if (mismatches > 5) break;
            }
            if (mismatches > 5) break;
        }
        printf("random walk: %ld steps, %ld mismatches\n", steps, mismatches);
        Expect(steps > 100000 && mismatches == 0, "a random walk of 120000 steps matches the Linux model");
    }

    if (failures) { printf("EPOLL_CORE_FAIL %d\n", failures); return 1; }
    printf("EPOLL_CORE_PASS\n");
    return 0;
}
