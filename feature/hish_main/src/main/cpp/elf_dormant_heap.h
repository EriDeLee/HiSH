#ifndef HISH_ELF_DORMANT_HEAP_H
#define HISH_ELF_DORMANT_HEAP_H
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// The static ArkTS runtime keeps its managed objects in a large read/write
// anonymous reservation near the bottom of the address space, because 32-bit
// object pointers must stay inside the first 4 GiB. The commercial system maps
// it at 0x00040000 with a 512 MiB size on the tested firmware, which makes the
// whole 0x00140000-0x20040000 range unusable for fixed-address executables.
//
// A terminal session is a fork copy of the app process, so it inherits that
// reservation even though no ArkTS code will ever run there again. Fixed-address
// (ET_EXEC) programs are refused with EEXIST when their image overlaps it.
//
// Releasing is allowed only for a run that is provably part of such a dormant
// copy: it starts with a private read/write anonymous [anon:ArkTs Static...]
// mapping at zero offset, device and inode, may continue into unnamed mappings
// with no gap, stays aligned to the 256 KiB pool size at both ends, and fits the
// 4 GiB object-space limit. Any other mapped byte in the target range refuses
// the whole plan, so the caller keeps the original EEXIST failure.
namespace hish {
struct DormantRange { uintptr_t begin = 0, end = 0; };
namespace detail {
constexpr uintptr_t DormantPoolAlignment = 256 * 1024;
constexpr uint64_t DormantObjectLimit = 4ULL * 1024 * 1024 * 1024;
struct MapsLine {
    uintptr_t begin = 0, end = 0;
    bool readable = false, writable = false, priv = false, zeroOffset = false,
        zeroDevice = false, zeroInode = false, unnamed = false, arktsStatic = false;
};
inline bool HexField(const char*& at, const char* end, uintptr_t& value)
{
    uintptr_t result = 0; const char* start = at;
    for (; at != end; ++at) {
        const char c = *at; unsigned digit;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A') + 10;
        else break;
        result = (result << 4) | digit;
    }
    value = result; return at != start;
}
inline bool DecimalField(const char*& at, const char* end, uintptr_t& value)
{
    uintptr_t result = 0; const char* start = at;
    for (; at != end; ++at) {
        if (*at < '0' || *at > '9') break;
        result = result * 10 + static_cast<unsigned>(*at - '0');
    }
    value = result; return at != start;
}
inline bool SkipSpace(const char*& at, const char* end)
{
    const char* start = at;
    while (at != end && *at == ' ') ++at;
    return at != start;
}
// Reads one /proc/self/maps line. A line that cannot be understood makes the
// whole plan fail, because an unknown mapping must never be overlapped.
inline bool ParseMapsLine(const char*& at, const char* end, MapsLine& line)
{
    if (!HexField(at, end, line.begin) || at == end || *at != '-') return false;
    ++at;
    if (!HexField(at, end, line.end) || line.end <= line.begin) return false;
    if (!SkipSpace(at, end) || end - at < 4) return false;
    line.readable = at[0] == 'r';
    line.writable = at[1] == 'w';
    line.priv = at[3] == 'p';
    at += 4;
    uintptr_t offset = 0, major = 0, minor = 0, inode = 0;
    if (!SkipSpace(at, end) || !HexField(at, end, offset)) return false;
    line.zeroOffset = offset == 0;
    if (!SkipSpace(at, end) || !HexField(at, end, major) || at == end || *at != ':') return false;
    ++at;
    if (!HexField(at, end, minor)) return false;
    line.zeroDevice = major == 0 && minor == 0;
    if (!SkipSpace(at, end) || !DecimalField(at, end, inode)) return false;
    line.zeroInode = inode == 0;
    while (at != end && *at == ' ') ++at;
    const char* name = at;
    while (at != end && *at != '\n') ++at;
    const char* nameEnd = at;
    while (nameEnd != name && (nameEnd[-1] == ' ' || nameEnd[-1] == '\r')) --nameEnd;
    const size_t nameLength = static_cast<size_t>(nameEnd - name);
    line.unnamed = nameLength == 0;
    static const char arktsPrefix[] = "[anon:ArkTs Static";
    const size_t arktsLength = sizeof(arktsPrefix) - 1;
    line.arktsStatic = nameLength >= arktsLength && !memcmp(name, arktsPrefix, arktsLength);
    if (at != end) ++at;
    return true;
}
inline bool ParseMaps(const char* maps, size_t size, std::vector<MapsLine>& lines)
{
    lines.clear();
    const char* at = maps;
    const char* end = maps + size;
    while (at != end) {
        MapsLine line;
        if (!ParseMapsLine(at, end, line)) return false;
        lines.push_back(line);
    }
    return !lines.empty();
}
inline bool Candidate(const MapsLine& line)
{
    return line.readable && line.writable && line.priv && line.zeroOffset && line.zeroDevice &&
        line.zeroInode && (line.arktsStatic || line.unnamed);
}
// Marks the lines of every run that is provably a dormant ArkTS reservation.
inline void MarkDormantRuns(const std::vector<MapsLine>& lines, std::vector<bool>& dormant)
{
    dormant.assign(lines.size(), false);
    size_t at = 0;
    while (at < lines.size()) {
        size_t runEnd = at + 1;
        while (runEnd < lines.size() && lines[runEnd].begin == lines[runEnd - 1].end) ++runEnd;
        const uintptr_t begin = lines[at].begin, end = lines[runEnd - 1].end;
        const uint64_t length = end - begin;
        bool allowed = lines[at].arktsStatic && begin % DormantPoolAlignment == 0 &&
            end % DormantPoolAlignment == 0 && length % DormantPoolAlignment == 0 &&
            length <= DormantObjectLimit;
        for (size_t i = at; allowed && i < runEnd; ++i) allowed = Candidate(lines[i]);
        if (allowed) for (size_t i = at; i < runEnd; ++i) dormant[i] = true;
        at = runEnd;
    }
}
}  // namespace detail
// Fills release with the parts of [low, high) that belong to a dormant ArkTS
// static heap copy. Returns false when any mapped byte in the range is not
// provably part of one, so callers keep the original failure in that case.
inline bool PlanDormantArkTsRelease(const char* maps, size_t size, uintptr_t low, uintptr_t high,
    std::vector<DormantRange>& release)
{
    release.clear();
    if (!maps || low >= high) return false;
    std::vector<detail::MapsLine> lines;
    std::vector<bool> dormant;
    if (!detail::ParseMaps(maps, size, lines)) return false;
    detail::MarkDormantRuns(lines, dormant);
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        if (line.end <= low || line.begin >= high) continue;
        if (!dormant[i]) { release.clear(); return false; }
        release.push_back({line.begin > low ? line.begin : low, line.end < high ? line.end : high});
    }
    return !release.empty();
}
}  // namespace hish
#endif
