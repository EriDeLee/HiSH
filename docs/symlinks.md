# 符号链接模拟

手机拒绝应用在任何可写目录创建符号链接和硬链接（`symlink`、`link` 返回 EACCES，官方文档也把它们列为受 SELinux 管控，见 [验收](verification.md)）。HiSH 在自己的 libc 层模拟符号链接，覆盖四类调用方：Linux 程序（musl 和 glibc）和 pkg、HiSH 自带的 shell 与内置 Toybox、系统 Toybox 命令。共享逻辑在 `hish_links_impl.h`，各库只加自己的公开函数。

| 调用方 | 实现 | 编进 |
| --- | --- | --- |
| musl Linux 程序 | `linux/links.c`、`hish_links_libc.h` | `libhish_linux_identity_padded.so`（预加载） |
| glibc Linux 程序 | 同上，glibc 变体 | `libhish_linux_glibc_tty.so`（预加载） |
| HiSH 自己的 shell、内置 Toybox、pkg、装载器 | `links_ohos.c`，符号 hidden | `libhish_shell.so` |
| 系统 Toybox 命令 | `links_ohos.c`，符号导出（`HISH_LINKS_EXPORT`） | `libhish_exec_bridge.so`（预加载） |

## 存储格式

链接是一个普通小文件：模式 `01600`（sticky 位加属主读写），内容为 `!<hish-symlink>` 加目标路径，不含 NUL，目标 1 到 4095 字节。sticky 位是标志，已在这台手机的应用存储上实测：创建、`chmod`、`cp`、`mv`、追加、重写之后都保留。格式与解析、路径解析算法在 `hish_link_core.h`，不含系统调用，主机测试直接运行它（`tests/symlink-emulation.cjs`）。

不认识标记的程序把它当成一个小文本文件；`cp` 连标记一起复制成新链接。

## 共同的行为规则

- `symlink` 先让内核试一次，被拒（EACCES、EPERM、EOPNOTSUPP、ENOSYS）才写标记；写失败会删掉半成品。目标已存在返回 EEXIST。
- `lstat` 把标记报告成链接：`S_IFLNK|0777`，大小为目标长度。`stat` 跟随到目标。`readlink` 返回目标。
- 目录链接作为路径中间一段时，内核对标记文件返回 ENOTDIR，此时才走慢路径：逐段 `lstat`、替换标记、重新交给内核。`..` 按目标目录向上，不按链接所在目录。链接最多跟随 40 次，之后 ELOOP。
- 跟随最后一段的操作先看一眼最后一段（一次额外系统调用）；`stat` 族直接用自己的结果，`open` 在成功后用 `fstat` 看，带 `O_TRUNC` 的先查，免得截断掉标记本身。`O_NOFOLLOW` 遇到标记返回 ELOOP，`O_PATH|O_NOFOLLOW` 返回链接本身的描述符。
- `chmod` 等经链接作用于目标，不会改动标记；`fchmodat(AT_SYMLINK_NOFOLLOW)` 对链接返回 EOPNOTSUPP，与 Linux 一致。
- `realpath` 先替换标记，再让内核报出规范路径（用 `O_PATH` 打开，读 `/proc/self/fd/N`），不依赖 `dlsym`。
- 本层自己的系统调用一律用内联 `svc`，从不调用被替换的 `syscall()`。
- `HISH_LINKS=0` 关闭整个模拟（紧急开关）；`HISH_LINKS_TRACE=1` 把拦截到的部分调用写到标准错误（Linux 程序一侧）。

## Linux 程序（musl 与 glibc）

`linux/links.c` 与 `terminal.c`、`exec.c` 同级，编进已经预加载到每个 Linux 程序的库。它替换约 40 个取路径的 libc 函数：`stat` 族（含 `statx`）、`access` 族、`symlink(at)`、`unlink(at)`、`rmdir`、`mkdir(at)`、`rename(at)(2)`、`chmod`、`chown`、`utimensat`、`truncate`、`chdir`、`creat`、`fopen`、`opendir`、`realpath`，以及 `syscall()` 本身。`open`、`openat`、`readlink`、`readlinkat` 仍在 `terminal.c`，它们经 `syscall()` 进入同一段代码；`exec.c` 的 `execve` 在读文件前先跟随链接。

glibc 变体另有版本化的 `__xstat`、`__lxstat`、`__fxstatat`（及 `64` 版，glibc 2.33 之前走这些入口，更新版本直接调用 `stat`，两种都覆盖）和 `_FORTIFY_SOURCE` 编译的程序用的 `__open_2`、`__openat_2`（及 `64` 版）。当前随包 glibc 为 Ubuntu 2.39，见 [组件来源](third-party.md)。

### C 库内部互相调用的函数

`hish_links_libc.h` 由 `links.c` 包含，处理那些在 libc 里自己打开目录和文件、预加载够不着的函数：

- `scandir`（musl 与 glibc）、`glob`/`globfree`、`nftw`、`ftw`（仅 musl）：改编自 musl 1.2.5 的 `scandir.c`、`glob.c`、`nftw.c`、`ftw.c`，文件系统调用换成本层的函数。`glob` 另外会进入作为目录链接的小文件（`*/x.c` 这样的模式穿过链接）。glibc 保留自己的 `glob`、`nftw`、`ftw`，因为它们的标志位与 musl 不同。
- `freopen`（musl 与 glibc）：先跟随路径里的链接，再把真实路径交给 libc 的 `freopen`。
- `HISH_LINKS=0` 时这些函数都改回 C 库自己的版本。musl 许可全文在该头文件开头。

## HiSH 自己的 shell 和内置 Toybox

`links_ohos.c` 编进 `libhish_shell.so`（mksh、内置 Toybox 小程序、`pkg` 和装载器都在里面），用真实的 libc 名字（`stat`、`open`、`symlink`、`readlink`、`chdir`、`opendir`、`realpath`、`syscall` 等 30 多个）定义同一套行为，可见性为 hidden。链接器因此把这个库内部所有对这些名字的调用绑到本文件，库外（libc++、系统库）仍直接走 libc，也没有新增导出符号。实际效果：

- `ln -s`、`ls -l`（显示箭头）、`readlink`、`cat`、`rm`、`mv` 对链接生效；`test -L`、`-h`、`-e`、`-d`、`-f` 的结果与真实链接一致。`cd` 进目录链接后 `pwd` 显示链接路径，`pwd -P` 显示目标目录。`realpath` 是 mksh 内建命令，同样生效，且只接受一个参数。
- 在 shell 里直接执行链接：当前目录、经目录链接、绝对目录链接、经 PATH 都可以，`node_modules/.bin/opencode` 可以直接敲。`HiSHExecFile` 本身没有改，它内部的 `open`、`access` 已经跟随链接。
- 硬链接仍是 EACCES，`ln` 不带 `-s` 时报 Permission denied。
- `syscall()` 也被接管，库内的 TTY 层 `open` 因此跟随链接。内核 exec 的尝试（`KernelExecve`）不在被接管的号码里。
- x86_64 模拟器 ABI 同时构建，该文件在非 AArch64 下为空。

## 系统 Toybox 命令

系统 `/system/bin/toybox` 的命令（`stat`、`find`、`cp`、`mv`、`touch`、`du`、`file`、`tar`、`dd`、`md5sum` 等）用的是系统 libc，看不到链接。办法与 `execvp` 钩子相同（见 [执行兼容层](elf-runtime.md)）：`shell_module.c` 让每个系统小程序都带着 `LD_PRELOAD=libhish_exec_bridge.so` 启动。该库里有 `links_ohos.c` 的第二份编译（CMake 的 `hish_links_export` OBJECT 库，定义 `HISH_LINKS_EXPORT`），符号导出，所以小程序的 PLT 调用落到这里。

- 这份拷贝默认关闭（`HISH_LINKS_START_DISABLED`），没有构造函数能打开它；只有库的构造函数确认进程带着系统小程序标记时才调用 `HishLinksEnableForApplet()`，并且调用者环境里的 `HISH_LINKS=0` 仍然生效。同一个库也被内核 exec 桥预加载进 Linux 程序的引导进程，那里它只是透传。
- 导出 31 个路径函数，与 hidden 版相同：`stat`、`lstat`、`fstatat`、`access`、`faccessat`、`open`、`openat`、`readlink(at)`、`symlink(at)`、`unlink(at)`、`rmdir`、`mkdir(at)`、`rename(at)`、`chmod`、`fchmodat`、`chown`、`lchown`、`fchownat`、`utimensat`、`truncate`、`chdir`、`creat`、`fopen`、`opendir`、`realpath`、`syscall`，另有 `execvp`、`execve`、`vfork` 和开关函数 `HishLinksEnableForApplet`。系统 Toybox 导入的路径函数里，`mknod(at)`、`mkstemp`、`mkdtemp`、`statfs`、`statvfs`、`link(at)`、扩展属性函数和 `dlopen` 不在其中。
- 代价：每个系统小程序启动约多 20 毫秒（10 次 `date` 从 0.94 秒到 1.15 秒，单次测量）。

## 不覆盖的

- 直接内联 `svc` 的程序看到的仍是小文件。Bun 1.4.4 就是这样：读 `.text` 得到 705 处 `svc #0`，其中 `openat` 143 处、`newfstatat` 169 处、`fstat` 154 处、`faccessat` 14 处、`read`/`write` 等；而 `symlinkat`、`readlinkat`、`unlinkat`、`mkdirat`、`renameat`、`fchmodat`、`utimensat`、`chdir`、`execve` 走 libc。所以 Bun 的 `symlink`、`readlink`、`lstat`、`stat`、`exists`、`access`、`chmod`、`utimes`、`mkdir`、`unlink`、`rename`、`rm`、`chdir`、`spawn` 看得到链接，而它自己的 `readFile`、`writeFile`、`copyFile`、`readdir`、`realpath`、`Bun.file` 把标记当小文件。
- seccomp 用户通知在这台手机上不可用（见 [加载实验](history/loader-experiments.md)），内联系统调用目前没有别的拦截办法。
- 仍留在 C 库内部的调用：动态链接器装载库（含 `dlopen`），所以动态库的链接仍由 pkg 复制成文件。musl 没有 `fts`，程序自带的 `fts` 调用的是已覆盖的函数；`popen` 和 `system` 由 mksh 解释命令，同样已覆盖。glibc 自己的 `fts`、`glob`、`nftw`、`ftw` 不在本层。
- `readdir` 把标记报告成普通文件（`d_type` 为 `DT_REG`），`lstat` 才报告链接。
- 硬链接（`link`、`linkat`）仍是 EACCES，调用方的退路照常。
- 系统 Toybox 用到但本层没有覆盖的路径函数（`mknod(at)`、`mkstemp`、`mkdtemp`、`statfs`、`statvfs`、`link(at)`、扩展属性函数、`dlopen`）穿过目录链接时不工作。
- 没有 glibc 的真实应用能跑：随包 glibc 运行库不含 `libpthread.so.0`，Bun 编译出的 glibc 版 opencode 报找不到它。glibc 变体只用 `tests/fixtures/linux_links.c` 的 glibc 构建验证过。

## pkg

设备拒绝符号链接时 pkg 用副本代替文件链接，目录链接则无法复制。现在目录链接写成标记（`WriteLinkMarker`），记录为 `L:相对目标`，换代和卸载按这个记录处理，不参与哈希校验。

指向较大可执行文件的链接也写成标记：目标在本事务内、归档里带执行位、至少 64 KiB，且链接名和目标名都不是库（名字含 `.so.` 或以 `.so` 结尾）。git 的 148 个辅助程序链接指向 3 MB 的 `git`，原先每个存成副本，代次目录 584 MB；现在是标记，136 MB。库文件链接、数据文件链接和小程序链接仍是副本：`ld.so` 装载库时看不到标记，数据文件可能被不经 libc 的代码读取。

## 设备检查程序

`tests/fixtures/linux_links.c` 是自检程序（47 项，glibc 构建 32 项，另有不计数的信息行）；`scripts/linux/link-check-build.cjs` 用已有 SDK 和 musl、glibc 目录把它编成 `linux_links_musl`、`linux_links_glibc`，加 `--reference` 还会编出把 `glob`、`nftw`、`ftw`、`scandir` 换成未改动的 musl 源码的 `linux_links_musl_orig`。用法见 [测试](testing.md)。`--ab DIR` 模式在不含链接的目录树上打印这四个函数的结果，用来逐字节比较两个版本。
