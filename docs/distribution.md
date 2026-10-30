# 软件交付边界

## 默认 HAP 的责任

| 随包内容 | 原因 |
| --- | --- |
| ArkUI/ArkWeb、xterm、字体和许可 | 终端界面及输入输出 |
| OpenHarmony mksh | 默认 shell、脚本和 Linux 调用方的 system/popen |
| 内置 Toybox 基础工具 | 为终端提供已适配的基本文件/文本操作 |
| 原生会话与 exec 桥 | 创建应用进程、传输字节、执行真实 ELF |
| 六个 ARM64 运行库 | 提供跨程序通用的 musl/glibc 装载与执行兼容 |
| pkg 与 APK 自举数据 | 提供官方仓库依赖解析、下载验签、私有安装和功能检查 |

六个运行库为：`libhish_linux_musl_loader.so`、`libhish_linux_identity_padded.so`、`libhish_linux_glibc_loader.so`、`libhish_linux_glibc_compat.so`、`libhish_linux_glibc_tty.so`、`libgcc_s1.so`。最后一个支持 glibc 取消和栈展开，不是 GCC 编译器。

连同两 ABI 的四个原生基础库，当前规则共允许 14 个库、17 个 rawfile。APK 自举压缩数据嵌入 ARM64 shell 库；完整许可增加一个 rawfile。唯一机器可读清单为 `scripts/daily-package-policy.json`。

## 普通软件的责任

Git、Git 内部 helpers、curl、GNU printf/uname、TinyCC 及其专属依赖均不属于默认 HAP。编译器头文件、CRT、静态库、CA 安装器和应用专属环境初始化也不再由终端启动流程提供。

软件包应自行携带或声明依赖、内部子程序、证书数据和更新元信息。Git 的内部程序应放在软件包自己的标准布局中；终端不把它们逐个登记为内置命令，不把 `GIT_EXEC_PATH` 强制设为公共 bin。

标准应用不应为了加入发行包而改名为 `libhish_linux_APP.so`、修改其依赖名，再手工加入工具表。通用兼容层中的平台限制与某个应用的适配必须区分；现有兼容层仍不保证任意上游包可直接安装运行。

## 当前责任边界

- 不自建分发仓库；pkg 接入 Alpine 官方 main+community/aarch64，无包名/依赖白名单，功能检查不作为安装资格。Bun 走官方 npm；PRoot 是显式的平台源码端口。
- 不删除备用源码、旧 payload、手机 HOME 或已有编译器/Git 文件。
- 不用空壳或隐藏工具列表代替移出实际交付。
- 不全局封禁 `git`、`curl`、`tcc` 等名字。升级脚本的处理见 [Shell](shell.md)。

当前随包有 pkg 及必要自举依赖、按需安装的 PRoot 端口产物和功能探针。没有默认 C 编译器或作为用户应用的 curl/Git，Bun/jq/zstd 由手机联网安装。PRoot 不作为默认会话后端，不内置 rootfs。详细方案见 [包管理器](packages.md)。
