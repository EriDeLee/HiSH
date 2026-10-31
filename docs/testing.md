# 测试

## 主机回归

从仓库根执行；测试使用已有 SDK 和缓存，在 `.test` 写隔离输出，不安装工具：

```cmd
node --test tests/mksh-integration.cjs tests/daily-packaging.cjs tests/package-inventory.cjs tests/signed-packaging.cjs tests/base-distribution.cjs tests/repository-hygiene.cjs tests/source-runtime.cjs tests/daily-appearance.cjs tests/musl-source.cjs tests/package-manager.cjs tests/package-core.cjs tests/elf-dormant-heap.cjs
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

## 手机复验

签名、覆盖安装和设备测试另行授权。HDC 仅传文件、驱动输入、读取取证；每次注入先确认新鲜电源、焦点及终端页面。复杂命令通过文件送入后在可见终端执行。

底座应检查 shell、基础工具、脚本、环境、管道、输入、作业和关闭回收；pkg 另外检查真实下载验签、普通命令执行、升级/卸载/重装、网络失败保留原安装、坏签名/未知公钥拒绝及重启应用。不能借旧全工具链结果声称新包提供了 Git/编译器。状态见 [验收](verification.md)。
