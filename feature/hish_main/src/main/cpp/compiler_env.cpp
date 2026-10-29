#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <zlib.h>

#ifdef HISH_COMPILER_DATA_VERSION
extern "C" const unsigned char HiSHCompilerData[], HiSHCompilerDataEnd[];
static bool Directory(const std::string& path)
{
    if (mkdir(path.c_str(), 0700) && errno != EEXIST) return false;
    struct stat info {};
    return !lstat(path.c_str(), &info) && S_ISDIR(info.st_mode);
}
static bool Save(const std::string& path, const void* bytes, size_t size)
{
    std::string temporary = path + ".XXXXXX";
    int fd = mkostemp(temporary.data(), O_CLOEXEC);
    if (fd < 0) return false;
    size_t at = 0;
    while (at < size) {
        ssize_t n = write(fd, static_cast<const char*>(bytes) + at, size - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        at += n;
    }
    const bool closed = !close(fd);
    if (at == size && closed && !rename(temporary.c_str(), path.c_str())) return true;
    unlink(temporary.c_str()); return false;
}
static uint32_t Word(const unsigned char* data)
{
    return uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
}
static bool Install(const std::string& root)
{
    const std::string marker = root + "/.version";
    char previous[65] = {};
    const int fd = open(marker.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) { const ssize_t n = read(fd, previous, sizeof(previous)); close(fd); if (n == 64 && !memcmp(previous, HISH_COMPILER_DATA_VERSION, 64)) return true; }
    if (HiSHCompilerDataEnd - HiSHCompilerData < 4) return false;
    const uint32_t expanded = Word(HiSHCompilerData);
    if (!expanded || expanded > 64 * 1024 * 1024) return false;
    std::vector<unsigned char> data(expanded);
    uLongf size = expanded;
    if (uncompress(data.data(), &size, HiSHCompilerData + 4, HiSHCompilerDataEnd - HiSHCompilerData - 4) != Z_OK || size != expanded) return false;
    const auto* at = data.data(); const auto* end = at + size;
    while (at < end) {
        if (end - at < 8) return false;
        uint32_t names = Word(at), bytes = Word(at + 4); at += 8;
        if (!names || names > 512 || size_t(end - at) < size_t(names) + bytes) return false;
        const std::string name(reinterpret_cast<const char*>(at), names); at += names;
        if (name[0] == '/' || name.find("..") != std::string::npos || name.find('\0') != std::string::npos) return false;
        for (size_t slash = name.find('/'); slash != std::string::npos; slash = name.find('/', slash + 1)) {
            if (!Directory(root + "/" + name.substr(0, slash))) return false;
        }
        if (!Save(root + "/" + name, at, bytes)) return false;
        at += bytes;
    }
    return Save(marker, HISH_COMPILER_DATA_VERSION, 64);
}
#endif

// This installs compiler input data, never executable mappings or permissions.
extern "C" void HiSHPrepareCompilerEnvironment()
{
#ifdef HISH_COMPILER_DATA_VERSION
    const char* prefix = getenv("PREFIX"); if (!prefix) return;
    const std::string share = std::string(prefix) + "/share", hish = share + "/hish", root = hish + "/toolchain";
    if (!Directory(share) || !Directory(hish) || !Directory(root) || !Install(root)) return;
    setenv("HISH_TOOLCHAIN_ROOT", root.c_str(), 0);
    setenv("C_INCLUDE_PATH", (root + "/include").c_str(), 0);
    setenv("LIBRARY_PATH", (root + "/lib").c_str(), 0);
#endif
}
