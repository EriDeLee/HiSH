# 内置包管理器

## 来源

`pkg` 有三类来源，统一由 `native/packages/bootstrap-摘要/etc/hish/packages.json` 描述。该清单由构建从实际载荷生成，其 SHA256 编进 `libhish_shell.so`：

- **Alpine 仓库：** v3.23 官方 `main`、`community` 的 aarch64 仓库，复用未修改的 APK/libapk 3.0.8 下载并验签。没有包名或依赖白名单。
- **应用管理的本地包：** 随 HAP 附送，当前为 PRoot 平台端口和 Alpine 原版 talloc 2.4.3-r0。可离线安装；`pkg` 不能把它们更新或替换成远程版本，只随应用更新变化。
- **远程注册表来源：** 当前只有 Bun，取自 npm 官方 `@oven/bun-linux-aarch64-musl`。版本跟随注册表 latest，tarball 必须位于配置的注册表下，并按元数据中的 SHA-512 校验。该摘要与载荷来自同一份未独立签名的元数据，只证明二者一致。
- **Bun 附带命令：** `usr/bin/bunx` 运行 `bun x`；`bin/node` 以 `node` 为程序名启动 Bun，进入 Bun 自带的 Node 兼容模式，供 npm 安装脚本里的 `node 脚本` 使用。Bun 自己的做法是在 `/tmp` 建符号链接，本设备既无 `/tmp` 也拒绝符号链接。Bun 1.4.4 的 Node 模式不处理 `node --version` 与 `node -v`，应以 `node 脚本`、`node -e` 为准。`bin/node` 在 PATH 中排在 `usr/bin` 之后，装了真正的 Node.js 时后者优先，两者不争同一路径。附带文件只在 Bun 安装时写入；版本未变的已装 Bun 会原样保留，需 `pkg remove bun` 后重装才有新增的附带文件。

核心代码不按包名分支。本地包、远程来源和功能探针都是清单里的数据，源文件分别为 `scripts/packages/prepare.cjs`、`sources.json` 和 `checks.json`。

## 使用

```sh
pkg search tree
pkg update
pkg info bun proot talloc
pkg install tree jq bun proot
pkg list
pkg upgrade
pkg remove tree
pkg doctor
pkg doctor --functional --capabilities
```

- `pkg list` 显示版本、来源（app-managed / alpine / npm）、配置状态，以及是否只作为依赖安装、链接是否存为副本。
- `pkg doctor` 只报告状态：当前代次、清单是否已接受、逐文件改动或缺失、未运行的安装脚本。`--functional` 才执行清单中的功能探针；`--capabilities` 才运行符号链接、硬链接、ptrace 和 seccomp 用户通知探针。
- 安装成功与功能验证分开报告；没有探针的包显示为未验证，不阻止安装。
- opencode 的 grep 和 glob 工具需要 `rg`。它先找 PATH 里的 `rg` 和 `~/.local/share/opencode/bin/rg`，都没有就从 GitHub 下载 glibc 版；本机网络会替换 GitHub 的证书，下载失败，下载到的 glibc 版也缺 `libpthread.so.0` 跑不起来，结果是 `ripgrep execution failed`。先 `pkg install ripgrep`（Alpine 的 `ripgrep-15.1.0-r0`，与 opencode 要的版本相同），之后这两个工具正常。验证见 [验收](verification.md)。

## 应用管理的包

- 同名的仓库包永远不会被解包；如有仓库包把它们当依赖，改用本地版本，并检查版本约束，不满足就失败并给出依赖链。与本地包路径相同的仓库文件让事务失败。
- `upgrade`、`update`、安装或移除其他包都保留已安装副本的字节和记录摘要，并提示"kept app-managed"。
- 新 HAP 带来不同版本时，由终端会话启动时的应用迁移器处理：只替换已安装的本地包，不新增原本未安装的包，并且不联网。迁移完成前，`pkg` 拒绝所有会修改包的命令。迁移失败的原因记在 `state/migration-error`；`pkg doctor` 只在迁移仍待处理时显示它，会话启动发现已无待迁移项时删除旧记录（此前提早返回的路径漏删，导致旧版本留下的错误一直显示）。
- 新版应用不再提供某个本地包时，已安装副本保留，名字仍不允许远程包替换。

## 安装语义

- 软件位于不可变代次 `native/packages/generations/gNNNNNN`，活动代次记录在普通文件 `active` 中，用原子重命名切换，不依赖符号链接。
- 每个会话在启动时固定一个代次，写入 `HISH_PACKAGE_ROOT` 和 PATH。会话启动的程序一直使用该代次。终端里的 `pkg` 是 `$ENV` 加载的 shell 函数，事务完成后只把当前窗口切到新代次。
- 清理旧代次时保留活动代次和最近一个完整代次，以及任何进程的 exec 环境里仍引用的代次。`/proc` 读不了时也保留。
- 崩溃恢复会丢弃未完成的 `.tmp-*` 事务；活动记录无效时，回到最新的完整代次。恢复不联网。
- 事务失败不会发布代次：如果请求的包既没有下载到、也不在当前代次里，整个事务中止，已安装的包保持原样。仓库下载为空或只完成一部分时不会生成更小的代次。
- 不执行 APK 安装脚本和触发器。带此类脚本的包会照常安装，但标为 `unconfigured`，脚本原文存放在代次的 `.hish-package/scripts/包名/`。原因见 [兼容方案](compatibility-package-refactor.md)。
- 链接按设备能力处理。能创建符号链接时保留真实链接；不能时，目录链接和指向较大可执行文件（至少 64 KiB、带执行位、不是库）的链接写成模拟链接的标记文件（格式和覆盖范围见 [符号链接](symlinks.md)），记录为 `L:目标`，换代时重建，卸载时随包删除；其余文件链接存为副本并计数显示。库文件链接不用标记，因为动态链接器装载库时看不到标记。此前目录链接会让事务失败，`pkg install git curl` 因 `etc/ssl1.1/certs` 被挡住；git 的 148 个辅助程序链接原先各复制一份 3 MB 的程序。
- 用户改过的包文件不会被静默覆盖：保留用户的版本，新版本另存为 `路径.hish-new`。
- 归档检查覆盖 gzip/tar 完整性、路径逃逸、重复条目、设备文件、链接循环与逃逸，以及压缩 64MiB / 解压 128MiB 上限。

## 旧布局导入

首次运行会只读导入旧版 `native/packages/root`：记录快照写入 `state/legacy-snapshot`，文件复制到新代次，旧目录保持不变。本地包逐文件与 HAP 载荷比对，一致才认定为 app-managed，否则标为 `legacy-unknown`，需先 `pkg remove` 再安装。Bun 按原记录的 SHA-512 归入 npm 来源。仓库包在第一次联网事务后才有逐文件记录，在此之前不能离线重用。

## PRoot 适配

原版 PRoot 的临时 loader 内核 execve 在真机返回 EACCES13。端口保留 PRoot 的路径与系统调用跟踪，改用允许执行的系统 Toybox 和 HiSH 引导库：装载脚本放在保留的环境区，由平台适配器映射真实目标段，沿用 RW、复制、RX 路线，拒绝 W+X。seccomp 加速关闭，仍使用 ptrace。它不授予内核 root，也不是安全隔离边界。

已测范围是 ARM64 musl rootfs 的文件、shell 和 fork/exec。glibc rootfs、任意静态程序和完整发行版生命周期未证明。

## 验证

`node --test tests/package-core.cjs` 在主机上用假仓库驱动真实的事务代码，覆盖锁定、冲突、迁移、导入、崩溃点、旧代次清理、脚本与链接记录和远程来源校验。`tests/package-manager.cjs` 覆盖真实解包器与自举数据。真机结果见 [验收](verification.md)。
