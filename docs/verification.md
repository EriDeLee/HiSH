# 验收状态

更新：2026-11-01。包内容、源码状态和手机状态分别记录。

## 当前：系统命令认链接、glibc 层、C 库内部函数、pkg doctor 旧错误（2026-11-01）

- 修改：
  - 符号链接第 3 阶段：`shell_module.c` 让每个系统小程序都带着 `libhish_exec_bridge.so` 预加载启动（原先只有 8 个会启动程序的）；该库含 `links_ohos.c` 的导出副本（CMake `hish_links_export` OBJECT 库，`HISH_LINKS_EXPORT`），默认关闭，只在系统小程序里由库的构造函数打开。
  - glibc：`linux/links.c` 加进 `libhish_linux_glibc_tty.so`，另有 `__xstat` 族和 `__open_2` 族。`realpath` 改为不用 `dlsym`（`links_realpath`，三个构建共用）。
  - C 库内部函数（`hish_links_libc.h`）：`scandir`（musl、glibc）、`freopen`（musl、glibc）、`glob`/`globfree`/`nftw`/`ftw`（musl，改编自 musl 源码，MIT）；`HISH_LINKS=0` 时都回到 C 库自己的版本。
  - pkg doctor：`Migrate` 在“无待迁移项”的提早返回路径上也删除 `migration-error`；`Doctor` 只在迁移仍待处理时显示它。
  - 设备检查程序 `tests/fixtures/linux_links.c` 与 `scripts/linux/link-check-build.cjs`。设计见 [符号链接](symlinks.md)。
- 主机：97 项 node:test、27 项会话、3 项编码通过。新增或改写的有：`package-core` 的旧迁移错误用例、`symlink-emulation`（五个构建的严格告警编译，含 glibc、导出版和 x86_64；设备检查程序能为两个 C 库链接；约束用例）、`toybox-exec`（不再有小程序名单，开关只在系统小程序分支）。
- 真机：build-8（device-11）HAP 11,611,841 字节，SHA256 `49499044be6ac29767bb1f3bbb6dcf8fdc9cc28969fb702be5e15698376b6c11`；build-9（device-12，加了 `scandir` 的回退和许可头）HAP 11,611,569 字节，SHA256 `fc45dca6282f63e358f193cace14558cf16d54982279bfd551fe2b0c1f66924e`。都用 `hish-debug.p7b` 签名后覆盖安装，授权仍只有 INTERNET。以下系统命令用例两个版本结果相同。
- 系统命令（`hish-links-8.sh`）：
  - `stat` 把 `lf`、`ld`、`ldangle` 报成 symbolic link（大小 10、4、7），`stat -L` 和经目录链接的 `stat` 跟随到目标；`find . -type l` 找到全部 4 个链接，`find . -type f` 不含链接，`find ld/` 和 `find -L .` 进入目录链接，`find -exec readlink` 正确。
  - `cp` 默认复制目标内容，`cp -P` 和 `cp -a` 复制成链接（`readlink` 为 `real/f.txt`、`real`），`cp -a` 一次复制链接、目录链接、悬空链接、链的链接全部正确；`mv` 链接保留目标。
  - `touch` 经链接改的是目标的修改时间，`rmdir` 目录链接报 Not a directory，`du -a ld/` 进入目录链接，`dd if=链接` 读到内容，`file` 报 “symbolic link to real/f.txt” 与 “broken symbolic link to nowhere”，`md5sum` 经链接与目标相同。
  - `tar cf` 把链接存成链接（`tar tvf` 显示 `lf -> real/f.txt`），`tar xf` 在新目录里重建出链接。
  - 对照：`HISH_LINKS=0 stat` 报 regular file、大小 25（标记本身），`HISH_LINKS=0 find -type l` 数到 0。`HISH_LINKS=0 cp` 把标记复制成普通文件，但保留了 sticky 位，所以它仍是一个有效链接（`cat` 读到 hello），符合此前记录的 `cp` 行为。
- 全部系统小程序（`hish-sweep-10.sh`）：逐个用 `timeout 5 NAME --help` 运行，跳过 mount、reboot 一类会改系统的名字，共 140 个；132 个退出 0 或 1；其余 8 个都是用法错误，没有一个崩溃（没有 126、127、134、139）。`cmp`、`grep`、`egrep`、`fgrep`、`sort` 是内嵌 Toybox，对 `--help` 退出 2；`env`、`nohup`、`timeout` 退出 125，直接运行各自打印一行用法（`usage: env [-0i] …`）。
- 启动耗时（各 10 次连续运行）：预加载的 `stat` 1.25 秒、`date` 1.15 到 1.18 秒（上一版未预加载的 `date` 为 0.94 秒，每次约多 20 毫秒），内嵌的 `ls` 0.87 到 0.91 秒，`bun --version` 1.47 到 1.57 秒。单机，build-8 与 build-9 各测一次。
- C 库层（`linux_links.c`，用 `link-check-build.cjs` 的做法编出）：
  - build-8：musl 47 项全过，glibc 32 项全过（glob、nftw、ftw 跳过）。`HISH_LINKS=0` 时 musl 过 8 项、失败 39 项，glibc 过 1 项、失败 31 项，说明用例分得出开关。build-9 用 `link-check-build.cjs` 编的程序重复了层开的两项，结果相同（47 项、32 项）。
  - 与 C 库逐字节对照（build-9，`hish-links-15.sh`）：不含链接的树上，层开与层关的输出（1305 行）完全相同，musl 与 glibc 都是；有一个目录链接 `sublink -> sub` 的树上两者不同（1354 行对 1324 行，`glob */*` 开着 6 项、关着 3 项），这证明 `glob`、`nftw` 关闭时确实走到 C 库自己的函数，而不是又走了一遍自己的代码。`scandir`、`freopen`、`ftw` 的回退在这些输出里看不出差别，只读过代码，未在手机上证实。
  - 与未改动 musl 源码对照（build-8 `hish-links-14.sh`，build-9 再做一次）：把原 `glob.c`、`nftw.c`、`ftw.c`、`scandir.c` 改名编进同一个测试程序，在两棵不含链接的树上（含空格、中文、`*`、`?`、`[x]`、减号开头、200 字符长名）输出 1305 行和 1392 行完全相同。每棵树覆盖 40 个模式乘 11 组 glob 标志、7 个起点乘 6 组标志乘 3 个深度限制的 nftw、7 个起点的 ftw 和 5 种 scandir。
- 回归：Bun 一致性 33 项全过；`bun install opencode-ai` 新目录 11 秒（build-9 为 9 秒）完成，日志里 symlink 或 EACCES 的行数为 0，`node_modules/.bin/opencode --version` 在 shell 里直接得到 1.19.5；opencode 界面在 build-8 和 build-9 都能打开（截图 `device-11/ui/`、`device-12/ui/`），Ctrl+C 退出回到提示符；上一轮 8 个辅助命令的启动、退出码、环境一致性用例（`hish-verify-7.sh`，build-8）结果与 device-8 相同，`env -i` 清掉 `HISH_PACKAGE_ROOT` 的既有限制不变。
- `pkg doctor`：此前输出 `last migration error: read …/bootstrap-b7bcb46c…/talloc-2.4.3-r0.apk`，同时显示 `app manifest … (accepted)`。该记录来自一个旧版本（自举目录摘要 `b7bcb46c…` 不是当前的 `d7a02a83…`），迁移器在无待迁移项时提早返回，从不删它。新版本第一次会话启动后，`state` 目录只剩 `accepted-manifest`、`legacy-snapshot`、`remote`，`pkg doctor` 不再显示该行。
- 为评估下一步做的只读测量（build-9，`hish-probe-16.sh` 至 `19.sh`、`cpuwatch.cjs`）：
  - opencode 停在界面不动时，8 个 5 秒窗口的总 CPU 中位数 202.4%（200.6 到 223.2），线程 `OS_FFRT_2_1` 和 `Worker` 各约 99 到 100%，常驻内存 763 到 855 MB。与此前的空闲 CPU 记录一致，本轮改动没有改变它，原因仍未查明。
  - 应用数据所在分区 465 GB，可用 318 GB，测试文件不占空间问题。
  - opencode 1.19.5 的 ripgrep 查找顺序（读安装的程序文本）：先 `which("rg")`，再 `~/.local/share/opencode/bin/rg`，都没有就从 GitHub 下载 15.1.0，平台键写死为 `arm64-linux`（gnu 版）。本机两处都没有 `rg`；curl 和 Bun 的 `fetch` 访问 GitHub 都报 `self-signed certificate in certificate chain`（本机网络换了证书），即使下载成功，gnu 版也缺 `libpthread.so.0` 跑不起来。Alpine 有 `ripgrep-15.1.0-r0`（同版本）。尚未验证：opencode 的 grep、glob 工具此时是否显示失败，`pkg install ripgrep` 后是否恢复。
- 未验证：
  - 真实的 glibc 应用：没有能跑的。随包 glibc 运行库不含 `libpthread.so.0`，手机上 Bun 编译的 glibc 版 opencode 报 `error while loading shared libraries: libpthread.so.0`（`hish-probe-11.sh`）。glibc 层只用自己的 glibc 测试程序验证过。
  - `dlopen`、glibc 的 `fts`/`glob`/`nftw`/`ftw`、系统 Toybox 的 `mknod`、`mkstemp`、`statfs`、扩展属性穿过目录链接。musl 没有 `fts`，`popen`/`system` 由 mksh 解释命令，这两项不是缺口。
  - `readdir` 仍把链接标记报成普通文件（`d_type` 为 8）。
  - `pkg upgrade` 含程序标记的包在真机上的流程；长时间使用下各预加载层的稳定性。
  - 20000 次 `stat` 的耗时：musl 开着 74 毫秒、关着 176 毫秒，glibc 开着 95 毫秒、关着 175 毫秒，各一次测量。关着反而慢，原因未查，不能据此说层加快了调用，也不能说它的开销有多大。
  - musl 许可文本随包交付仍是上文记录的待办，改编的 `glob` 等函数增加了这一项的覆盖范围。
  - 手机上的测试文件未清理：`~/oc-idle` 676 MB（其中 `links3` 632 MB 是第二份 opencode 安装）、`~/node_modules` 632 MB、`native/tmp` 25 MB（101 个文件）、两个包代次 245 MB（`hish-probe-16.sh`）。这些会误导 opencode 的模型。

## 前轮：符号链接模拟第 2 阶段、程序链接存标记、git 变量（2026-11-01）

- 修改：(1) `hish_links_impl.h` 抽出共享逻辑，`links_ohos.c` 把同一套行为用真实 libc 名字、hidden 可见性编进 `libhish_shell.so`（mksh、内置 Toybox、pkg、装载器），见 [符号链接](symlinks.md)。(2) pkg 把指向较大可执行文件（至少 64 KiB、带执行位、不是库）的文件链接存成标记，其余文件链接仍是副本。(3) 会话启动时设置 `GIT_EXEC_PATH`、`GIT_TEMPLATE_DIR`，`pkg` 函数换代时同步。第一次构建失败：x86_64 模拟器 ABI 同时构建，`links_ohos.c` 对非 AArch64 报 `#error`，已改为该 ABI 下为空并加了测试。
- 主机：95 项 node:test（`symlink-emulation` 由 3 项改写为解析器 37 例、两个 libc 层严格告警编译含 x86_64 空文件、不调用被接管的 `syscall()` 与 hidden 约束；`package-core` 新增“程序链接成标记、库和数据和小程序链接仍是副本”）、27 项会话、3 项编码通过。
- 真机（build-7b、device-10）：签名 HAP 11,571,025 字节，SHA256 `db19053c54b8f748272b429d373dc76fec7ab413906a4ae8ba8d7684557f8c51`，`hish-debug.p7b` 签名后覆盖安装，授权仍只有 INTERNET。
- 第 2 阶段，shell 与内置 Toybox（`hish-links-6.sh`）：`ln -s` 对文件、目录、悬空目标都成功，再建同名为 “File exists”，`ln` 不带 `-s` 仍是 Permission denied；`ls -l` 显示 `lrwxrwxrwx … lf -> real/f.txt` 等三条；`readlink` 返回目标，对普通文件退出码 1；`realpath` 给出规范路径；`cat` 经文件链接和目录链接读到内容，悬空链接报 No such file；`test -L`、`[ -h ]`、`-e`、`-d`、`-f` 结果都与真实链接一致，悬空链接 `-e` 为假而 `-L` 为真；`cd` 进目录链接后 `pwd` 为链接路径、`pwd -P` 为目标目录；环链接报 “Symbolic link loop”。
- 在 shell 里直接执行链接（同一脚本）：当前目录的文件链接、经目录链接、绝对目录链接、经 PATH 查找都输出正确；第 3 轮装出来的 `node_modules/.bin/opencode --version` 在 shell 里直接得到 1.19.5（此前是 Permission denied）。`mv` 链接、`rm` 链接、`rm` 目录链接、`rm -r` 目录链接都只动链接，目标保留。
- 系统 Toybox 命令未覆盖，同一脚本如实报告：`stat -c %F` 对链接报 “regular file”，`find . -type l` 没有输出，`cp` 链接得到的新文件仍是链接（把标记当文件复制）。这些属于第 3 阶段。
- git 变量与程序链接（`hish-links-7.sh`，在交互 shell 里 source，使用 `pkg` 函数）：新会话里 `GIT_EXEC_PATH`、`GIT_TEMPLATE_DIR` 已指向当前代次；`pkg remove git curl` 后本窗口换到 g000110，两个变量被取消；`pkg install git curl` 用 12 秒（此前 21 秒），换到 g000111，两个变量指向新代次。代次目录 584,212 KB 降到 135,840 KB。`ls -l` 显示 `git-receive-pack -> git`、`git-add -> ../../bin/git` 是链接，`libcurl.so.4`（790,928 字节）仍是副本。
- 之后（同一脚本）：在 shell 里直接敲 PATH 上的链接名 `git-upload-archive` 能运行；不手动设任何变量，`git ls-remote https://gitlab.alpinelinux.org/alpine/aports.git HEAD` 成功；本地 `git init`、`add`、`commit`、`log` 正常，链接存为 120000；`git ls-remote` 本地路径和 `git clone --no-local` 会让 git 自己启动指向 `git` 的 `git-upload-pack` 链接，结果正确，检出里 `link.txt -> a.txt`，status 为空。
- 未验证：第 3 阶段的系统命令；`HISH_LINKS=0` 对新 `libhish_shell.so` 的效果；长时间使用下 mksh 和 Toybox 经过被接管的 libc 调用（每次 `open` 多一次 `fstat`，`access` 和 `stat` 多一次检查）的性能，未量；`pkg upgrade` 含程序标记的包在真机上的流程（主机测试覆盖安装、换代、卸载）；`scandir`、`glob` 等经 libc 内部调用的路径。

## 前轮：符号链接模拟第 1 阶段（2026-11-01）

- 实现：`hish_link_core.h`（标记格式和路径解析）、`linux/links.c`（编进预加载库 `libhish_linux_identity_padded.so`）、`linux/exec.c`（读文件前跟随链接）、pkg 的目录链接标记（`WriteLinkMarker`）。设计、覆盖和盲点见 [符号链接](symlinks.md)。标志位选 sticky，因为已实测它在这台手机的应用存储上经创建、chmod、cp、mv、追加、重写都保留。
- 主机：94 项 node:test（新增 `tests/symlink-emulation.cjs` 3 项：解析器 37 例、对 AArch64 musl 严格告警编译、与 syscall 替换相关的结构约束；`tests/package-core.cjs` 把“目录链接被拒”改为“目录链接成为标记并随代次保留”）、27 项会话、3 项编码通过。
- 真机（build-6、device-9）：签名 HAP 11,551,849 字节，SHA256 `22b067eb3a09771dd7d51d47b5129d7f63eafead4cdeec6a8db2199e6dac7121`，`hish-debug.p7b` 签名后覆盖安装，授权仍只有 INTERNET。
- Bun 经 libc 的调用，33 项全过（`links-conformance.js`）：`symlink`、`readlink`、`lstat`、`stat`、`exists`、`access`、`chmod`、`utimes`（经链接只改目标）、已存在名字 EEXIST、绝对目标、链接链、空格和中文名、4000 字节目标、4096 字节目标 ENAMETOOLONG、悬空链接、两种环和自环（stat 为 ELOOP）、目录链接内的 `stat`/`mkdir`/`rename`/`unlink`、`chdir` 进目录链接、`..` 在目录链接之后按目标目录上行、`unlink` 链接不动目标、`rename` 链接、`rm -r` 目录链接只删链接、`rmdir` 目录链接为 ENOTDIR，以及经链接、经目录链接执行脚本和 Linux 程序。第一版套件把 Bun 自己的读写也算进去：写入经链接时 Bun 内联的 `openat` 直接改写了标记文件，后面的用例连带失败（24 过 24 败）。已拆开：这些调用改作 INFO 报告，不计入。
- Bun 自己的文件接口不经过 libc，看不到模拟：`readFileSync` 读到 `!<hish-symlink>pt`，`Bun.file().size` 为 17（标记长度），`copyFileSync` 复制的是标记，`readdirSync(目录链接)` 为 ENOTDIR，`realpathSync(链接)` 返回链接自身，`readdir withFileTypes` 把链接报成普通文件。读 `.text` 的静态统计与此一致，见 [符号链接](symlinks.md)。
- C 程序经 libc（jq、git，`hish-links-2.sh`）：`jq` 读文件链接、链接链、目录链接内文件都得到正确内容，悬空链接报“No such file or directory”；Bun 的 `spawn` 经目录链接执行脚本输出正确。git 往返：`git init` 不把 `core.symlinks` 记为 false，`git add` 把文件链接和目录链接都存成 120000，`commit` 后 status 为空，删除后显示两项 ` D`，`git checkout` 重建出标记，再 status 为空，`git clone` 的检出里链接和目标都对。zstd：加 `-f` 压缩链接的目标并能解压回 `{"a":1}`；不加 `-f` 时打印 “is a symbolic link, ignoring”，与真实 Linux 的 zstd 对链接的处理一致（`hish-links-5.sh`）。
- `bun install opencode-ai`（`hish-links-3.sh`，新目录）：12 秒完成，日志里提到 symlink 或 EACCES 的行数为 0，输出“installed opencode-ai@1.19.5 with binaries: - opencode”；`node_modules/.bin/opencode` 是链接，目标 `../opencode-ai/bin/opencode.exe`；Bun 的 `spawn` 经它得到 1.19.5，`bun run opencode --version` 也是 1.19.5。在 shell 里直接敲 `node_modules/.bin/opencode` 仍是 Permission denied（shell 一侧属第 2 阶段）。
- `pkg install git curl`（`hish-links-4.sh`）：21 秒成功，此前被 `etc/ssl1.1/certs` 挡住。该路径现在是标记 `!<hish-symlink>../ssl/certs`，Bun 的 `lstat` 为链接、`readlink` 为 `../ssl/certs`。默认环境下 `git version 2.52.0`、curl 8.23.0 可运行，`curl https://registry.npmjs.org/` 返回 200（只靠会话的证书变量），git 本地 init、add、commit、log、status 正常。代价：git 的 148 个文件链接存为副本，代次目录从 109,470 KB 增到 584,212 KB（约 475 MB）。
- git 走 https 时，不设 `GIT_EXEC_PATH` 报 “'remote-https' appears to be a git command, but we were not able to execute it”；设成 `$HISH_PACKAGE_ROOT/usr/libexec/git-core` 后 `git ls-remote https://gitlab.alpinelinux.org/alpine/aports.git HEAD` 成功。本地 git 提示 `templates not found in /usr/share/git-core/templates`（无害）。会话尚未自动设置这两个变量。
- 速度（各为一次测量，单机）：`statSync` 普通文件 20000 次 135 ms 增至 167 ms（每次约 +1.6 µs）；`existsSync` 不存在的文件 20000 次 125 ms 增至 217 ms（每次约 +4.6 µs，多一次对最后一段的检查）；`lstatSync` 链接 5000 次 94 ms；`symlink` 加 `unlink` 一对约 0.2 ms。
- 观察到但未调查：`pkg doctor` 显示 `last migration error: read …/bootstrap-b7bcb46c…/talloc-2.4.3-r0.apk`，该自举目录不是当前版本的（当前为 `bootstrap-d7a02a83…`），可能是旧版本留下的记录。
- 未验证：任何经 glibc 库的程序；`scandir`、`glob`、`nftw`、`freopen` 的路径经链接；动态链接器装载库时遇到标记；长时间使用下预加载层的稳定性；`pkg` 升级和卸载含标记的包在真机上的流程（主机测试覆盖了安装、换代、卸载）；`HISH_LINKS=0` 在真机上只用于速度基线，未验证它能让已有标记恢复为普通文件的行为。

## 前轮：Toybox 辅助命令与会话证书变量（2026-11-01）

- 修改：(1) 内嵌 Toybox 的 `execvp` 改名为 `HiSHToyboxExecvp`（`toybox_adapter.c`，查找规则在 `toybox_exec_path.h`），转入 mksh 用的 `HiSHExecFile`。(2) 系统 Toybox 里的 `env`、`timeout`、`nice`、`nohup`、`setsid`、`taskset`、`time`、`find` 带着 `libhish_exec_bridge.so` 预加载启动（`HiSHExecSystemApplet`），该库导出 `execvp`、`execve`、`vfork`，机制见 [ELF 兼容层](elf-runtime.md)；装载代码改用 `syscall(SYS_execve)`，不经过被替换的符号。(3) 会话启动时把 `SSL_CERT_FILE`、`CURL_CA_BUNDLE`、`GIT_SSL_CAINFO` 指向 pkg 自举目录里的 CA 包（`HiSHPackageCertificates`），已设置的值不覆盖。
- 原因的更正：前一轮把“辅助命令不能启动 Linux 程序”归因于内嵌 Toybox 的 `xexec`。实际只有 `xargs` 是内嵌的；其余是系统 `/system/bin/toybox` 的小程序（`libhish_shell.so` 符号表没有它们的 `*_main`）。系统 Toybox 动态导入 `execvp`、`execve`、`fork`、`vfork`；读 `.text` 的调用点：`execvp` 只在一个小函数里调用（有 10 个调用者，符合 `xexec`），`execve` 只在另一个函数里调用，反汇编可见它处理 `-i`、`NAME=值`、`PATH=` 并自己逐项搜索 PATH，符合 `env`，不走 `xexec`。所以 build-3 只修好 `xargs`，build-4 修好除 `env` 外的 7 个，build-5 补上 `env`。
- 主机：90 项 node:test（新增 `tests/toybox-exec.cjs` 5 项：只有 xexec 启动程序且构建重命名它、预加载名单与导出、对 OHOS 头文件的编译检查、PATH 查找 21 例、系统小程序环境往返 14 例）、27 项会话、3 项编码通过。
- 真机（build-5、device-8）：签名 HAP 11,529,992 字节，SHA256 `6e46a75221c4b2e1cc205e99c9cf2675e01c6d0a83f10c72b8a403714b5de584`，`hish-debug.p7b` 签名后覆盖安装，授权仍只有 INTERNET。以下均为该版本：
  - 8 个命令各自启动 `bun --version` 输出 1.4.4、退出 0（`setsid` 无 `-w` 选项，按无选项测）。`find -exec sh -c …` 通过。`env timeout …`、`timeout env …`、`env sh -c 'env bun --version'` 嵌套通过。
  - 退出码和信号：`env bun -e 'process.exit(5)'` 为 5，`timeout … exit(6)` 为 6，`timeout sh -c 'exit 7'` 为 7，`env sh -c 'exit 3'` 为 3；`timeout 2 bun -e 'await Bun.sleep(30000)'` 约 2 秒后退出 124；`env` 找不到命令 127、不可执行文件 126、运行带 shebang 的脚本输出正确，`env PATH="$w:$PATH" hello.sh` 按新 PATH 找到脚本。
  - 环境：被启动的 Bun 看到的 `LD_*`、`HISH_*` 在直接运行、`timeout`、`env`、`nice`、`find -exec` 五种方式下逐字相同，没有 `HISH_SAVED_*` 或 `HISH_SYSTEM_APPLET`；调用者设置的 `LD_LIBRARY_PATH`（指向应用目录内）在五种方式下以同样位置出现。
  - 未改变的行为：`env -i` 清掉 `HISH_PACKAGE_ROOT`，随包 Bun 找不到 `libstdc++.so.6`；不用任何辅助命令、只在 shell 里去掉该变量结果相同；`env -i HISH_PACKAGE_ROOT=… bun --version` 正常。属已有限制，与本机制无关。
  - 不在名单内的系统小程序（`cp`、`touch`、`date`、`ls`、`uname`）照常工作；bun、jq、zstd、`node -e`、`pkg list`、`xargs -n1 bun`、mksh 关键字 `time` 回归正常。
- 证书（build-3、device-6 通过，之后版本未改此处）：新会话里三个变量都指向 `native/packages/bootstrap-d7a02a83…/etc/ssl/certs/ca-certificates.crt`（178,791 字节）。不带任何证书选项时，Alpine 的 curl 访问 https://registry.npmjs.org 返回 200，git ls-remote https://gitlab.alpinelinux.org/alpine/aports.git 成功，`xargs -n1 curl` 同样 200；Bun 的 `fetch` 仍返回 200，新变量没有影响它。给单条命令设的错误值仍然优先（curl 退出 77）。
- 未验证：会话启动前已设置的证书变量不被覆盖（只读过代码：`setenv(…, 0)`）；`flock`、`watch` 等不在名单内的系统小程序是否同样需要；`pkg` 装好的 git/curl 在不设 `GIT_EXEC_PATH` 时的表现。
- 符号链接模拟的前期探测（只读，未实现模拟）：应用存储上的普通文件能保留 sticky(1000)、setgid(2000)、setuid(4000) 及其组合；`chmod`、`open(…,01600)` 创建、`cp`（不带 `-p` 也保留）、`mv`、追加、重写之后都不丢，目录同样；`readdir` 仍把这些文件报成普通文件；真正的 `symlink` 仍是 EACCES（`hish-probe-9.sh`，device-8）。Bun 1.4.4 动态导入 `symlink`、`symlinkat`、`readlink`、`readlinkat`、`link`、`linkat`、`lstat`、`stat`、`fstat`、`open`、`openat`、`opendir`、`readdir`、`mkdirat`、`renameat`、`realpath`、`access`、`syscall`（读 `.dynsym`），说明它的这些文件操作经过 libc 而不是内联系统调用。
- 启动耗时（build-5，各 10 次连续运行的总时间）：未预加载的系统小程序 `date +%Y` 0.94 s，预加载的 `nice true` 1.11 s（每次约多 17 ms），`env bun --version` 2.66 s，直接 `bun --version` 1.53 s。单机单次测量，未重复。

## 前轮：opencode 实际会话与空闲 CPU（2026-11-01）

- 只读取证，没有改代码或包。会话库、日志和截图拷到 `C:/Users/Lee/opencode/tmp/opencode/hish-oc-session/`。
- 会话：2026-10-31 20:26–20:28，opencode 1.19.5，模型 opencode/big-pickle（免费，无需 `/connect`），用户请求“探索一下系统”。16 条消息、26 次 bash 工具调用全部完成，3 次非零退出都是模型自己命令里不存在的路径或 mksh 不支持的选项；中位耗时 554ms，除两次等待外最长 2.9s。两次 41.7s、33.3s 的等待发生在读 `native/tmp/*` 的 `external_directory` 权限询问之后，是等待授权，不是命令慢。
- 模型的结论中，`git`、`curl`、`tcc`、`gnu-*` 是旧转发脚本、执行报 `No such file or directory`，与 [Shell](shell.md) 的设计一致（真正的 git/curl 需 `pkg install`，本轮未装未测）。它还称“OpenTUI 渲染库加载失败”，那是读了修复前遗留在 `native/tmp` 的诊断文件 `hish-oc-diag.txt`，现状并非如此；这些遗留测试文件会误导模型，尚未清理。
- opencode 日志 5 条 WARN：`failed to initialize fff` ×4（工作目录是 HOME，上游拒绝在主目录建文件索引）；`background dependency install failed` ×1，原因 `EACCES … symlink '../uuid/dist-node/bin/uuid' -> …/.config/opencode/node_modules/.bin/uuid`，即本设备拒绝符号链接，opencode 自带的插件依赖安装在这里失败。
- 空闲 CPU：界面 20 分钟无变化（11:19 与 11:39 截图文字相同），同一 opencode 进程的两个线程（主线程 `OS_FFRT_2_1`、`Worker`）在 `top -H` 的全部采样中各占约 100%，持续至少 12 分钟，每线程累计 CPU 约 43 分钟；`hidumper --cpuusage` 为用户态 16.47%（约 2 核）、内核态 0.00%。常驻内存 508MB（top），PSS 884,767kB，其中 SwapPss 397,530kB。opencode 日志里该进程在 2026-10-31 20:29 之后的唯一活动是 2026-11-01 11:01 的一次实例启动。原因未验证。
- 上游有同类报告：anomalyco/opencode #11119（已关闭）、#30086（重复关闭，55 条评论，有人用 `"snapshots": false` 缓解）、#31831（不计划处理），症状为空闲 150–500% CPU、400–1000MB 内存。本机上其中两个常见原因都不成立或未核实：fff 初始化失败，没有 `fff-bg-*` 线程；snapshot 未核实。
- 同日复测（用户批准，退出旧进程，用 `/exit`，数据未丢；全部为新进程、每次约 1–3 分钟、`top -H` 每 5 秒一窗，`cpuwatch.cjs`）：
  - 新建空目录 `~/oc-idle` 里启动：33 窗中位 200.8%（最小 200.4，最大 217.0），两个热线程同前，启动后第一窗起就是；RES 约 760MB。**所以与 HOME 目录、旧进程无关，可稳定复现。**
  - 关掉所有可选后台功能（`OPENCODE_DISABLE_FFF`、`OPENCODE_EXPERIMENTAL_DISABLE_FILEWATCHER`、`…AUTOUPDATE`、`…PRUNE`、`…MODELS_FETCH`、`…MOUSE`、`…TERMINAL_TITLE`、`…AUTOCOMPACT`、`…PROJECT_CONFIG`、`OPENCODE_PURE`）：17 窗中位 200.4%。
  - PATH 最前放可正常应答的 git、rg 替身：14 窗中位 201.0%。
  - 对照：同一台手机上的普通 Bun 进程（主线程加一个 Worker、1 秒定时器）空闲 8 窗中位 0.2%，最大 2.6%。
  - 其他排除项：Bun 定时器正常（`setInterval(16)` 59.2 次/秒、链式 `setTimeout(1)` 约 689 次/秒、Worker 内相同）；JIT 已开（热循环 1.2ns/次，DFG 编译 1 次）；系统日志（hilog）里该进程 0 行；opencode 自身日志每次启动只有 15 行；系统其他进程无异常占用（`hidumper --cpuusage`）。
  - 结论：空转发生在 opencode 内部，与 Bun、线程、定时器、JIT、目录、git/rg 缺失及上述可选功能无关；每次启动即复现，所以不是偶发。具体循环位置未知：`hidumper` 报告用户态占满、内核态 0.00%，但 HongMeng 的内核态统计是否可信未验证，不能据此排除系统调用空转（先前“不是系统调用”的说法过强，已撤回）；也没有符号可采样。
- git/curl：`pkg install git curl` 失败，事务回滚，已装包一个没变：`pkg: unsupported directory symlink: etc/ssl1.1/certs; transaction not activated`。该目录链接来自 `ca-certificates-bundle`（`etc/ssl1.1/certs -> /etc/ssl/certs`，OpenSSL 1.1 旧路径）；这一组 16 个包共 164 个符号链接，163 个是文件链接（`pkg` 已按副本处理），只有这 1 个是目录链接。git 与 curl 因此仍不能装。
- 符号链接探测（`symprobe.sh`）：在 HOME、tmp、files、cache、temp、preferences、database 共 7 个应用可写目录里，符号链接、硬链接、指向 `.` 的目录链接全部 `Permission denied`；另 4 个路径不存在。官方文档（`@ohos.file.fs`）写明 `fileIo.symlink` “从 API version 11 开始，不支持三方应用使用”，与实测一致。
- 官方文档《NDK涉及的musl libc接口使用限制的说明》列出 `link`/`linkat`/`symlink`/`symlinkat`/`readlink`/`readlinkat` 受 SELinux 管控（`lnk_file create`、父目录 `add_name`），应用无法改变；与上面的实测一致，所以应用存储里无法建真正的符号链接或硬链接，任何“让链接可用”都只能是 HiSH 内部的模拟。
- Bun 自带的 `--cpu-prof`（`BUN_OPTIONS="--cpu-prof --cpu-prof-md --cpu-prof-dir=prof"`，空闲 25 秒后 `/exit`）没有生成 `prof` 目录，未取得 profile。改用系统的 `hiperf`：
  - 只有 `hiperf record --app com.lee.hish` 被允许（`-t` 要求 debug 或 profileable 应用，`-a` 要求 root）；它只采样命令名为 `com.lee.hish` 的进程，所以 opencode 必须直接在提示符下启动，经 `sh 脚本` 或 `exec` 启动的进程名是 `toybox`，采不到。原始数据 `hish-oc-session/oc-perf2.data`（6 秒、12,307 个样本）。
  - 两个热线程各占 50% 周期：约 29% 在 HiSH 的 musl 层 `epoll_pwait` 里，其余在 Bun 事件循环：`EventLoop::tick`、`Timer::drain_timers`、`drain_due_wtf_timers`、`get_timeout`、`us_loop_run_bun_tick`，以及“即将等待”钩子 `Bun__JSC_onBeforeWait` 里 JSC 的垃圾回收步骤 `Collector::runCurrentPhase`（约 10%）。热点里没有 JavaScript 即时编译代码。调用链：`main → cli::start → boot_standalone → Run::start → VirtualMachine::load_entry_point → EventLoop::wait_for_promise → auto_tick → us_loop_run_bun_tick → bun_epoll_pwait2 → epoll_pwait`，Worker 线程同样。
  - 地址转函数名的依据：opencode 内嵌的运行时是 Bun 1.4.4（二进制里有 `Bun/1.4.4`），它的 76,334,736 字节代码段与手机上 `pkg` 装的 Bun 1.4.4、以及 Bun 官方发布的带符号版 `bun-linux-aarch64-musl-profile.zip`（205,042,325 字节）逐字节相同，且程序按链接地址映射，所以采样地址可直接查带符号版。opencode 自己的符号表只有 39KB，不能用。
  - 解读：两个线程的事件循环都没有真正睡眠（等待超时为 0 或极短），并持续处理定时器；手机上的普通 Bun 进程空闲时不会这样。为什么超时为 0 未确定，尤其未确定是不是 HiSH 造成的。`BUN_JSC_useConcurrentGC=0` 无变化（9 窗中位 202.2%）。
- 真 git/curl 的运行测试（不是安装）：把 `pkg` 本该装的同一批 16 个 Alpine APK（git 2.52.0-r0、curl 8.23.0-r0 及其依赖）在电脑上解到临时目录（文件链接按副本、目录链接跳过、全程不建链接），传到手机 `~/oc-idle/gitroot` 运行，没有改任何包状态。结果：
  - `git --version` 为 2.52.0，`curl --version` 为 8.23.0，均退出 0。
  - git 本地操作全部退出 0：init、add、commit、log、status、diff、branch、gc、fsck，以及分离的 `--git-dir` 与 `--work-tree`（init、add -A、write-tree），后者是快照类工具的用法。
  - 从 gitlab.alpinelinux.org 做 HTTPS 浅克隆（`--depth 1`）成功，6 秒、122 个对象、fsck 无输出；`git ls-remote` 经 HTTPS 退出 0；curl 访问 http://example.com 为 200，带 `--cacert` 访问 https://registry.npmjs.org 为 200。
  - 需要显式设置：`LD_LIBRARY_PATH`（临时目录不是代次，真正安装后随包库目录由 HiSH 自动搜索，未验证）、`GIT_EXEC_PATH`、`GIT_TEMPLATE_DIR`，以及证书路径。Alpine 构建把 `/etc/ssl/certs/ca-certificates.crt` 写死在程序里，不设时报 `error adding trust anchors from file: /etc/ssl/certs/ca-certificates.crt`，curl 退出 77。不设 `GIT_EXEC_PATH`、`GIT_TEMPLATE_DIR` 时的行为没有单独测。
  - github.com 的 HTTPS 失败（`self-signed certificate in certificate chain`）不是 HiSH 或 git 的问题：这台手机的网络返回的 github.com 证书由 “Scholar Root CA v1”（O=Scholar Verify）签发，gitlab.com、npmjs、Alpine 的服务器都返回正常公共证书；用 HiSH 自带的 CA 包结果相同。
- Toybox 辅助命令不能启动任何 Linux 程序：对目标 `bun --version`（musl 程序），`timeout`、`env`、`xargs`、`nice`、`nohup`、`setsid`、`taskset`、`time`、`find -exec` 全部报 `exec bun: Permission denied`（`find` 退出码仍为 0，其余为 126）；直接运行与 `sh -c` 正常。当时读源码得出的原因（Toybox 的 `xexec` 直接调用 `execvp`，`third_party/toybox/lib/xwrap.c:232`，只有 mksh 的 `execve` 被重定向）只适用于内嵌的 `xargs`；其余是系统 Toybox 的小程序，见本文件“当前”一节的更正。opencode 会话里模型用过的 `timeout 8 curl …` 属于后一类。
- 未由本轮证明：`/connect` 其他服务商；opencode 的文件编辑与搜索类工具；`pkg` 装好的 git/curl（本轮只证明同样的程序在临时目录能跑）；不设 `GIT_EXEC_PATH` 时 git 的表现。
- 手机上遗留（未删除）：`~/oc-idle`（我建的测试目录，内含约 13 个小脚本、`gitroot/` 37MB、`gitwork*` 克隆），`native/tmp/hish-*` 约 20 个测试文件。`pkg` 状态与测试前相同（代次 g000108）。

## 前轮：opencode 界面可打开（2026-10-31）

- 症状：运行 `opencode.exe` 报 `Failed to initialize OpenTUI render library`，`Error loading shared library …/tmp/.bun-20020268-….so: No error information`。
- 原因：Bun 把 opencode 的界面库 `libopentui.so`（13,869,504 字节，Zig 构建）解出到 TMPDIR 后 dlopen，它的依赖是 musl 标准名 `libc.so`。DSO 检查把 `libc.so` 一律当作 OHOS libc 拒绝（ELIBBAD）；musl 没有 ELIBBAD 文案，所以只显示 No error information。用 `HISH_MUSL_DSO_DIAGNOSTICS=1` 看到的是 `validate DSO dynamic table/libc errno=80`。该库需要的 90 个外部符号全部由 HiSH musl 提供。
- 修复：只有位于 HAP 库目录、`/data/storage/el1/bundle` 或应用存储之外、或读不到来源的 DSO，依赖 `libc.so` 时才拒绝；其他位置按上游 musl 解析为 loader 自身。glibc 名与 `libc++.so` 仍一律拒绝。
- 真机（build-2、device-4）：bun、jq、zstd、`node -e` 正常；TMPDIR 中的界面库 dlopen 成功；HAP 自带的 OHOS `libc++_shared.so` 仍被拒绝。opencode 1.19.5 界面正常显示（`device-4/oc-2.jpeg`），Ctrl+C 退出后终端回到提示符。授权仍只有 INTERNET。
- 签名 HAP 11,518,466 字节，SHA256 `17d756fba799432e49ce52e7208bfc2ac13a8c9344d541e8952cfd63294ca45d`，以 `hish-debug.p7b` 签名后覆盖安装成功。
- 主机：85 项 node:test、27 项会话、3 项编码通过，`git diff --check` 通过。
- 未由本轮证明：接入模型后的实际对话与工具调用（`/connect`、ripgrep、git、shell）；长时间使用；opencode 进程内存。

## 前轮：bun install 可装 opencode-ai（2026-10-31）

- 症状：`bun install opencode-ai` 只报 `postinstall script from "opencode-ai" exited with 127`，没有原因。
- 原因一：安装脚本是 `node ./postinstall.mjs`，会话里没有 node。Bun 1.4.4 的临时 node 是 `/tmp/bun-node-*` 下的符号链接，本设备会话中 `/tmp` 不存在，符号链接与硬链接也被拒绝，Bun 不报错直接跳过。最小测试证实脚本已启动并打印 `sh: node: inaccessible or not found`。
- 原因二：脚本随后要运行 opencode 主程序验证，主程序 217,521,256 字节（musl）/ 211,209,208 字节（glibc），超过原 128MiB 文件上限。
- 另见：依赖包安装脚本的输出在本设备丢失，同一脚本普通模式只剩 `exited with 7`，`--verbose` 才可见。未修，原因未查。
- 修复：Bun 包附带 `bin/node`（`exec -a node bun "$@"`）；三个装载入口的文件与映像上限改为 512MiB；直接运行入口只读头部，各段从文件读入，不保留整文件副本。
- 真机（build-1、device-2）：bun、jq、zstd、bun 启动自身子进程均正常；同一条空闲 `bun -e` 进程 VmRSS 205,084kB → 106,948kB。重装 bun 后新代次有 `bin/node`，`node -e` 输出 `node object v26.3.0`；依赖安装脚本能找到 node。`bun install opencode-ai` 退出 0，`bin/opencode.exe` 为 217,521,256 字节的 musl 版，`--version` 输出 `1.19.5`。`pkg doctor` 正常，授权仍只有 INTERNET。
- 签名 HAP 11,518,912 字节，SHA256 `bae76e28add654d76496e5290811f2ddcc0f5b9fc644a389363ad6b6bc5d016f`，以 `hish-debug.p7b` 签名后覆盖安装成功。证据在 `C:/Users/Lee/opencode/tmp/opencode/hish-node-127/`。
- 主机：85 项 node:test（含新增 node 替身位置检查）、27 项会话、3 项编码通过，`git diff --check` 通过。
- 未由本轮证明：opencode 交互界面；PRoot 在新上限下的功能探针；glibc 版 opencode 未被选用的原因。`node --version` 与 `node -v` 不输出版本：Bun 1.4.4 只在 `bun` 名下处理 `--version`，Node 模式下在终端里进入 REPL，否则报 Missing script。
- 测试中向等待输入的 Bun REPL 发送 Ctrl+D 没有效果；脚本结束后会话退出，显示“终端已结束”。二者是否相关未单独验证。

## 前轮：固定地址程序在提示符下可运行（2026-10-31）

- 症状：提示符下 `bun` 报 `File exists`、退出码 126，而脚本里的 bun 正常。原因是会话 shell 是应用进程的 fork 副本，继承了静态 ArkTS 运行时在前 4GiB 预留的 512MiB 对象堆（实测 `0x00140000-0x20040000`，应用侧同一块 523,264kB、实际占用 0kB）；Bun 的固定地址 `0x00200000-0x062ec000` 落在其中，预留时返回 EEXIST。
- 修复：仅在 EEXIST 时读一次 `/proc/self/maps`，只释放可证明属于休眠 ArkTS 静态堆的重叠部分，再重试一次；其他映射一律不碰，保留原报错。仅限带 `HISH_SESSION_COPY=1` 的会话副本，且只发生在即将被替换的命令进程。不碰撞的程序不读内存表。
- 真机（build-7）：会话 shell pid 22476 仍有 5278 个映射、`OVERLAP 00140000-20040000` 仍在，同一窗口内 `bun --version` 输出 `1.4.4`、退出码 0，`bun h.js` 输出 `42`；`jq -n 42`、`zstd` 1.5.7、`pkg list` 输出与改动前一致；标记 `HISH_SESSION_COPY=1`。
- 签名 HAP 11,519,123 字节，SHA256 `06a47eae6cd683353bb236f6bd5bb67cd3d2810a000c7830bee66a6137a37004`；覆盖安装成功。证据在 `hish-pkg-refactor/build-7`、`device-16`。
- 主机：新增 `tests/elf-dormant-heap.cjs`（12 个用例）通过。同一位置另测 `package-core` 13/13 通过；在 `Documents` 仓库内该项仍随机出现 `cannot rename: Permission denied`（文件同步/杀毒干扰，换位置即不复现，与本改动无关）。
- 未由本轮证明：其他固件若改名或改布局，会退回原来的 `File exists`；除 Bun 外的其他固定地址程序；释放前后的异常退出路径；ArkTS 归属是源码比对推断，商用固件源码不可见。

## 前轮：代次化包管理器重构（2026-10-31）

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
