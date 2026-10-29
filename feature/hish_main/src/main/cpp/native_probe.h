#pragma once
#include <string>
namespace hish {
// Each destructive probe runs only on an explicit, separate application launch.
std::string Probe(const std::string& root, const std::string& kind);
}
