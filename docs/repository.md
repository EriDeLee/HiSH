# 仓库维护

## 文档归属

维护文档只在 `docs`，按职责组织，不设置项目自有 README 或 CLAUDE 文件，也不创建根目录转发文件。第三方原始说明、源码摘要及 LICENSE/COPYRIGHT 保留原位；根目录 LICENSE 是法律文件。

| 原来混在一起的文字 | 现在归属 |
| --- | --- |
| 项目介绍、来源、目标 | [产品](product.md) |
| 随包工具、版本及更新责任 | [交付](distribution.md) |
| fork、作业、回收注意事项 | [进程](process-lifecycle.md) |
| 装载、权限映射和 libc 机制 | [执行兼容层](elf-runtime.md) |
| mksh、脚本和旧入口 | [Shell](shell.md) |
| 操作约束与签名纪律 | [安全](safety.md) |
| 构建命令、测试命令、验收结果 | 各自的 [构建](build.md)、[测试](testing.md)、[验收](verification.md) |
| 旧权限、ptrace、软件实验 | `history` 中对应专题，不复制整篇交接 |

重组前 24 份混合文档的原文快照保留在仓库外 `C:/Users/Lee/opencode/tmp/opencode/hish-base-only-1/document-originals/`。快照只作历史追溯，不作为维护入口或新授权；源码和原始取证未删除。

## Git 忽略

`.gitignore` 排除依赖目录、Hvigor/CMake/测试输出、机器设置、IDE/AI 本地状态、安装包及签名私密文件。`/.hallmark/` 已存在；`git check-ignore -v --no-index -- .hallmark/preflight.json` 命中该规则，普通 Git 状态不列该文件。

保留 `build-profile.template.json5`、锁文件、测试及备用源码、必要生成头文件。不要忽略整个 third_party、bench、所有 JSON/bin 或维护文档。忽略规则不是打包规则，也不会取消跟踪已跟踪文件。

`tests/repository-hygiene.cjs` 验证文档主题、禁用文件名、内部链接及忽略正反例。本轮不改已有 `libvncserver` 暂存删除，不执行 git add、提交或仓库清理。
