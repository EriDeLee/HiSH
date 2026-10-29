#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace hish {
struct ExecutablePatch {
    uintptr_t address;
    std::vector<unsigned char> bytes;
};
// Replaces only the caller's newly reserved image pages with private installed RX pages.
bool MapExecutableTemplate(void* address, size_t length);
// Experimental command semantics: a supervisor waits for one detached child.
// This deliberately does not claim PID-preserving exec semantics.
int RunPatchedImage(const std::vector<ExecutablePatch>& patches,
    const std::function<void()>& enter, bool diagnostic, bool notify);
}
