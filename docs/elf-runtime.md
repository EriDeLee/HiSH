# 通用 ELF 与 libc 兼容层

## 装载真实输入

`native_elf.cpp` 与 `linux/exec.c` 校验 ELF 类型、架构、段边界、地址范围、页重叠、可执行栈和权限。固定地址用自有地址预留，不覆盖既有非自有映射。默认代码路径不是历史 `hish-run`。

文件 RX 返回 EACCES/EPERM 时，`elf_code_mapping.h` 在已预留范围内建立匿名 RW，复制真实输入字节，清指令缓存，再转 RX；任何一步失败都终止。不创建 W+X，不按文件字节相同转发给另一个随包程序。普通私有 ELF 仍要求执行位。

## 内核 exec 桥

Linux 调用方经 `exec_bridge.cpp`、`exec_bridge_shared.h` 将有界参数和环境写入封存 memfd，利用允许执行的系统 Toybox 与 OHOS 引导库进入新进程地址空间，再装载目标 ELF。该桥保留已测的 PID、参数、环境、标准流和 CLOEXEC 语义；请求不写明文磁盘。

执行系统 OHOS 工具前清理 Linux 的 LD_PRELOAD/LD_LIBRARY_PATH。内核 `/proc/self/exe` 仍可能显示引导 Toybox；兼容层只对已实现的公开 readlink/open/exec 提供逻辑镜像。

`HISH_KERNEL_EXEC_BRIDGE=0` 是旧直接跳转对照，保留原映射，多线程时返回 ENOTSUP。默认桥也不等于完整 Linux exec；隐藏 libc、直接 syscall 和静态程序内部调用仍需独立验证。

## musl

固定 musl 1.2.5 源码及 Alpine 补丁提供符号、重定位、TLS 和初始化。`linux/musl_dso_mapping.h` 为 DSO 独立预留空间，再装载真实段；不依赖旧二进制偏移、零填充注入或重叠 R/RX 头视图。

库搜索由 `linux_library_path.h` 共用：随包运行库优先，显式应用私有绝对目录其次，`native/usr/lib/musl` 最后。realpath 后拒绝相对、空项及 native 根之外的目录；列表上限 32768 字节。此规则不是对同 UID 任意原生代码的隔离承诺。

当前 DSO 路径限制包括 4KiB 页、64MiB 文件、256MiB 映像、16MiB 对齐，不接受 W+X、代码 BSS、TEXTREL、可执行栈或重叠段页。

## glibc

平台构建禁用 rseq，将 libc 首个 RX LOAD 拆成 R 头和 RX 尾，并将唯一 clone3 调用点改为 ENOSYS 以走已有回退。指令匹配数量变化即构建失败。栈展开使用 `libgcc_s1.so`。这是平台兼容库适配，不是任意未修改 glibc 均可运行的证明。

## 公共兼容入口

两套执行/终端库覆盖已测的 exec、fd exec、spawn、system、popen/pclose、文件动作和信号/终端状态。closefrom 枚举实际描述符，避免已测手机会以 SIGSYS 拒绝的 close_range。exec 数据缓冲使用独立映射，避免 fork 后等待已消失线程的分配器锁。

身份适配仅支持查询/保持原身份，不授予新 UID/GID。普通文件执行、内核 PTY、任意依赖与完整 Linux 语义仍有限制。来源见 [组件](third-party.md)，历史失败见 [加载实验](history/loader-experiments.md)。
