# MacKeyFlow for Windows

一个轻量的 Win32 常驻工具，把几种 macOS 风格的键盘习惯带到 Windows。程序用纯 C 编写，没有运行时依赖，启动后只驻留系统托盘。

![MacKeyFlow 图标](assets/icon.png)

## 功能

| 快捷键 | 行为 | 说明 |
| --- | --- | --- |
| `CapsLock` 短按 | `Ctrl+Space` | 切换输入法，屏蔽原始 CapsLock |
| `CapsLock` 长按（≥400 ms） | CapsLock | 切换大小写，保留键盘指示灯 |
| `Esc` 长按（≥1 s） | `Alt+F4` | 关闭当前窗口，倒计时期间松开即可取消 |
| Win + 反引号（`） | 同程序窗口切换 | 类似 macOS 的 Cmd + 反引号，按可执行文件路径匹配，支持多进程应用 |

短按 `Esc` 会原样传递给当前应用。长按功能有 200 ms 激活延迟，避免误触。

## 使用

双击 `macos-keyflow.exe`。手动启动会请求管理员权限，以便向任务管理器等管理员窗口发送模拟按键。Windows 的 UAC 安全桌面不在此工具的控制范围内。

程序运行后不会显示主窗口。右键托盘图标可以：

- 设置或取消登录时启动；
- 重新安装键盘钩子（按键功能异常时）；
- 打开运行日志；
- 查看项目页面；
- 退出程序。

日志保存在当前用户的临时目录（`%TEMP%`），托盘的“打开日志”会直接打开该目录中的运行日志：

- `%TEMP%\macos-keyflow.log`：启动、初始化失败、输入注入和恢复信息；
- `%TEMP%\macos-keyflow-crash.log`：未处理的原生异常地址和错误码。

启动时会识别旧的 `MyToolKit` 单实例锁，避免两个版本同时拦截键盘。已启用旧版自启时，首次运行新版本会先创建指向新 EXE 的 `MacKeyFlow` 任务，成功后再移除旧任务。项目页面为 https://github.com/qcbf/macos-keyflow 。

自启任务设置为：**无运行时限、允许电池供电时启动、切换到电池不停止**。计划任务启动的程序意外退出后会每分钟重试，最多 3 次；从托盘正常退出不会触发此恢复。直接双击启动的实例不受任务计划恢复机制管理。

## 编译

### MSVC（推荐）

需要 Visual Studio Build Tools 或完整 Visual Studio：

```bat
build.bat
```

脚本会自动查找 Visual Studio C++ 工具链，启用栈保护与严格警告检查，输出 `macos-keyflow.exe`。编译中间文件和用于崩溃定位的 PDB 保存在 `build/`。

运行回归测试：

```bat
build.bat test
```

测试会截获模拟按键，并使用 Windows 任务计划接口解析配置，不修改真实的自启任务。

### MinGW-w64

```bat
windres macos-keyflow.rc -o macos-keyflow.o
gcc -O2 -s -mwindows macos-keyflow.c macos-keyflow.o -o macos-keyflow.exe ^
  -luser32 -lshell32 -ladvapi32 -lgdi32 -ldwmapi -lpsapi -lole32 -loleaut32 -ltaskschd -luuid
```

## 文件

```text
macos-keyflow.c        主逻辑：键盘钩子、热键、托盘和开机启动
macos-keyflow.rc       嵌入图标和 UAC manifest
macos-keyflow.ico      Caps 键帽托盘图标（16～256 像素）
assets/icon.png        图标 PNG 源图
macos-keyflow.manifest UAC 与 Windows 兼容性声明
build.bat              MSVC 一键编译脚本
tests/regression.c     按键状态、线程生命周期与任务配置回归测试
```

## 稳定性设计

低级键盘钩子运行在独立的消息线程中，托盘菜单里的任务计划操作不会阻塞键盘回调。Win + 反引号被其他程序占用时会在后台定时重试。长短按依据真实按键时间判断；Esc 一次持续按住最多关闭一个窗口，前台窗口改变会取消关闭。隐藏的顶层窗口接收 Explorer 重启广播并恢复托盘图标；关键初始化、输入注入和消息投递失败会写入日志。异常记录与 Windows 错误报告同时保留。
