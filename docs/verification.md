# 验收状态

更新：2026-10-30。包内容、源码状态和手机状态分别记录。

## 当前手机：最小底座 device-1

- 已签名并覆盖安装：`C:/Users/Lee/opencode/tmp/opencode/hish-base-only-1/device-1/phone-base-signed.hap`。
- 7,935,004 字节；SHA256 `c3401c2bf23d6b176b786d7a215f10fe6d67c13d0449dfb58410a5c94ecc7195`。
- 真机脚本退出0：mksh语言、中文/空格文件、管道/重定向、环境、脚本、exec PID及十轮短管道通过。
- 五组可见交互通过：命令/文件Tab补全、ASCII/中文删除和插字、exec后历史、Ctrl+C、外部作业暂停/jobs/fg。关键截图已读取核对；最终留在普通终端提示符。
- hish-tools实屏不再列Git/helpers、curl、TinyCC；五个旧应用转发脚本非0拒绝，内容不同的同名用户脚本仍可执行。该同名脚本测试不代表运行了真正Git或编译器。
- 原有15份原版程序/依赖安装前、安装后及测试后摘要匹配；未清数据、迁移HOME、删除用户文件或重置偏好。
- 最终实际授权仅 `ohos.permission.INTERNET`（状态0）；息屏值前后均为 `Timeout=30000ms  OverrideTimeout=104430000ms`，本轮未修改。最终应用进程树快照无Z状态，不代表长期压力通过。
- 最终主机回归：62项 node:test、27项会话、3项编码全部通过，`git diff --check`通过；原暂存仍仅 `libvncserver` 删除。
- 证据：同目录 `installation.json`、`signed-package.json`、`functional/proof.json`、五组交互报告和 `final-proof/proof.json`；最终权限、电源、进程及主机日志保留在 `final-proof/`。未开发包管理器，未提交。

签名工具只新增 `.pages.info`；46个原公共条目内容逐字节一致，未读取该新增签名条目的内容。未签名包规则仍拒绝它。首次签名后检查因此中止在安装前，补独立签名前后比较与正反例后，复用了同一签名包继续安装，没有重新构建或放宽普通组件清单。

## 上一个手机包：build-15（历史）

- 包：`C:/Users/Lee/opencode/tmp/opencode/hish-mksh-native-1/build-15/phone-mksh-signed.hap`。
- SHA256：`766e60a18050ba9f6cec911e6e32fdf4e6cfa7782e7e60a554d935ae0915419d`，32,483,370 字节。
- 证据：同目录 `final-proof/proof.json`、`adoption.out`、`processes.log`、交互报告及 Vim 文件读回。
- 已测：mksh 语言/脚本/管道/导出环境/exec PID，六组可见输入与作业交互，十轮收养退出，Git、TinyCC、jq、zstd、Vim 和证书验证 HTTPS。Vim 精确保存 `MKSHVIM15\n`；zstd 还原 2MiB。
- 原版 15 份下载程序/依赖摘要不变，实际授权仅 INTERNET。末次快照无 Z，不是长期压力或任意软件兼容证明。

该旧包已被上述最小底座覆盖；旧包及证据仍保留。它包含普通应用，其应用测试不能移名为底座包通过。

## 本轮：最小底座

源码已移除默认 Git/curl/GNU/TinyCC 路由、应用依赖和编译器数据；不再执行专属环境/证书安装。只保留六个通用运行库与终端底座。旧原文已按主题重组，项目自有说明不再保留通用 README/CLAUDE 文件。

底座候选已完成两 ABI 构建及严格 HAP 检查：

- 包：`C:/Users/Lee/opencode/tmp/opencode/hish-base-only-1/build-2/phone-daily-unsigned.hap`。
- 7,734,083 字节；SHA256 `51f09401af821087e6d29a2458db07837106c697f7c0a46359d275e06b615f1d`。
- 14 个原生库、16 个 rawfile、18 个 ArkTS 来源；六个通用运行库可执行段与选定代次匹配。实际包不含普通应用、工具链数据或应用专属初始化器。
- 主机59项 node:test、27项会话、3项编码通过。包含旧转发脚本正反例、退役生成库摘要归档、文档归属/禁用文件名/链接及 `.hallmark` 忽略验证。
- 证据：同目录 `package-verification.json`、构建日志与 `final-proof/`。这是构建阶段的59项结果；后续签名/覆盖安装和真机结果见首节，签名比较新增3项测试。源码未提交，原暂存仍仅 `libvncserver` 删除。

build-1 因交付规则依赖以绝对和含相对段的路径重复登记，导致 Ninja 生成重复输出；没有清缓存。`configure_inputs.cmake` 统一路径并去重，真实 CMake 回归与 build-2 通过，失败日志保留。

## 旧主机清理包

上一轮未签名包及检查记录见 [主机清理历史](history/host-cleanup.md)。它仍带普通应用，不是本轮最小底座。

## 未验证

全新安装、当前包重启、其他设备/固件、发布域、长期后台/高输出压力、完整 PTY、隐藏 libc/静态直接调用、任意 ELF、完整无障碍和 emoji 字素均没有本轮证明。构建已有资源重复、ArkTS 异常处理、SDK 和上游工具警告，不以屏蔽规则宣称零警告。
