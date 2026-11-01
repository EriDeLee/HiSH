# 会话与进程生命周期

```text
WebTerminal → NativeTerminalSession → NAPI 异步任务
  → 应用 fork → 监督进程 → fork → mksh → 命令子进程
```

会话的 HOME、PREFIX、TMPDIR 分别位于应用 `files/native/{home,usr,tmp}`。环境和工作目录只在子进程设置，不修改 UI 进程。保留已有用户文件、历史、字体及背景。

Linux 程序写死的 `/etc/ssl/certs/ca-certificates.crt` 在本机不存在。会话启动时，若 pkg 自举目录里有 CA 包（`native/packages/bootstrap-摘要/etc/ssl/certs/ca-certificates.crt`，即 pkg 校验自己下载所用的那份），就把 `SSL_CERT_FILE`、`CURL_CA_BUNDLE`、`GIT_SSL_CAINFO` 指向它；已设置的值不覆盖。curl 读前两个之一，git 只读 `GIT_SSL_CAINFO`。

Alpine 的 git 到 `/usr` 下找辅助程序和模板，本机没有这个目录。会话启动时，若当前代次有 `usr/libexec/git-core` 和 `usr/share/git-core/templates`，就设置 `GIT_EXEC_PATH` 和 `GIT_TEMPLATE_DIR`，已设置的值不覆盖。`pkg` 函数把本窗口切到新代次时同步更新：值为空或指向旧代次时重设，新代次没有 git 就取消；用户自己设的值不动。

## 通道和输入

原生会话先尝试内核 PTY；已测手机返回拒绝时使用 socket 字节通道和共享终端状态。共享状态保存窗口大小、termios、前台组和输入控制字符；UI 按 VINTR/VQUIT/VSUSP 发送相应信号。它不能替代完整内核行规程或嵌套 PTY。

`NativeTerminalSession.ets` 管理输入背压、分帧、输出尾帧、输出轮询和关闭/启动竞态。`native_terminal.cpp` 管理描述符、信号、会话表和监督循环；NAPI 层不执行应用软件安装。

## 回收责任

监督进程使用 CHILD_SUBREAPER 收养脱离中间父进程的后代。它非阻塞回收自己的已退出子进程；shell 的直接作业仍由 shell 等待。监督等待不能使用 WUNTRACED，否则可能把停止的作业当成退出。

build-13 只等待主 shell，漏收一个 `Zs` 后代；build-15 的十轮收养退出测试及最终快照验证了修复。该结果不是长期无泄漏证明，证据见 [验收](verification.md)。

不要提前关闭并复用继承的服务描述符编号，历史测试因此出现管道失效。PDEATHSIG 绑定创建线程，不能用于跟随会退役的 NAPI 工作线程。会话退出时的清理只能作用于自有进程，操作授权见 [执行纪律](safety.md)。
