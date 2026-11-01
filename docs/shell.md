# mksh 与命令入口

默认交互、`sh`、`mksh`、受支持脚本及 Linux 调用方的 system/popen 使用应用 mksh。`mksh_adapter.c` 同步真实导出环境，设置 SHELL、UTF-8 默认语言和私有历史文件，保留正常 `.mkshrc` 行为。

mksh 以 OBJECT 编入 `libhish_shell.so`，固定 51 份上游文件并逐份校验摘要，不运行上游构建脚本。新 shell/app 命令进入干净的 OHOS 引导进程，不在同一进程中重复进入 mksh main 复用解析/作业/历史全局状态。

## 基础工具

内置 Toybox 仅提供已有基础工具子集，不含 Toybox shell。构建目录生成 printf/sleep/test 命令表覆盖，不改下载源码。系统工具在命令子进程中执行 Toybox `--long` 探测；列出名字不代表其权限或功能通过。

内嵌 Toybox 里会启动其他程序的只有 `xargs`，它经 `xexec()` 启动。构建把该文件里的 `execvp` 改名为 `toybox_adapter.c` 的 `HiSHToyboxExecvp`：按 PATH 顺序查找（规则同 execvp，实现在 `toybox_exec_path.h`），再交给 mksh 用的同一个装载入口 `HiSHExecFile`，所以 `xargs` 能启动随包 Linux 程序；此前直接调用系统 execvp，报 `Permission denied`。`tests/toybox-exec.cjs` 确认 Toybox 源码里只有 xexec 一处启动程序，并在主机上运行查找函数。

`env`、`timeout`、`nice`、`nohup`、`setsid`、`taskset`、`time` 和 `find` 不在内嵌子集里，由系统 `/system/bin/toybox` 提供。它们的 `execvp`/`execve` 来自系统 libc，单靠它们启动不了随包 Linux 程序。`HiSHRunApplet` 让每个系统小程序都带着 HiSH 的预加载库启动系统 Toybox，由该库的 `execvp`、`execve`、`vfork` 进入 HiSH 装载器，机制见 [ELF 兼容层](elf-runtime.md)。预加载使每次系统小程序启动多约 20 毫秒。mksh 自带关键字 `time`。

符号链接：内置 Toybox 的 `ln -s`、`ls -l`、`readlink`、`cat`、`rm`，mksh 的 `cd`、`pwd -P`、`test -L`、`realpath`、命令查找与直接执行，以及全部系统 Toybox 命令（`stat`、`find`、`cp`、`mv`、`touch`、`du`、`file`、`tar` 等）对模拟链接生效。机制和范围见 [符号链接](symlinks.md)。

`hish-tools` 说明底座范围，不是普通软件仓库，也不再罗列 Git 内部 helpers。查询已安装命令使用 `command -v NAME` 或 `whence NAME`；软件交付归属见 [交付边界](distribution.md)。

## 旧转发脚本

以前的 HAP 在 `$PREFIX/bin` 生成过以下形式的文件：

```sh
#!/bin/sh
exec git "$@"
```

移出随包 Git 后，执行这个文件会再次找到自己。OHOS 与 Linux 两条执行路径只对私有公共 bin 下、名字及全文完全匹配的旧应用转发脚本返回 ENOENT，不删除文件。

不按名字封禁 Git/TinyCC，不拒绝用户内容不同的同名脚本或真实 ELF。未来软件包替换旧文件后，应走普通路径查找与 ELF/脚本执行，而不是恢复写死的应用工具表。

旧 BusyBox/ash/dash/mrsh、试验 shell 和诊断命令的禁用与此不同：它们不是本阶段可恢复的默认 shell。源码仍备用。

## 已知交互行为

mksh 长行使用水平滚动，行首 `<` 可能表示隐藏内容。裸 `sleep` 是 mksh 内建，不适合证明外部作业暂停；作业测试使用明确外部路径。停止状态可能显示 `Signal 19`，以 jobs 保留和 fg 实际恢复确认，不改写观察结果。
