#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <AbilityKit/native_child_process.h>
__attribute__((constructor)) static void PreloadAudit()
{
    const char* enabled=getenv("HISH_PRELOAD_AUDIT");
    if(enabled&&!strcmp(enabled,"1")){
        const char text[]="HISH_OHOS_PRELOAD_CONSTRUCTOR_OK\n";
        write(1,text,sizeof(text)-1);
    }
}
extern "C" __attribute__((visibility("default"))) void HiSHRouteChild(NativeChildProcess_Args) {}
extern "C" __attribute__((visibility("default"))) OHIPCRemoteStub* NativeChildProcess_OnConnect() { return nullptr; }
extern "C" __attribute__((visibility("default"))) void NativeChildProcess_MainProc() {}
extern "C" __attribute__((visibility("default"))) int HiSHAuditConst42() { return 42; }
extern "C" __attribute__((visibility("default"))) int HiSHProbePayload(int fd)
{
    const char text[] = "NATIVE_MODULE_OK\n";
    return write(fd, text, sizeof(text) - 1) == sizeof(text) - 1 ? 42 : -1;
}
