# 测试

## 主机回归

从仓库根执行；测试使用已有 SDK 和缓存，在 `.test` 写隔离输出，不安装工具：

```cmd
node --test tests/mksh-integration.cjs tests/daily-packaging.cjs tests/package-inventory.cjs tests/signed-packaging.cjs tests/base-distribution.cjs tests/repository-hygiene.cjs tests/source-runtime.cjs tests/daily-appearance.cjs tests/musl-source.cjs tests/package-manager.cjs tests/package-core.cjs tests/elf-dormant-heap.cjs tests/toybox-exec.cjs tests/symlink-emulation.cjs tests/epoll-emulation.cjs
node tests/native-session.cjs
node tests/terminal-codec.cjs
git diff --check
```

mksh 检查会读取已构建原生对象；改源码后先构建，不拿旧对象当新代码证据。底座合同测试将真实共享 C 辅助函数优化为 LLVM IR，检查旧脚本匹配及普通命令名不封禁；它不是设备运行测试。

## 包检查

```cmd
node scripts/inspect-daily-hap.cjs PATH_TO_HAP PATH_TO_SELECTED_PAYLOADS
```

检查只读 ZIP 公共条目、允许的代码和许可，不提取或读取签名材料。库与 rawfile 必须逐项匹配交付规则；额外条目也须在规则内。还检查普通应用/编译器数据、专属初始化函数、旧 shell、探针和闲置资源排除。

逐个比对六个运行库的可执行段。SDK strip 会改变从偏移 0 开始的 RX 段内三个 ELF 节表元数据字段；只规范化 e_shoff/e_shnum/e_shstrndx，其他头字段和指令仍精确匹配，并有正反例测试。

SDK 的 `ets/sourceMaps.map` 只允许该位置，禁止 sourcesContent，来源必须匹配当前 18 个 ArkTS 模块记录。不能因为菜单关闭就断言其代码未编入。

签名后以已核验的未签名包为基准：原公共条目内容必须一致，只允许签名工具新增 `.pages.info`，不读取其内容。`compareSigned` 检查此边界；未签名包检查仍拒绝额外条目。新增代码、变更原代码/元数据、缺失原条目均有失败测试。

## 符号链接的设备检查

主机测试只编译并运行共享的解析代码，C 库层必须在手机上验证。`scripts/linux/link-check-build.cjs` 把 `tests/fixtures/linux_links.c` 编成 musl 和 glibc 两个程序（`--reference` 另编一份用未改动 musl 源码的对照）：

```cmd
node scripts/linux/link-check-build.cjs NEW_OUTPUT_DIR NATIVE_SDK --reference .test/linux-tools/musl-source-build/HASH/source
```

把程序送进应用的 home，`chmod 700` 后在终端运行：

```sh
./linux_links_musl DIR            # DIR 不存在；musl 应为 47 项通过
./linux_links_glibc DIR           # glibc 应为 32 项通过（glob、nftw、ftw 跳过）
./linux_links_musl --ab TREE      # 在不含链接的目录树上打印 scandir、glob、nftw、ftw 的结果
HISH_LINKS=0 ./linux_links_musl --ab TREE
./linux_links_musl_orig --ab TREE
```

三份 `--ab` 输出在不含链接的树上必须逐字节相同；树里有目录链接时，开着与关着的输出必须不同，这证明 `HISH_LINKS=0` 真的回到了 C 库自己的函数。系统命令用 `find`、`stat`、`cp`、`mv`、`touch`、`tar` 等在含链接的目录里对照 `HISH_LINKS=0` 的结果。

## epoll 的设备检查

同一个脚本也编出 `linux_epoll_musl` 和 `linux_epoll_glibc`（`tests/fixtures/linux_epoll.c`）。送进应用的 home，`chmod 700` 后在终端运行：

```sh
./linux_epoll_musl                  # 每项 PASS，末行 EPOLL_C_TOTAL pass=N fail=0
./linux_epoll_glibc
HISH_EPOLL=0 ./linux_epoll_musl     # 对照：一次性相关的项必须 FAIL，epoll_pwait2 的项仍 PASS
```

对照那次的失败项证明检查程序分得出开关。空闲 CPU 的验证用普通 Bun：`process.stdout.write` 一次后空闲，看 `top -H` 里每个线程；修正前主线程和 Worker 各约 100%，见 [验收](verification.md)。`tests/fixtures/epoll_probe.c` 是原始探针，直接打印内核对一次性、边沿触发、停用方式的回答，用来判断某台手机的内核本身怎样。

## 手机复验

签名、覆盖安装和设备测试另行授权。HDC 仅传文件、驱动输入、读取取证；每次注入先确认新鲜电源、焦点及终端页面。复杂命令通过文件送入后在可见终端执行。

底座应检查 shell、基础工具、脚本、环境、管道、输入、作业和关闭回收；pkg 另外检查真实下载验签、普通命令执行、升级/卸载/重装、网络失败保留原安装、坏签名/未知公钥拒绝及重启应用。不能借旧全工具链结果声称新包提供了 Git/编译器。状态见 [验收](verification.md)。
