#pragma once
#include <functional>
#include <string>

namespace hish {
void PtraceAudit(int reportFd, const std::function<void(const std::string&)>& log);
void PtraceAdvancedAudit(const std::string& root, int reportFd, const std::function<void(const std::string&)>& log);
void PtraceCompiledAudit(const std::string& root, int reportFd, const std::function<void(const std::string&)>& log);
}
