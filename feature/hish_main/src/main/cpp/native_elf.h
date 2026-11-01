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
// System Toybox applet that starts other programs; they may be Linux ELFs. Never returns on success.
int HiSHExecSystemApplet(char* const argv[]);
// execve with an explicit environment, entering HiSHExecFile (defined in mksh_adapter.c).
int HiSHMkshExecve(const char* path, char* const argv[], char* const envp[]);
// execvp for embedded and preloaded Toybox code: PATH lookup, then HiSHExecFile.
int HiSHToyboxExecvp(const char* file, char* const argv[]);
// Terminal session start: package migration plus the active generation path.
int HiSHPackageSession(char* buffer,unsigned capacity);
// Path of the CA bundle pkg trusts; 0 on success, -1 when it is unavailable.
int HiSHPackageCertificates(char* buffer,unsigned capacity);
#ifdef __cplusplus
}
#endif
