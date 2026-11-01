// Host fixture for feature/hish_main/src/main/cpp/toybox_exec_path.h.
// Runs inside a scratch directory prepared by the test. The host has no execute
// bit, so a file counts as executable unless its path goes through "noexec/".
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

static int HostExecutable(const char* path)
{
    if (strstr(path, "noexec/")) { errno = EACCES; return -1; }
    return 0;
}
#define HISH_EXECUTABLE_FILE(path) HostExecutable(path)
#include "toybox_exec_path.h"

static int failures;
static void Expect(bool condition, const char* name)
{
    if (condition) { printf("ok %s\n", name); return; }
    ++failures; printf("FAIL %s\n", name);
}
// The resolved path, or "-<errno>" when the lookup fails.
static std::string Resolve(const char* name, const char* search, size_t capacity = PATH_MAX)
{
    std::string out(capacity ? capacity : 1, '\0');
    errno = 0;
    if (HishResolveCommand(name, search, &out[0], capacity)) return "-" + std::to_string(errno);
    return out.c_str();
}
static std::string Failed(int code) { return "-" + std::to_string(code); }

int main()
{
    Expect(Resolve("tool", "a:b") == "a/tool", "the first PATH entry wins");
    Expect(Resolve("tool", "b:a") == "b/tool", "PATH order decides, not name order");
    Expect(Resolve("tool", "dirs:a") == "a/tool", "a directory with the name is skipped");
    Expect(Resolve("both", "noexec:b") == "b/both", "an unusable file does not hide a later executable");
    Expect(Resolve("both", "b:noexec") == "b/both", "an executable beats a later unusable file");
    Expect(Resolve("only", "noexec:a") == Failed(EACCES), "only an unusable file reports EACCES");
    Expect(Resolve("only", "noexec:b") == Failed(EACCES), "EACCES survives later entries that lack the name");
    Expect(Resolve("nothing", "a:b") == Failed(ENOENT), "a missing command reports ENOENT");
    Expect(Resolve("here", "a::b") == "here", "an empty middle entry means the current directory");
    Expect(Resolve("here", "a:") == "here", "an empty trailing entry means the current directory");
    Expect(Resolve("here", ":a") == "here", "an empty leading entry means the current directory");
    Expect(Resolve("nothing", nullptr) == Failed(ENOENT), "an unset PATH uses the default and fails cleanly");
    Expect(Resolve("nothing", "") == Failed(ENOENT), "an empty PATH uses the default and fails cleanly");
    Expect(Resolve("a/tool", "a:b") == Failed(EINVAL), "a name with a slash is the caller's job");
    Expect(Resolve(std::string(NAME_MAX + 1, 'n').c_str(), "a") == Failed(ENAMETOOLONG), "a name beyond NAME_MAX is rejected");
    Expect(Resolve("tool", "a:b", 6) == Failed(ENAMETOOLONG), "a result that does not fit the buffer is rejected");
    Expect(Resolve("tool", "a:b", 7) == "a/tool", "a result that exactly fits the buffer is returned");
    Expect(Resolve("tool", "a:b", 0) == Failed(ENOENT), "a zero-size buffer is rejected");
    Expect(Resolve("tool", (std::string(PATH_MAX + 10, 'd') + ":a").c_str()) == "a/tool", "an over-long PATH entry is skipped");
    Expect(Resolve(nullptr, "a") == Failed(ENOENT), "a null name is rejected");
    Expect(Resolve("", "a") == Failed(ENOENT), "an empty name is rejected");
    puts(failures ? "TOYBOX_EXEC_FAIL" : "TOYBOX_EXEC_PASS");
    return failures ? 1 : 0;
}
