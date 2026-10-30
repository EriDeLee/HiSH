# 当前底座组件来源

## Shell 与基础工具

| 组件 | 固定来源 | 许可位置 |
| --- | --- | --- |
| OpenHarmony mksh | `88b0a4a12f2c219c1421ce5a3b90b97b785a08ad` | `third_party/mksh/COPYRIGHT`、`TaC-mksh.txt`，完整条款随 rawfile 交付 |
| Toybox 工具子集 | `6a0c0ef5eeb2c43e4feef9781398c1718b8bddcb` | `third_party/toybox/LICENSE` |
| xterm.js / fit | 保留的上游代码与署名 | 应用开源许可页 |
| 字体 | 保留的三款字体资源 | `rawfile/term/fonts/LICENSE.txt` |

mksh 的 51 份上游文件由 `hish-source-manifest.json` 固定，CMake 逐份校验。适配位于应用的 `mksh_adapter.c` 和平台编译定义，上游字节不改。Toybox 使用构建目录的命令表覆盖，不运行上游构建脚本。

## 平台运行库

musl 1.2.5 上游归档及 Alpine aports 提交 `8aef0c37b0ad23dc4137f0e4755b97a59dc698b8` 的九份 AArch64 相关补丁由 `musl-source.cjs` 固定 SHA512。1349 编译单元使用已有 SDK LLVM；来源、输入摘要、完整命令、COPYRIGHT 与产物记录在独立缓存代次。

glibc 构建输入来自 Ubuntu ARM64 libc6/libc6-dev `2.39-0ubuntu8.9`；固定 URL 和 SHA256 见 `scripts/linux/prepare.cjs`。其平台适配见 [执行兼容层](elf-runtime.md)，不能称为未修改 glibc。

栈展开库为 Ubuntu `libgcc-s1` `14.2.0-4ubuntu2~24.04.1`，GPL-3.0 with GCC Runtime Library Exception 3.1，完整许可保留在 `third_party/gcc-runtime-LICENSE.txt`，对应 [gcc-14 源包](https://launchpad.net/ubuntu/+source/gcc-14/14.2.0-4ubuntu2~24.04.1)。这不是随包编译器。

正式公开分发仍须完成对应源码、修改和许可交付审查；本地构建通过不等于发布合规认证。

## 包管理器自举

APK/libapk 3.0.8、OpenSSL 3.5.9、zlib 1.3.2、Alpine 公钥及 CA bundle 来自 Alpine v3.23 main/aarch64。固定 URL、SHA256 和所选文件见 `scripts/packages/bootstrap-lock.json`。未修改 APK 可执行文件或库字节；只将所需文件放入自举私有布局。GPL-2.0-only、libfetch BSD、Apache-2.0、Zlib、MPL-2.0 许可及源码入口随包 `licenses/packages.txt` 交付。

普通 jq/zstd 及其依赖不在 HAP 中。手机安装时验证上游签名，保留原始 PKGINFO。具体行为见 [包管理器](packages.md)。

PRoot 平台端口基于 `25dc6a3134891f98a79f57ce1c2c1b23ff15cad1`，GPL-2.0-or-later；按需安装的编译产物和探针位于自举。固定归档、uthash、talloc/构建头文件摘要见 `scripts/packages/proot-lock.json`，补丁逻辑及适配源文件见同目录构建脚本和 `proot/`。talloc 动态依赖由 Alpine 安装，未静态链接进端口。

## 备用但不交付

mrsh 源码及 MIT 许可、原生探针和实验加载器保留。闲置 WebGL 插件位于 `third_party/xterm-optional/`；与 Git 原件在规范化换行后全文相同，原始摘要不同。

Git/curl、TinyCC 和 GNU 工具的旧配方、包摘要和缓存保留，版本与历史变更见 [应用实验](history/application-tests.md)。它们不属于上表当前底座，不因保留来源说明而重新交付。
