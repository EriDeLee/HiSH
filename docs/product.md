# 产品目标

HiSH 是基于 [harmoninux/HiSH](https://github.com/harmoninux/HiSH) 的 HarmonyOS 原生终端实验。应用自有进程执行真实 ARM64 ELF；HDC 只用于部署、测试和取证，不提供运行时后端。

保留 xterm.js、双排虚拟键、复制粘贴、字体和背景设置。默认 shell 为 OpenHarmony mksh，少量基础工具由内置 Toybox 或系统可执行工具提供。不恢复 QEMU、PRoot、容器、虚拟机、VNC 或远程终端服务。

## 软件更新方向

发行包提供终端底座与必要的通用兼容层；普通软件独立安装和更新，不因升级 Git、curl 或编译器而重发 HAP。TinyCC 已明确退出默认包，其适配源码保留。

包管理器是下一阶段任务。本阶段不选择或实现包管理器、软件源、依赖解析、自动安装或更新流程，也不提供假占位命令。具体责任见 [软件交付](distribution.md)。

## 验证边界

现有证据只覆盖已测设备、调试固件及安装沿革。完整 Linux 用户空间、任意 Linux ELF、完整内核 PTY、全新安装、发布域、多机、长期压力、emoji 字素和完整 Bash 兼容均未证明。已安装包与新源码分开记录在 [验收](verification.md)。

## 项目归属

本分支起于 `EriDeLee/HiSH` 的 `personal`，包含上游提交 `dbd6e72667151a6b190d2ca5572f2c6ff99136d9`。保留 HiSH 来源链接、MIT 版权及 [根目录许可](../LICENSE)。组件来源另见 [组件清单](third-party.md)。
