# 主机构建

在仓库根目录执行。使用 Node.js 24、HarmonyOS Command Line Tools 26 和 JDK 21；target API 26，最低 API 24，保留 arm64-v8a/x86_64。

## 首次准备

以下步骤安装项目依赖、下载平台构建输入；先检查脚本及写入范围并取得授权。已有配置不可覆盖。

```cmd
if not exist build-profile.json5 copy build-profile.template.json5 build-profile.json5
ohpm install
node scripts/linux/prepare.cjs
node scripts/packages/prepare.cjs
```

运行库准备只取 musl/glibc 的构建输入和 glibc unwinder，不准备 Git/curl、TinyCC、BusyBox/dash 或 GNU 工具应用。缓存位于 `.test/linux-tools`，来源记录为 `base-provenance.json`；旧输入保留。包管理器准备另在 `.test/packages` 下载固定摘要的 APK 自举程序、依赖、公钥及 CA；不预装目录内的用户应用。

## 未签名底座包

在当前进程设置 JDK 21，输出目录必须是尚不存在的仓库外目录：

```cmd
node scripts/build-daily.cjs C:\path\to\command-line-tools C:\new-evidence-directory
```

脚本清除诊断/候选环境，准备并校验包管理器自举数据，构建独立运行库代次，重生成已有两 ABI CMake 配置，再调用官方 Hvigor `assembleHap --no-daemon`。实际 HAP 检查通过后才复制包并写 `package-verification.json`。不签名、不安装、不碰手机或启动服务。

## 构建一致性

全部编译和清单成功后才更新 `runtime-current.cmake`。CMake 校验输入及产物 SHA256；不匹配即失败，不退回旧 payload。运行库代次仍只有六个通用库，没有用户应用或 compiler-data。APK 的 CA、依赖和公钥属于独立自举数据，HAP 检查逐字节匹配压缩数据。

旧生成输出中的明确退役应用/依赖按摘要移到 `.cxx/.../excluded-diagnostics`，不删除原字节。未知额外库使 HAP 检查失败，不以通配符放行。

## 保留实验配方

`prepare.cjs --diagnostics` 和 `build.cjs NATIVE_SDK --diagnostics` 保留旧软件/探针配方，用于单独实验，不是发行路径。旧 UI 探针钩子位于 `tests/fixtures/diagnostics`；仅开启原生宏不会恢复 UI 调度。不要借旧测试将普通应用重新塞入底座。
