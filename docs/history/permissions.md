# 历史权限对照

这里只归档权限实验的结论和证据，不提供新授权。当前安装包及新候选分别见 [验收](../verification.md)。

## 三项代码内存权限与两个附加权限

2026-10-28 曾同时测试 ALLOW_WRITABLE_CODE_MEMORY、ALLOW_EXECUTABLE_FORT_MEMORY、DISABLE_CODE_MEMORY_PROTECTION。匿名 RW→RX/RWX 和受控内存编译运行成功；未做单项消融，不能分别归因。

第二个 Profile 添加 ALLOW_EXTERNAL_NATIVE_CODE，安装成功但状态 -1；`atm dump -d -p` 报权限不存在。SDK 声明存在不代表设备固件定义存在，也不能由此判断用户申请错误。证据根目录：`C:/Users/Lee/opencode/tmp/opencode/hish-external-1/`。

第三个 Profile 的 CUSTOM_SANDBOX 定义存在、状态 0；文件执行及 HOME/memfd 执行映射仍拒绝，内存编译和七组回归通过。设备为 `FMR0223825041853`、UID20020268、debug_hap、HongMeng Kernel1.13.0、OpenHarmony-7.0.0.111。证据根目录：`hish-sandbox-1/`。没有读取 Profile 或另加独立 bin 签名流程。

## 动态沙箱机制核对

已读 OpenHarmony 上游实现：系统在创建启动消息时设置 CUSTOM_SANDBOX 标志，但同时检查设备条件和实际权限；不是应用缺一个公开“开启”调用。

- [CreateStartMsg / SetStartMsgCustomSandboxFlag](https://github.com/openharmony/ability_ability_runtime/blob/a0047a48162ef9b18dd403c14925058d506c3d0f/services/appmgr/src/app_mgr_service_inner.cpp#L6083-L6147)
- [IsStartOptionsWithAnimation](https://github.com/openharmony/ability_ability_runtime/blob/a0047a48162ef9b18dd403c14925058d506c3d0f/services/common/src/app_utils.cpp#L293-L301)：读取系统参数，默认 false；本机读取失败，不等于读到 false。
- [VerifyCustomSandbox](https://github.com/openharmony/ability_ability_runtime/blob/a0047a48162ef9b18dd403c14925058d506c3d0f/services/common/src/permission_verification.cpp#L164-L171)
- [appspawn 条件](https://github.com/openharmony/startup_appspawn/blob/751538e417925348ef212077030f464355da71a4/modules/sandbox/normal/sandbox_core.cpp#L1204-L1235)与[沙箱配置](https://github.com/openharmony/startup_appspawn/blob/751538e417925348ef212077030f464355da71a4/etc/modern/appdata-sandbox-app.json#L550-L562)

上游代码不等于商业固件逐行实现。本机运行时是否选用了动态沙箱未证实，不能写“完整启用但无效”。[API26 bin 权限继承变更](https://developer.huawei.com/consumer/cn/doc/harmonyos-releases/changelogs-for-all-apps-7001#ch2026031760265)也不能推广为本项目所有 fork/内存执行都必须新增签名。

## 零额外 ACL

后来使用普通 Profile，仅声明 INTERNET：覆盖安装及用户报告重启后 15 组复验通过，实际授权仍仅 INTERNET。相同 Profile 声明矩阵仅完成 mask15、0、1，即 3/16；零组合成功不等于矩阵完成，也不能归因真正内核机制。

| 历史阶段 | 包/证据目录（相对 `C:/Users/Lee/opencode/tmp/opencode/`） | 摘要 |
| --- | --- | --- |
| 零 ACL 测试包 | `hish-acl-min/profile-control-mask-0/` | `e8ecbdb38ad20348b971a40ace13dea441da6152beb4ec4fe05cd5b544fc1ab0` |
| 重启后复验 | `hish-acl-min/profile-control-mask-0-after-reboot-verified/` | result、15 组报告及前后授权 |
| 去测试亮屏逻辑的收尾包 | `hish-acl-min/final-zero-acl/` | `8ce385341ac6265da4445e8a2db055be3fee2cc300bb8ee3676ccdcda4575628` |

前两次重启复验分别被锁屏和权限取证空内容挡住，保留失败目录；只有 verified 是通过记录。收尾包通过 15 组但未再次重启。最小额外 ACL=0 只覆盖该设备、调试固件和安装沿革，不包括全新安装、发布域或任意程序。

早期测试曾临时更改息屏后恢复；build-15 的前后值独立记录为 Timeout30000ms、Override104430000ms。中间覆盖值来源未确定，不能把旧记录归给当前任务。本轮没有设置、恢复、唤醒或解锁操作。
