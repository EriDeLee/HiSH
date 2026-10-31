# 验收状态

更新：2026-10-31。包内容、源码状态和手机状态分别记录。

## 当前：代次化包管理器重构（2026-10-31）

- 包管理器改为不可变代次：`native/packages/generations/gNNNNNN`，`active` 指针原子切换；会话固定自己的代次与 PATH，运行中的程序不受后续安装影响。
- 真机完整生命周期通过（`REFACTOR_LIFECYCLE_EXIT=0`，`REFACTOR_LIFECYCLE_PASS`）：旧布局导入、`pkg doctor`/`info`、`pkg upgrade` 保留应用自带包、离线 `remove proot` 后重装字节一致、`talloc` 守卫、`bash` 安装脚本记录为未运行并标记 `unconfigured`、`bash`/`tree` 实跑、代次保留与回收、`nginx` 目录符号链接拒绝且不激活、并发锁、registry 更新查询、不存在的包与非法路径拒绝、`pkg doctor --functional --capabilities` 中 proot 与 bun 功能探针通过。
- 修复一个数据丢失级缺陷：仓库失败时旧实现会发布一个更小的代次并返回成功，导致已安装的 Alpine 包消失。现在请求的包既未下载也未安装时整个事务失败，不再发布；主机新增回归测试。真机复测：不存在的包返回非零，已装包一个不少。
- 真机遇到设备侧匿名可执行内存拒绝（`mprotect` 返回 `EINVAL`），与本次重构无关：重构前 HAP 同样失败；授予 `ALLOW_WRITABLE_CODE_MEMORY` 等 ACL 后仍失败；重启无效；下午设备自行恢复后同一份代码全部通过。签名文件页仍可执行，应用目录文件、memfd、共享别名均返回 `EACCES`。
- 最终包为 `build-6`，签名 HAP 11,511,584 字节，SHA256 `897961d1ac7fabf2d3180a9d07b3d91d5e8aec92afe73b70dbd76d743e22c7c4`。证据在 `hish-pkg-refactor/build-6`、`device-14`。
- 主机 103 项 node:test 通过（含新增的失败事务回归测试），`git diff --check` 通过。

未由本轮证明：全新安装、覆盖安装后的 HAP 升级迁移、其他设备或固件、发布域签名、长期运行压力。

## 前轮验证：包管理与 Bun / PRoot（历史）

工作证据位于 `C:/Users/Lee/Documents/Codex/2026-10-30/go/work/`。原版 APK 3.0.8 负责官方 Alpine v3.23 main/aarch64 的依赖下载和验签，应用安装器激活前运行真实功能检查。

- 真机 jq/zstd 安装、JSON中文运算、1MiB压缩还原、升级、移除及重装通过；不在目录内的包与网络失败均保留原安装。篡改签名控制区被拒绝，空公钥目录报 UNTRUSTED。
- Bun 1.4.4 官方 ARM64 musl 二进制通过脚本、文件、Worker、自启动子进程、HTTPS及 npm 依赖安装/导入；见 [包管理](packages.md) 的精确选项和限制。仍未接入 `pkg install bun`。
- PRoot 5.4.0-r2 APK 验签/版本成功，rootfs 实际执行失败。关闭 seccomp 加速后的 trace 确认临时 loader 内核 execve 返回 EACCES13；版本输出不算功能通过。静态 rootfs BusyBox 另有 SIGSYS，原因未进一步确定。
- 通用修复：普通 ELF 文件上限128MiB；相对路径镜像解析为绝对路径，合作式 self-exec及 Bun 子进程通过。未修改上游程序字节或系统权限。
- 主机68项 node:test、27项会话及3项编码检查通过。构建、签名、安装与最终真机证据另存，不能由此推断全仓库或发布域兼容。
- 最终为 work/pkg-build-7、pkg-device-5；已覆盖安装并 force-stop/start 后复测。签名 HAP 11,128,295字节，SHA256 `cdcbd160f55f83dbbaea32ce67396c76e0438d8554c84dd9d25836ae2c39d02b`。原15份用户测试文件安装前后摘要一致，已安装 jq/zstd及 world/版本记录跨覆盖安装、重启、复测摘要一致。默认 bun install、TypeScript、bun run 也通过；原始日志见 work/compat-1，交付副本见同任务 outputs/hish-package-verification。

## 前一手机包：最小底座 device-1（历史）

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

## 早期最小底座构建（历史）

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

## 早期最小底座的未验证项

全新安装、当前包重启、其他设备/固件、发布域、长期后台/高输出压力、完整 PTY、隐藏 libc/静态直接调用、任意 ELF、完整无障碍和 emoji 字素均没有本轮证明。构建已有资源重复、ArkTS 异常处理、SDK 和上游工具警告，不以屏蔽规则宣称零警告。
