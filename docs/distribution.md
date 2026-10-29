# 软件交付边界

## 默认 HAP 的责任

| 随包内容 | 原因 |
| --- | --- |
| ArkUI/ArkWeb、xterm、字体和许可 | 终端界面及输入输出 |
| OpenHarmony mksh | 默认 shell、脚本和 Linux 调用方的 system/popen |
| 内置 Toybox 基础工具 | 为终端提供已适配的基本文件/文本操作 |
| 原生会话与 exec 桥 | 创建应用进程、传输字节、执行真实 ELF |
| 六个 ARM64 运行库 | 提供跨程序通用的 musl/glibc 装载与执行兼容 |

六个运行库为：`libhish_linux_musl_loader.so`、`libhish_linux_identity_padded.so`、`libhish_linux_glibc_loader.so`、`libhish_linux_glibc_compat.so`、`libhish_linux_glibc_tty.so`、`libgcc_s1.so`。最后一个支持 glibc 取消和栈展开，不是 GCC 编译器。

连同两 ABI 的四个原生基础库，当前规则共允许 14 个库、16 个 rawfile。唯一机器可读清单为 `scripts/daily-package-policy.json`。

## 普通软件的责任

Git、Git 内部 helpers、curl、GNU printf/uname、TinyCC 及其专属依赖均不属于默认 HAP。编译器头文件、CRT、静态库、CA 安装器和应用专属环境初始化也不再由终端启动流程提供。

将来的软件包应自行携带或声明依赖、内部子程序、证书数据和更新元信息。Git 的内部程序放在软件包自己的标准布局中；终端不再把它们逐个登记为内置命令，不把 `GIT_EXEC_PATH` 强制设为公共 bin。

标准应用不应为了加入发行包而改名为 `libhish_linux_APP.so`、修改其依赖名，再手工加入工具表。通用兼容层中的平台限制与某个应用的适配必须区分；现有兼容层仍不保证任意上游包可直接安装运行。

## 本阶段不做的事

- 不实现包管理器或软件仓库，不承诺普通应用现在可更新。
- 不删除备用源码、旧 payload、手机 HOME 或已有编译器/Git 文件。
- 不用空壳或隐藏工具列表代替移出实际交付。
- 不全局封禁 `git`、`curl`、`tcc` 等名字。升级脚本的处理见 [Shell](shell.md)。

当前没有随包包管理器，也没有默认 C 编译器或网络下载应用。这是尚未完成下一阶段的状态，不将旧包的工具能力写给新包。
