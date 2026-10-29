# 历史 Shell 迁移

## 备用 mrsh

`third_party/mrsh` 固定上游 `4c81598721bc5eeb28f9faa818b3102d0471b7f6`，保留MIT许可、测试和本地移植。旧版加入管道交互、提示符、SIGINT、原生命令/ELF、exec导出同步、EOF/PS2、多行复合命令、作业/前台组和128+signal退出码。

旧exec漏同步导出变量导致原版Vim缺库；回归在 `tests/fixtures/shell_exec_export.sh`。这些旧实现未宣称完整POSIX兼容，现不编入默认包，也不作为mksh失败回退。

## mksh 候选与误判纠正

| 阶段 | 实际问题或结果 |
| --- | --- |
| build-3 | UI注入失真，旧失败现场不能用于后续包结论 |
| build-4/5 | 原生TTY用RTLD_NEXT查写函数失败，ENOSYS38导致无提示符；OHOS分支改直接syscall，Linux分支保留 |
| build-6 | 摘要保护拒绝变更，属于正确失败 |
| build-13 | 功能通过，但快照发现监督进程漏收养退出PID41542/PPID41214，纠正“无残留”结论 |
| build-14 | 两ABI构建通过，手机息屏，签名前停止，未安装 |
| build-15 | 用户解锁确认后安装，完整功能、六组交互、Vim和十轮收养退出独立通过 |

build-15十个测试后代实际交给监督PID39316，最终PID全部消失、应用树无Z；外部作业暂停/jobs/fg仍通过。主机当时24+27+3项，不能代替当前源码回归。包身份及手机证据统一见 [验收](../verification.md)。

早期 mrsh/BusyBox/dash 的版本输出、旧15组探针、旧重启证据及临时权限不移用为新mksh包结果。UI长行水平滚动和中文注入差异归属 [Shell](../shell.md) 与 [终端](../terminal.md)，不再混入包交付说明。
