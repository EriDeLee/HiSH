# 内置包管理器

## 分发方案

`pkg` 接入 Alpine v3.23 官方 `main`、`community` 的 aarch64 仓库，复用未修改的 APK/libapk 3.0.8 解析依赖、下载并验签。没有包名或依赖白名单：有效仓库包名均可请求，搜索查询上游索引。

Bun 使用官方 npm 的 `@oven/bun-linux-aarch64-musl`；版本、URL 和 SHA512 固定在 `scripts/packages/upstream.json`。PRoot 使用固定上游源码的 HiSH 平台端口，见 `scripts/packages/proot-lock.json`、`proot-build.cjs` 及 `proot/`。它是可复现的源码适配构建，不是声称原版 Alpine PRoot 可直接运行。两者的固定版本由 HAP 更新，`pkg info` 显示来源和版本。

原版 `apk add --usermode` 在此设备上会因软链接创建及 O_TMPFILE 发布失败而中止。管理器调用 APK 的 fetch/verify/search/policy/update，由应用安装器写入私有目录；兼容层让 O_TMPFILE 提前返回 EOPNOTSUPP，使用上游命名临时文件回退。

## 使用

```sh
pkg search tree
pkg update
pkg info bun proot
pkg install tree jq zstd bun proot
tree .
bun -e 'console.log(6*7)'
proot -r /absolute/path/to/rootfs -w / /bin/sh
pkg doctor
pkg list
pkg upgrade
pkg remove tree
```

安装资格和兼容验证分开：安装成功代表依赖解析、来源验证和解包完成，不等于应用所有功能均可用。`doctor` 对 jq、zstd、Bun、PRoot 执行内置功能探针，其余包明确显示没有内置探针，不阻止安装。版本输出不代替功能断言。

`upgrade` 对整个请求集合重新解析 Alpine 最新版本。Bun/PRoot 重新安装当前 HAP 固定版本，不伪造已经更新到上游最新版。x86_64 构建仍明确提示包分发需要 ARM64 设备。

## 安装和恢复

- 软件位于 `native/packages/root`；请求集合、版本及来源位于 `.hish-package`。自举为 `native/packages/bootstrap-摘要`。保留已有 HOME、配置和用户文件。
- 每份 APK 用上游公钥验签，接受 aarch64/noarch；包名只做格式校验，防止选项、路径或 URL 被当作名称。没有 allow-untrusted 或关闭证书验证的透传。
- Bun 下载使用自举 libfetch/OpenSSL，固定 CA、校验证书与主机名，再校验完整归档 SHA512。摘要缓存位于 `native/packages/cache`，每次复用重新校验。
- 新代次在 `.transaction-*` 下解包，验证 gzip/tar、路径、包元数据及架构。APK 模式必须有 PKGINFO；Bun 的 tgz 模式仅在完整性校验后使用。
- 不执行 APK 安装脚本、触发器和服务操作。运行时依赖这些系统动作的软件需要进一步适配，此限制不以包名白名单实现。
- 文件软/硬链接在所有包提取后转为同根目录内的普通文件副本；保留目标字节和执行权限。拒绝逃逸、循环/无目标链接、设备及文件冲突。目录链接暂不支持。
- 每份归档限制为压缩64MiB、解压128MiB；目前支持 gzip/tar APK。其他格式和超限输入明确失败，不激活半成品。
- flock 排他操作；旧代次保留为 `previous`。切换失败恢复旧目录；两次重命名之间中断时，下次 pkg 恢复。没有断电持久性或与运行中应用升级完全无缝的保证。
- PATH 优先软件目录，避免旧底座转发脚本遮住新安装的同名程序。Linux 库搜索通用包含软件目录，不把每个仓库应用登记成原生 applet。

## PRoot 适配

原版 PRoot 的临时 loader 内核 execve 在真机返回 EACCES13。端口保留 PRoot 的路径/系统调用跟踪，改用允许执行的系统 Toybox 和现有 HiSH 引导库。PRoot 将有界装载脚本放在保留的环境区，保持内核初始栈交给 OHOS 链接器，再由平台适配器映射真实目标段并转入来宾。

代码映射沿用 RW、复制、RX 路线，拒绝 W+X、可执行栈、段重叠和地址冲突，不授予内核 root/chroot。来宾 exec 交给 PRoot，避免再次进入普通执行桥。seccomp 加速关闭，仍使用 ptrace 跟踪。

端口自动透传运行所需的 `/system`、HiSH 原生库目录及 OHOS 链接器 namespace 配置；它不是安全隔离边界。rootfs、绑定路径和工作目录由调用者选择。PRoot 包含 talloc 依赖；HAP 自举保存按需安装的端口产物和探针，不自带发行版 rootfs。

已测 ARM64 musl rootfs 的文件读取、shell、子命令与 fork/exec。glibc rootfs、任意静态程序、安装器、systemd 和完整发行版生命周期不由这些测试证明。

## 来源与验证

自举输入见 `bootstrap-lock.json`；PRoot 源码、uthash 和构建头文件逐份校验，编译缓存校验指纹及产物摘要。源代码改动在构建脚本中使用唯一锚点，漂移即失败。`licenses/packages.txt` 保留许可和源码入口。

`node --test tests/package-manager.cjs` 用已有 g++/zlib 执行真实 C++ 解包器，覆盖路径、链接、gzip/tar 损坏、截断、APK元数据、上游tgz模式和输入篡改。真机结果及精确包信息见 [验收](verification.md)。
