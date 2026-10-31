// Host fixture for feature/hish_main/src/main/cpp/elf_dormant_heap.h.
// Synthetic /proc/self/maps snapshots, shaped like the tested phone.
#include "elf_dormant_heap.h"
#include <cstdio>
#include <string>
#include <vector>

static int failures;
static void Expect(bool condition, const char* name)
{
    if (condition) { printf("ok %s\n", name); return; }
    ++failures; printf("FAIL %s\n", name);
}
static std::string DeviceMaps()
{
    return
        "00040000-00080000 rw-p 00000000 00:00 0                                  [anon:ArkTs Static Object Space]\n"
        "00080000-000c0000 rw-p 00000000 00:00 0                                  [anon:ArkTs Static Non Movable Space]\n"
        "000c0000-00100000 rw-p 00000000 00:00 0                                  [anon:ArkTs Static Object Space]\n"
        "00100000-00140000 rw-p 00000000 00:00 0                                  [anon:ArkTs Static Non Movable Space]\n"
        "00140000-20040000 rw-p 00000000 00:00 0\n"
        "1400000000-1400001000 ---p 00000000 00:00 0                              [anon:partition_alloc]\n"
        "59c5c40000-59c5cd2000 r--p 00000000 104:4f 930863                        /data/storage/el1/bundle/libs/arm64/libc++_shared.so\n"
        "59c6a02000-59c6a9a000 r-xp 00341000 104:4f 934770                        /data/storage/el1/bundle/libs/arm64/libhish_shell.so\n";
}
static bool Plan(const std::string& maps, uintptr_t low, uintptr_t high, std::vector<hish::DormantRange>& release)
{
    return hish::PlanDormantArkTsRelease(maps.data(), maps.size(), low, high, release);
}
int main()
{
    const std::string maps = DeviceMaps();
    std::vector<hish::DormantRange> release;
    // bun: fixed 0x200000-0x62ec000, entirely inside the unnamed continuation.
    Expect(Plan(maps, 0x200000, 0x62ec000, release) && release.size() == 1 &&
        release[0].begin == 0x200000 && release[0].end == 0x62ec000, "bun range inside the dormant reservation");
    // TinyCC default: 0x400000-0x441000, also inside it.
    Expect(Plan(maps, 0x400000, 0x441000, release) && release.size() == 1 &&
        release[0].begin == 0x400000 && release[0].end == 0x441000, "tcc default range inside the dormant reservation");
    // A range that starts below the first mapped page only releases the mapped part.
    Expect(Plan(maps, 0x30000, 0x50000, release) && release.size() == 1 &&
        release[0].begin == 0x40000 && release[0].end == 0x50000, "unmapped head above the reservation is not released");
    // File-backed mappings are never released.
    Expect(!Plan(maps, 0x59c5c40000, 0x59c5c50000, release), "file-backed library mapping is refused");
    // Executable bundle page is never released.
    Expect(!Plan(maps, 0x59c6a02000, 0x59c6a03000, release), "executable bundle page is refused");
    // Any other mapped byte in the same range refuses the whole plan.
    Expect(!Plan(maps, 0x200000, 0x59c5c50000, release), "range reaching a file-backed mapping is refused");
    Expect(!Plan(maps, 0x1400000000, 0x1400001000, release), "guard page mapping is refused");
    // An unnamed block without a preceding ArkTS Static name is not attributable.
    {
        const std::string alone = "00140000-20040000 rw-p 00000000 00:00 0\n";
        Expect(!Plan(alone, 0x200000, 0x62ec000, release), "unnamed block without ArkTS name is refused");
    }
    // The dynamic ArkTS heap uses different names and must not be touched.
    {
        const std::string dynamic = "00140000-20040000 rw-p 00000000 00:00 0 [anon:ArkTS Heap]\n";
        Expect(!Plan(dynamic, 0x200000, 0x62ec000, release), "dynamic ArkTS heap name is refused");
    }
    // Named ArkTS space that is not pool aligned at both ends is refused.
    {
        const std::string shifted =
            "00040000-00140000 rw-p 00000000 00:00 0 [anon:ArkTs Static Object Space]\n"
            "00140000-00141000 rw-p 00000000 00:00 0\n";
        Expect(!Plan(shifted, 0x141000, 0x141000 + 0x1000, release), "unaligned run end is refused");
    }
    // A run larger than the 4 GiB object-space limit is refused.
    {
        const std::string huge =
            "00040000-00080000 rw-p 00000000 00:00 0 [anon:ArkTs Static Object Space]\n"
            "00080000-1400080000 rw-p 00000000 00:00 0\n";
        Expect(!Plan(huge, 0x1000000, 0x2000000, release), "run beyond the object-space limit is refused");
    }
    // Read-only or shared anonymous memory inside a verified run is refused.
    {
        const std::string mixed =
            "00040000-00080000 rw-p 00000000 00:00 0 [anon:ArkTs Static Object Space]\n"
            "00080000-20040000 r--p 00000000 00:00 0\n";
        Expect(!Plan(mixed, 0x1000000, 0x2000000, release), "read-only continuation is refused");
    }
    printf(failures ? "DORMANT_HEAP_FAIL %d\n" : "DORMANT_HEAP_PASS\n", failures);
    return failures ? 1 : 0;
}
