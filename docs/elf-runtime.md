# 通用 ELF 与 libc 兼容层

## 装载真实输入

`native_elf.cpp` 与 `linux/exec.c` 校验 ELF 类型、架构、段边界、地址范围、页重叠、可执行栈和权限。固定地址用自有地址预留，不覆盖既有非自有映射。默认代码路径不是历史 `hish-run`。

## 固定地址程序与 ArkTS 静态堆

静态 ArkTS 运行时用 32 位对象指针，因此把对象堆放在内存前 4GiB：商用系统在实测固件上从 256KiB 处预留 512MiB，`/proc/self/maps` 显示为 `[anon:ArkTs Static ...]` 若干段加一段无名 `rw-p`，覆盖 `0x00140000-0x20040000`。OpenHarmony 源码中对应 `PANDA_32BITS_HEAP_START_ADDRESS`（按 256KiB 对齐）与 `MapRWAnonymousInFirst4GB` 的预留；商用源码不可见，此归属是按地址、大小和命名比对的推断。

终端会话是应用进程的 fork 副本，会继承这段预留，而这段内不会再有 ArkTS 代码运行。固定地址（ET_EXEC）程序如 Bun（`0x00200000-0x062ec000`）正好落在其中，预留时得到 EEXIST，用户可见为 `File exists`。

`elf_dormant_heap.h` 只在预留失败且错误为 EEXIST 时读一次 `/proc/self/maps`，仅当整个重叠区都属于那段可证明的休眠副本——以 `[anon:ArkTs Static` 开头、之后无空隙的匿名私有读写段、零偏移零设备零 inode、两端按 256KiB 对齐、总长不超过 4GiB——才释放重叠部分并重试一次；其他任何映射都使整次释放放弃，保留原报错。释放只发生在即将被替换的命令进程里，会话 shell 与应用进程的堆不受影响。

会话子进程设置 `HISH_SESSION_COPY=1`；没有该标记的进程不释放任何映射。不碰撞的程序不读内存表，因此没有额外开销。


文件 RX 返回 EACCES/EPERM 时，`elf_code_mapping.h` 在已预留范围内建立匿名 RW，复制真实输入字节，清指令缓存，再转 RX；任何一步失败都终止。不创建 W+X，不按文件字节相同转发给另一个随包程序。普通私有 ELF 仍要求执行位。

## 内核 exec 桥

Linux 调用方经 `exec_bridge.cpp`、`exec_bridge_shared.h` 将有界参数和环境写入封存 memfd，利用允许执行的系统 Toybox 与 OHOS 引导库进入新进程地址空间，再装载目标 ELF。该桥保留已测的 PID、参数、环境、标准流和 CLOEXEC 语义；请求不写明文磁盘。

执行系统 OHOS 工具前清理 Linux 的 LD_PRELOAD/LD_LIBRARY_PATH。内核 `/proc/self/exe` 仍可能显示引导 Toybox；兼容层只对已实现的公开 readlink/open/exec 提供逻辑镜像。

## 系统小程序预加载

系统 `/system/bin/toybox` 里会启动其他程序的小程序（`env`、`timeout`、`nice`、`nohup`、`setsid`、`taskset`、`time`、`find`）用系统 libc 的 `execvp`/`execve`，内核不会执行应用目录里的 Linux 程序；其余小程序用系统 libc 的文件函数，看不到模拟的符号链接（见 [符号链接](symlinks.md)）。`shell_module.c` 因此让每一个系统小程序都改用 `HiSHExecSystemApplet`：带着 `LD_PRELOAD=libhish_exec_bridge.so` 内核执行系统 Toybox，没有名单。该库导出 `execvp`、`execve`（`env` 自己按 PATH 搜索后调用它）和 `vfork`。前两个转入 `HiSHExecFile`，由它决定交给内核（OHOS 程序）还是本装载器（Linux 程序）；`vfork` 改为 `fork`，因为 vfork 子进程与父进程共享内存，而装载器会改写进程映像。同一个库还带着链接层的导出副本，由库的构造函数在确认是系统小程序后打开，在内核 exec 桥的引导进程里保持关闭。

预加载和库路径只是该进程自己启动所需。调用者原有的 `LD_*`、`GLIBC_TUNABLES` 改名为 `HISH_SAVED_*` 同行，库的构造函数在小程序 `main` 之前改回原名并删除标记 `HISH_SYSTEM_APPLET`（`system_applet_env.h`，主机测试覆盖往返），所以被启动的程序看到的环境与直接启动一致。HiSH 自己的装载代码用 `syscall(SYS_execve)` 进入内核，不经过这个被替换的符号，否则会循环。预加载启动失败时退回普通启动，小程序仍能运行但不能启动 Linux 程序。`env -i` 会清掉 `HISH_PACKAGE_ROOT`，随包程序因此找不到库；直接去掉该变量结果相同，与本机制无关。

`HISH_KERNEL_EXEC_BRIDGE=0` 是旧直接跳转对照，保留原映射，多线程时返回 ENOTSUP。默认桥也不等于完整 Linux exec；隐藏 libc、直接 syscall 和静态程序内部调用仍需独立验证。

普通 ELF 文件与映像上限均为 512MiB，直接运行、Linux 调用方 exec 和 PRoot 三个装载入口一致。装载只读 ELF 头、程序头和解释器名，各段从文件直接读入映像，不保留整文件副本。逻辑镜像通过已打开文件的 procfs 描述符解析为绝对路径，覆盖 `./program` 调用；否则合作式 readlink 和 Bun 的 `process.execPath` 会退回引导 Toybox。原始 syscall 查询仍显示内核镜像。

## musl

固定 musl 1.2.5 源码及 Alpine 补丁提供符号、重定位、TLS 和初始化。`linux/musl_dso_mapping.h` 为 DSO 独立预留空间，再装载真实段；不依赖旧二进制偏移、零填充注入或重叠 R/RX 头视图。

解释器的 ABI 由 `elf_abi.h` 统一判断，两个装载入口共用。只有标准名 `ld-musl-aarch64.so.1` 和 `ld-linux-aarch64.so.1` 才选择对应 libc；其他解释器报 `unsupported ELF interpreter/ABI (路径)`，不按子串猜测。没有 PT_INTERP 的静态程序走静态路径，这不代表其直接系统调用都已兼容。

库搜索由 `linux_library_path.h` 共用：随包运行库优先，显式应用私有绝对目录其次，然后是本进程 `HISH_PACKAGE_ROOT` 代次的 `usr/lib`、`lib`，`native/usr/lib/musl` 最后。realpath 后拒绝相对、空项及 native 根之外的目录；列表上限 32768 字节。此规则不是对同 UID 任意原生代码的隔离承诺。

当前 DSO 路径限制包括 4KiB 页、64MiB 文件、256MiB 映像、16MiB 对齐，不接受 W+X、代码 BSS、TEXTREL、可执行栈或重叠段页。

依赖 `libc.so.6`、`ld-linux-aarch64.so.1` 或 `libc++.so` 的 DSO 一律在重定位和构造函数之前拒绝。`libc.so` 既是 OHOS libc 的名字，也是 musl 的标准名：DSO 位于 HAP 库目录、`/data/storage/el1/bundle` 或应用存储 `/data/storage` 之外时视为 OHOS ABI 库并拒绝，读不到来源也拒绝；其他位置按上游 musl 解析为本 loader 自身，例如 Zig 构建、由 Bun 解出到 TMPDIR 的 opencode 界面库。拒绝的 errno 为 ELIBBAD，musl 没有它的文案，`dlerror` 显示 `No error information`。

## glibc

平台构建禁用 rseq，将 libc 首个 RX LOAD 拆成 R 头和 RX 尾，并将唯一 clone3 调用点改为 ENOSYS 以走已有回退。指令匹配数量变化即构建失败。栈展开使用 `libgcc_s1.so`。这是平台兼容库适配，不是任意未修改 glibc 均可运行的证明。

## 公共兼容入口

两套执行/终端库覆盖已测的 exec、fd exec、spawn、system、popen/pclose、文件动作和信号/终端状态。closefrom 枚举实际描述符，避免已测手机会以 SIGSYS 拒绝的 close_range。exec 数据缓冲使用独立映射，避免 fork 后等待已消失线程的分配器锁。

身份适配仅支持查询/保持原身份，不授予新 UID/GID。普通文件执行、内核 PTY、任意依赖与完整 Linux 语义仍有限制。来源见 [组件](third-party.md)，历史失败见 [加载实验](history/loader-experiments.md)。
