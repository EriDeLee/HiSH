#pragma once
#include <functional>
#include <string>
namespace hish {
// Experimental scalar-query mediation. The kernel still executes the query via CONTINUE.
int CreateQueryListener(int reportFd);
bool SendQueryListener(int socket, int listener);
int ReceiveQueryListener(int socket);
bool ContinueQueryNotification(int listener);
// Never installs a filter in the application/session process.
void SeccompAudit(int reportFd, const std::function<void(const std::string&)>& log);
}
