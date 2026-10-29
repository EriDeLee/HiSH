#include "network_data.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// Runs before entering Linux libc. This only installs public, packaged trust
// anchors in an application-owned path; it never disables TLS verification.
extern "C" void HiSHPrepareNetworkEnvironment()
{
    const char* prefix = getenv("PREFIX");
    if (!prefix || !*HiSHCACertificates) return;
    const std::string share = std::string(prefix) + "/share", directory = share + "/hish";
    if ((mkdir(share.c_str(), 0700) && errno != EEXIST) ||
        (mkdir(directory.c_str(), 0700) && errno != EEXIST)) return;
    const std::string certificates = directory + "/ca-bundle.pem";
    std::string temporary = certificates + ".XXXXXX";
    const int fd = mkostemp(temporary.data(), O_CLOEXEC);
    bool good = fd >= 0;
    size_t offset = 0;
    while (good && offset < sizeof(HiSHCACertificates) - 1) {
        const ssize_t count = write(fd, HiSHCACertificates + offset, sizeof(HiSHCACertificates) - 1 - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) good = false;
        else offset += count;
    }
    if (fd >= 0 && close(fd)) good = false;
    if (good && rename(temporary.c_str(), certificates.c_str())) good = false;
    if (!good) { if (fd >= 0) unlink(temporary.c_str()); return; }
    setenv("CURL_CA_BUNDLE", certificates.c_str(), 0);
    setenv("SSL_CERT_FILE", certificates.c_str(), 0);
    setenv("GIT_SSL_CAINFO", certificates.c_str(), 0);
    setenv("GIT_EXEC_PATH", (std::string(prefix) + "/bin").c_str(), 0);
    const std::string templates = directory + "/git-template";
    if (!mkdir(templates.c_str(), 0700) || errno == EEXIST) setenv("GIT_TEMPLATE_DIR", templates.c_str(), 0);
    setenv("GIT_CONFIG_NOSYSTEM", "1", 0);
}
