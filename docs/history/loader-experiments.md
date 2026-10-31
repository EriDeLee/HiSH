# 历史加载与进程路线实验

## 调查范围

2026-10-19 至 21 的 Mate60Pro/debug_hap 初轮为 24 组、151 个隔离子进程；补查为 30 组、219 个用例，另含官方 C/C++、ArkTS 子进程对照。计数代表覆盖，不证明未知路线不存在。原始 UID20020264 与后续 UID20020268 不能混用。

| 路线 | 当时观察 | 不能推出 |
| --- | --- | --- |
| 直接 fork/wait | 返回42，应用自有进程可运行 | 任意内核 exec 可用 |
| 系统 Toybox | uname 等已测命令可执行 | 任意系统工具或 shell 可用 |
| 系统 sh、HOME 内核 exec、PTY | EACCES，其他 fd/memfd 路径也未打通 | 所有用户态装载均失败 |
| 随包 dlopen、受控 PIE 导出 main | 调用成功；namespace/保留地址加载有可用变体 | 通用 ELF 启动或下载代码权限 |
| 自有用户态 ELF | 无 libc、静态 PIE、固定地址及动态解释器用例通过 | 任意地址、重定位或 glibc 兼容 |
| 框架 NativeChildProcess | 返回16010004，设备日志2097264 | 应用 fork 不可用 |
| ArkTS API26 子进程 | 支持查询 false，调用16000061/801 | 所有固件永不支持 |
| 官方 executableBinaryPaths | 手机安装9568449；官方范围PC/2in1 | 改文件名即可支持手机 |
| 匿名/共享/双映射等早期矩阵 | 多项拒绝；shmget 等有 SIGSYS | 新 Profile 或后续实现也必然失败 |

EL1/EL2、namespace、X-only、mremap、COW、已有 RX 页、HAP 容器映射、硬链接、共享内存和内联 syscall 均有独立历史用例。`dlopen_ns(nullptr)` 崩溃及猜错 EL1 目录的准备失败未当作最终排除依据。sdk/公开源码/设备可见性不是商业固件的完备证明。

## ptrace 与标准 PRoot

TRACEME/ATTACH/SEIZE、寄存器/内存、fork/clone/exec 事件、自有代码页 POKETEXT 可用。手机 TinyCC 新编译的120字节函数在 detach 后运行：输入0/1/37/100分别得到17/18/720/5067，TracerPid0，父进程原函数仍42。

该函数只接受无 `.text` 重定位、最多4096字节的自有对象，不能当完整程序证明。原 Alpine PRoot5.4.0 本体版本成功，但启动 HOME 静态/动态目标仍 Permission denied。固定包摘要 `6965c02f281b4d2c46e69f28b48eb9ceea443eb7bc2b9cad3af5163906a184c3`；没有 rootfs 或默认 PRoot 后端。

## 完整 ptrace ELF 与 seccomp

后续 `hish-run` 为自有目标预留映像，映射随包 RX 模板、写实际代码后 detach；动态符号/TLS仍交给解释器。限制64MiB输入、256MiB映像、16MiB待写代码，拒绝W+X、代码BSS和段页重叠。该实验有额外监督进程，PID不等于命令PID，旧 OHOS 映射仍在。

TinyCC 原默认0x400000与引导匿名映射0x00140000..0x20040000冲突，mrsh和bridge均EEXIST17；0x60000000受控链接成功。这促成后来编译器适配，不是通用固定地址重定位。该匿名映射现已归因为静态 ArkTS 运行时的对象堆预留，处理见 [ELF 运行时](../elf-runtime.md)。

seccomp action 查询errno95、sizes22，NEW_LISTENER22；NO_NEW_PRIVS成功不等于用户通知可用。`--notify` 的 getppid CONTINUE 路径未通过，父端EPROTO71是次生错误，不能以聚合失败否定其他通过用例，也不能静默降级通知。

最终历史 signed HAP SHA256 `38930c8ac9674cec450a650e5ad66410b18b9ccdfe008b449043ed25be9c9795`。证据位于旧任务 `C:/Users/Lee/Documents/Codex/2026-10-18/wo-xia/work/hish-combined-final/`；交付结果索引在该任务 `outputs/hish-combined-evidence/results.json`。此实验代码保留，但不进默认包。

## 当前默认路径的由来

2026-10-28 将自有匿名 RW/copy/RX 接入普通 `./`，受控现编、多线程、文件、fd执行、执行位负面对照通过；不再要求hish-run。初始误拒绝安装器0644工具的问题以已登记inode修复，不对HOME副本转发。历史基线包 `hish-exec-1/phone-signed.hap` SHA256 `19f9b616a55b87da488f1362177d591aaa66b34fef2574627294094bb462efe1`；最终结果只看该目录 `verified/`，initial/final 中有测试入口及0644误拒绝的中间失败。

## 主要参考

- [Native子进程接口](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/capi-native-child-process-h)、[独立进程设备限制](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/isolation-process-development-guideline)
- [NDK libc权限限制](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/guidance-on-ndk-libc-interfaces-affected-by-permissions)、[HAP bin](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/hap-bin)
- [共享库/namespace](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/integrate-third-party-dlls)、[Seccomp调用列表](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/seccomp-symbol)
- [内核seccomp](https://docs.kernel.org/userspace-api/seccomp_filter.html)、[用户通知及TOCTOU](https://man7.org/linux/man-pages/man2/seccomp_unotify.2.html)
- [PRoot上游](https://github.com/proot-me/proot)、[Alpine PRoot配方](https://gitlab.alpinelinux.org/alpine/aports/-/tree/a9e2a2dc46f377eb25884f7fea9834492de7f518/community/proot)
