#ifndef HISH_SYSTEM_APPLET_ENV_H
#define HISH_SYSTEM_APPLET_ENV_H
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

// A system Toybox applet that starts other programs (env, timeout, find -exec, ...)
// runs with libhish_exec_bridge.so preloaded, so its execvp reaches the Linux loader.
// The preload and library path are settings for that process's own startup. What
// the caller had in LD_* travels renamed as HISH_SAVED_* and is put back before the
// applet's main runs, so the programs it starts see the caller's environment.
#define HISH_SYSTEM_APPLET_MARKER "HISH_SYSTEM_APPLET"
#define HISH_SAVED_PREFIX "HISH_SAVED_"

static inline bool HishHasPrefix(const char* text, const char* prefix)
{
    return !strncmp(text, prefix, strlen(prefix));
}
static inline bool HishLoaderSetting(const char* entry)
{
    return HishHasPrefix(entry, "LD_") || HishHasPrefix(entry, "GLIBC_TUNABLES=");
}

// Environment for the kernel exec of the system applet. Entries left over from an
// earlier hop are dropped so a stale marker or saved value can never be restored.
static inline std::vector<std::string> HishSystemAppletEnvironment(char* const* original, const std::string& libraries)
{
    std::vector<std::string> result;
    for (size_t i = 0; original && original[i]; i++) {
        const char* entry = original[i];
        if (HishHasPrefix(entry, HISH_SAVED_PREFIX) || HishHasPrefix(entry, HISH_SYSTEM_APPLET_MARKER "=")) continue;
        result.push_back(HishLoaderSetting(entry) ? std::string(HISH_SAVED_PREFIX) + entry : std::string(entry));
    }
    result.push_back("LD_PRELOAD=" + libraries + "/libhish_exec_bridge.so");
    result.push_back("LD_LIBRARY_PATH=" + libraries + ":/system/lib64/ndk");
    result.push_back(HISH_SYSTEM_APPLET_MARKER "=1");
    return result;
}

// Inverse, applied in place to the process environment before main: removes the
// app's own settings and the marker, and turns each HISH_SAVED_* entry back into
// the original (the rest of the string is already NAME=value).
static inline void HishRestoreSystemAppletEnvironment(char** environment)
{
    if (!environment) return;
    size_t kept = 0;
    for (size_t i = 0; environment[i]; i++) {
        char* entry = environment[i];
        if (HishHasPrefix(entry, HISH_SAVED_PREFIX)) entry += strlen(HISH_SAVED_PREFIX);
        else if (HishLoaderSetting(entry) || HishHasPrefix(entry, HISH_SYSTEM_APPLET_MARKER "=")) continue;
        environment[kept++] = entry;
    }
    environment[kept] = nullptr;
}
#endif
