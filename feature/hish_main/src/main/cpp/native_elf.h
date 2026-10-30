#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int HiSHHasNativeCommand(const char* name);
// Called only in shell/command processes. Success replaces control flow and never returns.
int HiSHExecFile(const char* path, char* const argv[]);
int HiSHExecPackagedImage(const char* path,char* const argv[],unsigned runtime,const char* executable);
int HiSHRunPtraceFile(const char* path,char* const argv[],const char* executable,int diagnostic,int notify);
int HiSHRunNativeCommand(char* const argv[]);
const char* HiSHExecError(void);
int HiSHNativeLibraryDirectory(char* buffer,unsigned capacity);
void HiSHPrepareToolWrappers(void);
void HiSHResetVirtualTerminal(void);
int HiSHHasApplet(const char* name);
void HiSHRunApplet(char** argv);
void HiSHInitializeApplets(void);
const char* HiSHSystemApplets(void);
const char* HiSHBuiltinApplets(void);
int HiSHExecNativeApplet(char* const argv[]);
#ifdef __cplusplus
}
#endif
