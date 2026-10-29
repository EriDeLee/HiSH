# 历史应用与打包实验

本页只记录旧版本，不代表当前底座提供这些应用。交付责任已经改为 [独立软件包](../distribution.md)。

## 旧版本和局部改动

| 应用/依赖 | 旧固定版本及许可 | 旧适配 |
| --- | --- | --- |
| Git | 2.52.0，GPL-2.0-only | HAP改名、依赖名、helper固定路由、空模板 |
| curl/libcurl | 8.22.0，curl | HAP依赖名及CA环境 |
| OpenSSL | 3.5.9，Apache-2.0 | 网络依赖闭包 |
| c-ares/Brotli | 1.34.8/1.2.0，MIT | 同上 |
| libidn2/libunistring | 2.3.8/1.4.1，GPL或LGPL双选 | 同上 |
| nghttp2/libpsl | 1.69.0/0.21.5，MIT | 同上 |
| zlib/Zstandard | 1.3.2/1.5.7，Zlib及BSD或GPL | 同上 |
| Expat/PCRE2 | 2.9.0/10.49，MIT/BSD-3-Clause | 同上 |
| CA包 | 20260909，MPL-2.0 AND MIT | 原子写入私有share目录，设置curl/Git证书变量 |
| TinyCC | `5fad6dc4a3cdb7c9094129a4c76e5d2599e94414`，LGPL-2.1-or-later | CRT/include路径、默认未指定ET_EXEC地址0x60000000 |

原 URL/SHA256、Alpine配方和许可证仍保留在 `scripts/linux/network-packages.json`、`compiler-packages.json`、缓存及来源记录中。旧配方未运行软件包安装脚本。TinyCC上游源码不改；对象、共享库、-run及显式地址不采用上述默认地址。旧网络依赖名适配不是未来软件包方案。

编译器数据曾包含匹配的头文件、CRT、libtcc1和musl静态库，压缩随包，版本变化时展开；它不赋予代码RX权限。默认带libc的 `tcc -run` 仍失败；临时 `INPUT ( )` 链接脚本复用宿主符号的实验与普通编译后执行是不同路径，静态libc内存对照139未解决。证据根目录 `hish-jit-3/final/`，不是当前默认功能。

## 原版下载程序

2026-10-28电脑下载20条记录后部署到手机，先核对摘要，再运行；不是手机下载或自动依赖安装。目标原程序/原版DSO没有重编或修改。

| 原程序 | 首轮 | 源码musl修复后 |
| --- | --- | --- |
| 静态jq1.8.1 | 默认126/File exists，ash条件下42 | 固定地址问题仍在 |
| xxHash0.8.3-r0 | 默认入口及正反校验通过 | 保持通过 |
| mawk1.3.4.20240123 | 127，缺libm.so.6 | 仍未补库 |
| 动态jq1.8.2-r0 | 找不到libjq/libonig | 原版HOME依赖下算出42 |
| zstd/pzstd1.5.7-r2 | 缺libzstd/C++/GCC依赖 | zstd两线程、2MiB逐字节还原；pzstd只证明压缩 |
| Vim9.2.1202-r0 | 缺pixman/ncurses，未进入编辑 | 原版批处理及交互编辑保存通过 |

严格默认入口功能从1/6变为4/6，不是6/6或任意ELF兼容。初轮证据 `hish-download-abi-1/device-1/`；源码候选最终证据 `hish-library-source-1/device-6/`，根目录均在 `C:/Users/Lee/opencode/tmp/opencode/`。

## musl DSO 修复的实际失败顺序

1. device-1仍用了旧缓存；其15组通过不属于源码loader。
2. device-2新loader进入包，但校验进程159，未运行后续功能。
3. device-3定位同身份setgid144被拒绝；调整预加载符号顺序，其他身份不授予。
4. device-4的bridge重定向被DSO日志污染，不能放宽精确13字节断言；收集误判0/15与13/15纠正为14/15。
5. device-5原版Vim首个致命调用为setfsgid151，后续kill是次生退出。
6. device-6补查询/保持原文件访问身份，分离DSO诊断开关；原包4/6及15组回归通过。

device-6 signed HAP SHA256 `a6418ca7082f8184ee73cb2ccb691ae1b09e7373d179e9f95712d76c6181123c`，loader SHA256 `6d0b82ec5041d3dfa07bb1c2e030439bb770c104c5dce3a932414a37780ae8e8`。最终读回15份原程序/依赖及terminfo摘要相同，Vim文件精确 `VIM_INTERACTIVE_OK\n` 19字节。原HOME测试目录 `.hish-download-abi-OMeIKj` 保留。

## 网络证据的范围

旧包验证过example.com HTTPS、无效CA拒绝、Gitee公开引用与浅克隆；早期GitHub证书链不受信任，未关闭校验或回退HTTP。Windows下载不代表手机代理/TUN已覆盖HiSH。软件源、包管理器和自动依赖更新没有因这些实验而完成。
