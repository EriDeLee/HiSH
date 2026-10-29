#pragma once
#include <functional>
#include <string>
namespace hish {
bool IsRouteAudit(const std::string& kind);
void RouteAudit(const std::string& root, const std::string& libs, const std::string& kind, int reportFd,
    const std::function<void(const std::string&)>& log);
}
