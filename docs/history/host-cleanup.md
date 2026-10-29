# 历史主机包清理

2026-10-30 上一轮完成备用WebGL/ArkTS探针移出资源/应用模块、NAPI和ptrace残留分支编译期排除，以及只在诊断构建链接框架子进程和hilog依赖。保留源码、旧包及用户文件，未签名安装。

当时包为 `C:/Users/Lee/opencode/tmp/opencode/hish-final-cleanup-1/build-2/phone-daily-unsigned.hap`，31,830,590字节，SHA256 `fc14af4bdfc65d59ba86fcbb493c923ac8d6438c8d8a33175f173ae9e67104dd`。规则为36个库、16个rawfile、18个ArkTS来源，28个运行库/工具可执行段匹配；主机48+27+3项和diff检查通过，暂存仍仅libvncserver删除。

该包仍包含Git/curl/GNU/TinyCC。上一轮“无残留”仅指当时排除清单，**不满足后来明确的最小底座交付目标**。新边界见 [软件交付](../distribution.md)。

## 检查器误报

- build-1曾拒绝SDK生成的sourceMaps.map。核对后只允许固定位置、无嵌入源码且匹配当前模块；没有允许任意map。
- 扩大运行库比较时，SDK strip改了RX段内的ELF节表元数据，指令没变；只规范化三个字段并补正反例。
- 第一次final-proof要求WebGL备份原始摘要相等，因CRLF/末尾换行失败。源码规范化后全文相同；报告明确byteIdentical=false，不称逐字节保留。

最终证据为上述build-2下 `final-proof-2/proof.json`；首个final-proof保留失败说明。前次文档只是整篇搬目录并留有通用入口文件，没有完成用户要求的主题整理。本轮重新分配正文，原文快照的保留位置见 [仓库维护](../repository.md)。
