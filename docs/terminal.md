# 终端界面

`rawfile/term/term.html` 加载 xterm.js、fit、appearance、native-selection 和 term.js，使用 DOM 渲染。没有默认 WebGL 或 Canvas 回退。备用 WebGL 代码在 `third_party/xterm-optional/`，不在包内资源目录。

```text
键盘 / 虚拟键 / 粘贴 → xterm onData → ArkTS → 原生输入
原生输出字节 → ArrayBuffer → Base64 传输 → Uint8Array → xterm
```

输入输出使用 UTF-8，不解释反斜杠文本来伪造终端输出。ArkWeb 提供选择/复制/粘贴菜单；Web 导航仅允许本地 resource 页面，避免远程页面进入带终端桥的组件。

## 设置和尺寸

保留双排键顺序、32vp 键高、16vp 展开条、既有字号、字体/背景文件及偏好。修饰键显示选中状态；重启前确认丢失未保存内容。不能据此宣称所有触摸目标满足 44vp。

容器、虚拟键、字体及窗口变化合并为一帧 fit，不用相对滚动补偿。首次同步也要应用 false 的光标闪烁设置。纯色背景转换 ArkUI `#AARRGGBB` 后按亮度选默认前景；4096 个 RGB 采样达到 4.5:1 不代表所有 ANSI 配色或图片背景均可读。

## 测试注入与真实输入

UITest 中文输入可能经剪贴板发送 Ctrl+V，mksh 会把它解释为“引用下一字节”。历史驱动用真实 X/退格消耗待引用状态，再点击新鲜 IME 候选；不能修改输出或把输入失真判成 shell 缺陷。

已测 ASCII/中文删除和中间插字、160 字符长行、历史及 Vim 编辑保存。所有字体/窗口组合、系统最大字体、完整无障碍和 emoji 字素尚未验证。具体包证据见 [验收](verification.md)。
