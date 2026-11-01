// Host fixture for feature/hish_main/src/main/cpp/system_applet_env.h.
// The kernel environment of a system applet and the restore done before its main.
#include "system_applet_env.h"
#include <cstdio>
#include <string>
#include <vector>

static int failures;
static void Expect(bool condition, const char* name)
{
    if (condition) { printf("ok %s\n", name); return; }
    ++failures; printf("FAIL %s\n", name);
}

using Strings = std::vector<std::string>;
// A mutable C environment block, as a process holds it.
struct Block {
    Strings text;
    std::vector<char*> pointers;
    explicit Block(const Strings& source) : text(source)
    {
        for (auto& entry : text) pointers.push_back(entry.data());
        pointers.push_back(nullptr);
    }
    Strings Read() const
    {
        Strings result;
        for (size_t i = 0; pointers[i]; i++) result.push_back(pointers[i]);
        return result;
    }
};
static Strings Built(const Strings& original)
{
    Block block(original);
    return HishSystemAppletEnvironment(block.pointers.data(), "/lib/app");
}
static Strings RoundTrip(const Strings& original)
{
    Block kernel(Built(original));
    HishRestoreSystemAppletEnvironment(kernel.pointers.data());
    return kernel.Read();
}
static size_t Count(const Strings& list, const std::string& entry)
{
    size_t count = 0;
    for (const auto& item : list) count += item == entry;
    return count;
}

int main()
{
    const Strings plain{"PATH=/a:/b", "HOME=/h"};
    Expect(RoundTrip(plain) == plain, "an environment without loader settings comes back unchanged");

    const Strings loader{"A=1", "LD_LIBRARY_PATH=/x:/y", "B=2", "LD_PRELOAD=/p.so", "GLIBC_TUNABLES=glibc.malloc.x=1", "C=3"};
    Expect(RoundTrip(loader) == loader, "the caller's LD_* and GLIBC_TUNABLES return in their original order");

    const Strings built = Built(loader);
    Expect(Count(built, "LD_PRELOAD=/lib/app/libhish_exec_bridge.so") == 1, "the kernel environment preloads the bridge library once");
    Expect(Count(built, "LD_LIBRARY_PATH=/lib/app:/system/lib64/ndk") == 1, "the kernel environment searches the app library directory once");
    Expect(Count(built, "HISH_SYSTEM_APPLET=1") == 1, "the kernel environment carries the marker once");
    size_t loaderEntries = 0;
    for (const auto& entry : built) loaderEntries += entry.rfind("LD_", 0) == 0 || entry.rfind("GLIBC_TUNABLES=", 0) == 0;
    Expect(loaderEntries == 2, "no caller loader setting reaches the applet's own startup");
    Expect(Count(built, "HISH_SAVED_LD_LIBRARY_PATH=/x:/y") == 1 && Count(built, "HISH_SAVED_GLIBC_TUNABLES=glibc.malloc.x=1") == 1,
        "the caller's settings travel as HISH_SAVED_*");

    const Strings lookalikes{"LDFLAGS=-L/x", "LDLIBS=-lm", "OLD_LD_X=1"};
    Expect(Built(lookalikes).size() == lookalikes.size() + 3 && RoundTrip(lookalikes) == lookalikes, "names that only look like loader settings are untouched");

    const Strings awkward{"X=a=b=c", "EMPTY=", "LD_LIBRARY_PATH=", "Y=with space"};
    Expect(RoundTrip(awkward) == awkward, "values with equals signs, spaces and empty values survive");

    const Strings stale{"HISH_SYSTEM_APPLET=1", "HISH_SAVED_LD_PRELOAD=/evil.so", "K=v"};
    Expect(RoundTrip(stale) == Strings{"K=v"}, "a stale marker or saved entry is dropped, never restored");

    Expect(Built(Strings{}).size() == 3, "an empty environment still gets the three startup settings");
    Expect(RoundTrip(Strings{}).empty(), "an empty environment round-trips to empty");

    HishRestoreSystemAppletEnvironment(nullptr);
    Block empty(Strings{});
    HishRestoreSystemAppletEnvironment(empty.pointers.data());
    Expect(empty.Read().empty(), "restoring a null or empty block does nothing");

    Block nothingSaved(Strings{"LD_PRELOAD=/lib/app/libhish_exec_bridge.so", "LD_LIBRARY_PATH=/lib/app:/system/lib64/ndk", "HISH_SYSTEM_APPLET=1", "Z=1"});
    HishRestoreSystemAppletEnvironment(nothingSaved.pointers.data());
    Expect(nothingSaved.Read() == Strings{"Z=1"}, "with nothing saved, the app's own settings simply disappear");

    puts(failures ? "SYSTEM_APPLET_ENV_FAIL" : "SYSTEM_APPLET_ENV_PASS");
    return failures ? 1 : 0;
}
