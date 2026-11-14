# QuantumDDraw

自研的最小 `ddraw.dll`：把 DirectDraw 游戏（目标 `gamemd.exe` / 尤里的复仇 + Ares / Mental Omega）的系统内存表面用 **OpenGL 1.1 固定管线** 画到窗口里。
不含第三方代码，只用 Windows SDK + 系统 `opengl32.dll`。x86，`/MT` 静态 CRT，目标 Win7 SP1 → Win11。

DLL 自带探针：每个接口方法都有调用计数，`ddraw.log` 每 5 秒重写一次。

## 编译

需要 VS 2022（v143 工具集，"使用 C++ 的桌面开发"）+ Windows 10/11 SDK。

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1            # Release|Win32 重新生成 + dumpbin 导入表自检
powershell -ExecutionPolicy Bypass -File build.ps1 -Test      # 另外编译并运行 tests\selftest.cpp（52 项检查）
powershell -ExecutionPolicy Bypass -File build.ps1 -Deploy    # 另外把 ddraw.dll / ddraw.ini 复制进 game\
```

产物：`build\ddraw.dll`（同时复制到根目录 `ddraw.dll`），导入表写入 `imports.txt`。
导入表自检只允许 `kernel32 / user32 / gdi32 / opengl32 / advapi32`，否则构建失败。
工程开启 `/W4 /WX`（警告即错误），定义 `_WIN32_WINNT=WINVER=0x0601`，链接器子系统版本 6.01。

## 安装

把 `ddraw.dll`（可选再加 `ddraw.ini`）放进游戏根目录，替换原来的 ddraw.dll。
Mental Omega 必须用 Syringe 带 spawner 启动（不带 `-SPAWN` 时原版 cnc-ddraw 也会以退出码 1 结束）：

```
Syringe.exe "gamemd.exe" -SPAWN -CD -SPEEDCONTROL -LOG
```

## ddraw.ini（`[ddraw]` 节，全部可省略）

| 键 | 默认 | 含义 |
|---|---|---|
| `windowed` | `true` | 游戏要求独占全屏（`DDSCL_EXCLUSIVE`）时：`true` = 在窗口里模拟、不改分辨率；`false` = 用 `ChangeDisplaySettings` 真切分辨率（退出 / Alt-Tab 时恢复；失败则回退为窗口） |
| `client` | 空 | 标记：`Entropy-DDraw` = 这是我们的 ini（模板自带）。没有这个标记的 ddraw.ini（cnc-ddraw 等留下的）里的 `noactivateapp=false` 被忽略，保持 `true`；日志 `config: client=…` 写明。客户端遇到没有标记的游戏目录 ddraw.ini 会按模板重新生成（旧文件存为 `ddraw.ini.replaced.bak`） |
| `windowscale` | `0` | 模拟窗口的初始客户区倍数；0 = 能放进工作区的最大整数倍 |
| `scale` | `aspect` | 窗口被拉伸时图像怎么填：`stretch` / `aspect` / `integer` |
| `linear` | `false` | 非整数倍缩放时用 `GL_LINEAR`（默认最近邻） |
| `vsync` | `false` | 通过 `wglGetProcAddress("wglSwapIntervalEXT")` 设置；取不到就跳过 |
| `maxfps` | `0` | 呈现上限；0 = 只要游戏改了画面就呈现 |
| `hookmouse` | `true` | 模拟全屏时重映射光标/坐标 API（见下文） |
| `dpi` | `permonitor` | 加载时声明的 DPI 感知（新 API 全部动态获取）：`permonitor` = 每显示器 v2（各显示器都是真实像素；移到缩放不同的显示器时客户区像素尺寸不变，只换边框）；`system` = 所有显示器用一个 DPI；`off` = 不声明（Windows 按位图拉伸窗口）。重启生效，日志写实际生效的感知方式和系统 / 显示器 / 窗口 DPI。旧键 `dpiaware=false` 等同 `off` |
| `resizable` | `true` | 模拟窗口可调大小 / 最大化 |
| `noactivateapp` | `false` | 模拟全屏时不让游戏知道窗口失焦（后台继续运行，不自动暂停） |
| `log` | `true` | 写 `ddraw.log` |
| `bpp` | `16` | `SetDisplayMode` 之前报告的桌面色深 |
| `pollprimary` | `true` | 把主表面可见区域与影子副本逐行比较，上传变化的行带。画面在变时约 100 Hz，0.5 秒没变化后降到约 30 Hz（Lock/Blt 仍立即唤醒）。QEC 的 Entropy.exe 会在 Unlock 之后继续写主表面（不经 Lock/Blt），关掉它在 QEC 下画面不更新 |
| `normalmode` | `true` | `DDSCL_NORMAL` 的游戏（QEC）：把游戏窗口客户区当作“屏幕”——主表面 = 客户区大小，坐标 API 全部按客户区相对值回答（同独占全屏模拟）。画面与窗口位置无关，窗口可以随意拖动。`false` = 旧行为（桌面大小的主表面，按屏幕坐标取图） |
| `presentsync` | `false` | `true` = 游戏对主表面的绘制调用（Blt / Unlock）停顿 `presentquiet` ms（最多 `presentcap` ms）后才呈现，一帧呈现一次；`presentquiet` / `presentcap` 只在它（或 `writewatch`）开启时生效。**注意：这种合并在绘制节奏不规则、成批绘制的游戏上会造成帧间隔不均（QEC 因此卡顿，Q163 起默认关）**；开启时 ddraw.log 每 10 秒写一行 `present intervals` 的 p50/p90/p99/最大值 |
| `pbo` | `true` | 有像素缓冲对象（GL 1.5 / `ARB_pixel_buffer_object`）时经 PBO 异步上传；取不到函数自动退回同步 `glTexSubImage2D` |
| `border` | `true` | 窗口状态保留标题栏/边框；`false` = 无边框窗口（`WS_POPUP\|WS_THICKFRAME` + 自己的 `WM_NCCALCSIZE`：整个窗口都是客户区；顶部 6 px 拖动，其余边/角 6–16 px 改尺寸）。含义与 cnc-ddraw 的 `border` 相同（客户端旧 ini 里的 `border=True` 表示“有边框”） |
| `fullscreen` | `false` | 启动时就用无边框全屏（窗口铺满所在显示器，不改分辨率）；`savesettings` 记住的状态优先 |
| `refresh_rate` | `0` | `windowed=false` 真切分辨率时的刷新率（Hz）；0 = 驱动默认。该刷新率不被接受时自动用默认刷新率再试 |
| `keytogglefullscreen` | `0x0D` | Alt + 该虚拟键：窗口 ↔ 全屏（`windowed=true` 时为无边框全屏，`false` 时为真切分辨率）。0 = 关闭 |
| `keyunlockcursor` | `0x09` | Ctrl + 该虚拟键：解除 / 恢复光标锁定（锁定 = 游戏要求的 `ClipCursor`，全屏时还会把光标限制在游戏画面内）。0 = 关闭 |
| `savesettings` | `true` | 把窗口状态的客户区位置/尺寸和“是否全屏”写进 DLL 旁的 `quantumddraw.state`，下次启动恢复。Q144 起同时记下所在显示器的设备名（`monitor=\\.\DISPLAY1`）和相对它左上角的位置（`rel_left` / `rel_top`），恢复时以它为准：换了主屏（虚拟桌面坐标整体平移）也回到同一台显示器的同一位置；那台显示器不在了就放到主屏的同一相对位置。最后仍不在任何工作区内时拉回最近的显示器 |
| `adjmouse` | `1.0` | 光标锁定时的鼠标速度倍率（0.1–10）。在 `GetCursorPos` 钩子里按相对位移累加并把真实光标放回游戏位置，鼠标消息用同一个位置（不抖）；未锁定时恒为 1:1。cnc-ddraw 的布尔值 `true/false` 按 1.0 处理 |
| `center_cursor_fix` | `true` | 游戏画面出现时和每次窗口/全屏切换后，把光标放到游戏画面中央（仅当游戏在前台） |
| `letterboxoffset` | `true` | 画面有黑边或缩放时（如 2560x1600 屏全屏显示 2560x1440 游戏），游戏子对话框（简报、Esc 菜单的按钮）的鼠标输入跟着画面走：消息钩子把每条鼠标消息的真实位置换算成游戏坐标，交给布局上位于该处的子窗口（或持有捕获的窗口），并换成它的客户坐标。子窗口本身的位置不动（游戏从布局读回位置来画对话框）。画面 1:1 贴在客户区原点时不介入。游戏放置的顶层弹窗也定位到画面内 |
| `fix_alt_key_stuck` | `false` | 失焦 / 回到前台时：`false` = 游戏收到过 Alt 按下却没收到抬起时补发一个 Alt 抬起（`WM_KEYUP`）；`true` = 另外把线程键盘状态里实际已松开的 Alt / Ctrl / Shift 清掉（`SetKeyboardState`），并补发这三个键的抬起 |
| `fix_not_responding` | `false` | 游戏线程在我们的绘制调用（Lock / Blt / BltFast / Flip）里、且 1 秒没取过消息时：`false` = `PeekMessage(PM_NOREMOVE\|PM_QS_INPUT)` 一次（Windows 就不会判定“未响应”）；`true` = 取出并分派一条窗口消息。呈现线程每秒检查一次：游戏线程 2 秒没有绘制调用且 250 ms 内不回应 `WM_NULL` 记为“long frame”，`IsHungAppWindow` 为真记为 “NOT RESPONDING” |
| `game_handles_close` | `false` | `true` = 关闭按钮 / Alt+F4 / `WM_CLOSE` 完全交给游戏。`false` = 先让游戏自己处理（QEC 点 × 或 Alt+F4 会自己退出）；0.4 秒后若游戏弹出了新的对话框（询问 / 存档界面）就什么都不做，否则向游戏线程投递 `WM_QUIT`，点击后 3 秒进程还在则恢复显示模式、保存窗口状态后结束进程（退出码 0） |
| `flipclear` | `false` | `Flip` 后把新的后备缓冲清黑，`SetDisplayMode` 后清空主表面和后备缓冲，`Restore` 清空该表面；之后都整幅重传。`IsLost` 恒为 `DD_OK`（表面在系统内存里，从不丢失） |
| `renderer` | `auto` | `auto` = OpenGL；建上下文失败（`ChoosePixelFormat` / `SetPixelFormat` / `wglCreateContext` / `wglMakeCurrent`），或只有微软软件实现 “GDI Generic”（没装显卡驱动、远程桌面）时退到 GDI，日志写明原因。`opengl` = 只用 OpenGL（失败就没有画面）。`gdi` = 不建 GL 上下文，`StretchDIBits` 直接画到窗口 DC（16 位按 5-6-5 位域、8 位带调色板颜色表；黑边单独填，缩放与 `scale` 相同，`linear=true` 时用 `HALFTONE`） |
| `crashdump` | `true` | 崩溃取证（见下文“崩溃报告”）：`true` = 栈、数据段、句柄、寄存器/栈指向的内存（QEC 约 13 MB）；`full` = 整个进程内存（QEC 约 500 MB）；`false` = 只写文字报告。只保留最新 5 份 |
| `center_window` | `false` | 窗口态启动时把窗口（含边框）放到游戏创建窗口时所在显示器（Q144 起不再固定主屏）的工作区正中，尺寸不变；覆盖 `savesettings` 记住的位置 |
| `remove_menu` | `false` | 去掉游戏窗口的菜单栏（`SetMenu(NULL)`），客户区尺寸保持不变 |
| `keyscreenshot` | `0x7B`（F12） | 按下即把当前画面（游戏分辨率，不含黑边/缩放）存为 DLL 目录下 `Screenshots\<日期_时间_毫秒>.png`；内置 PNG 编码器（LZ77 + 固定 Huffman 的 deflate），在后台线程编码；0 = 关闭。按键不传给游戏 |
| `crashtest` | `false` | 调试：首帧呈现 2 秒后呈现线程 `RaiseException(0xE0514444)`（`true`）或写地址 0x10（`av`），用来验证崩溃报告和 dump；进程随即结束 |
| `forceglfail` | `false` | 调试：把 OpenGL 上下文创建当作失败（验证 `renderer=auto` 退到 GDI） |
| `keynextmonitor` | `0` | Ctrl + 该键把窗口 / 全屏移到下一台显示器（按 `EnumDisplayMonitors` 顺序循环）。窗口态保持相对工作区的位置（放不下就拉进工作区），全屏铺满新显示器，独占模式在旧显示器恢复、在新显示器切换。0 = 关闭 |
| `resolutions` | 空 | `EnumDisplayModes` 只报这些尺寸，逗号分隔，如 `2560x1440,1920x1080`；空 = 全部。`EnumDisplayModes` 报的是游戏窗口所在显示器的**真实模式**（`EnumDisplaySettings` 的尺寸 + 刷新率），每个都有 8 / 16 / 32 bpp；不带 `DDEDM_REFRESHRATES` 时每个尺寸报一次、刷新率 0（与 DirectDraw 相同） |
| `max_resolutions` | 空 | `WxH` = 不报比它大的尺寸；`N` = 只报最大的 N 个尺寸；空 / 0 = 不限 |
| `guard_lines` | `false` | 每个表面前后各多分配一行哨兵（0xFD），`Lock` / `Unlock` / `Release` 时检查，被改写就记 `guard_lines ALARM`（哪个表面、前 / 后、改了几个字节、在第几个字节）并修复。`GetDC` 画图时视口下移一行并裁剪到表面内，GDI 不会碰到哨兵 |
| `maxgameticks` | `0` | 游戏线程两次读时钟（`QueryPerformanceCounter` / `timeGetTime` / `GetTickCount` / `GetTickCount64`，只钩游戏模块的导入表）之间最多前进这么多毫秒，卡顿之后不“快进”；被扣掉的时间四个时钟共用一个偏移（彼此一致、不倒退），每次钳制都记日志。0 = 不限。注意：YR 的 60 Hz 逻辑节拍来自 `timeSetEvent` 回调，不受它影响 |
| `monitortest` | 空 | 调试：在桌面右侧加一台**模拟**显示器（如 `1920x1080`，句柄 `7E570001`，设备名 `\.\QDDTEST1`），只存在于本 DLL 的显示器列表里，用于单显示器机器上走多显示器代码路径 |
| `guardtest` | `false` | 调试（需 `guard_lines=true`）：第 300 / 600 / 900 次 `Unlock` 故意越界写，验证报警 |
| `stalltest` | `0` | 调试：首次绘制调用后 20 / 40 / 60 s，把游戏线程在绘制调用里卡这么多毫秒（验证 `maxgameticks`） |
| `mouseprobe` | `false` | 调试：记录 `GetCursorPos` 钩子给游戏的坐标（真实位置 → 客户区 → 视口 → 游戏坐标）和游戏线程的键盘消息 |
| `vsynctest` | `false` | 调试：呈现线程不等待、连续呈现（测量帧间隔 / 验证 vsync） |
| `dump` | `false` | 调试：每 5 秒把主表面和最近一次被 Lock 的离屏表面写成 `ddraw_primary.bmp` / `ddraw_lastlock.bmp`（与 ddraw.log 同目录） |
| `trace` | `false` | 调试：记录游戏模块的 GetDC / BeginPaint / BitBlt / StretchDIBits / CreateDialog… 调用（前 24 次 + 每 500 次）和每种不同的 Blt→主表面矩形 |

## 架构

```
src/qdd.h          共用声明；QDD_METHODS 宏表 = 每个方法的名字 + 是否真实实现（日志里的 impl 列）
src/ddraw_main.cpp DllMain、导出函数、IDirectDraw/2/4/7 对象（3 个接口指针共用一个对象）
src/surface.cpp    IDirectDrawSurface v1..v7（一张超集 vtable；v1..v4 都是 v7 的前缀）
src/palclip.cpp    IDirectDrawPalette、IDirectDrawClipper
src/render.cpp     呈现线程：主表面脏矩形 → RGBA 暂存 → glTexSubImage2D → 一个四边形 → SwapBuffers
src/window.cpp     子类化游戏窗口、窗口化模拟 / 真全屏、DPI、光标 API 的 IAT 重映射
src/log.cpp        计数器、矩形尺寸直方图、事件日志；每 5 秒落盘
src/config.cpp     ddraw.ini
tests/selftest.cpp 走真实 ddraw.h C++ 接口的自测（游戏不走的路径：8 位调色板、翻转链、色键、重叠/拉伸 Blt、GetDC…）
tools/*.ps1        测试辅助：截图（PrintWindow）、按键/按钮消息（不移动真实鼠标）、菜单退出
```

* **表面**：每个表面是一个自顶向下的 DIB section（系统内存，pitch 4 字节对齐）。`Lock` 返回真实指针，`GetDC` 就是把这个 DIB 选进内存 DC。
* **Blt/BltFast**：CPU 矩形拷贝，带裁剪；同一表面内重叠拷贝按方向逐行处理（目标在下方时自下而上，`memmove` 处理左右重叠；有色键时先拷到临时行）；尺寸不同则最近邻拉伸（同表面时先快照源区域）；`COLORFILL`、源色键、目标色键（含 override）。
* **呈现**：独立线程持有 GL 上下文。主表面的改动（脏矩形 + `pollprimary` 扫描出的行带）先在 `g_lock` 下拷进影子副本，纹理只从影子副本更新——游戏在拷贝之后才写入的像素一定与影子不同，下一次扫描必然发现（Q140 之前是先转换再同步影子，两次读之间的写入会永久丢失 → 静态画面停在旧帧/黑屏）。16 位以 `GL_UNSIGNED_SHORT_5_6_5`、32 位以 `GL_BGRA` 直接从影子上传（不再转 RGBA），可经 PBO；8 位用 256 项调色板表转 RGBA。`WM_PAINT/SIZE/MOVE/SHOWWINDOW/ACTIVATEAPP` 强制整幅重传并呈现（每秒最多 10 次）；没有新内容时 100 ms 呈现一次。每 10 秒在日志写一行 `perf`（各阶段平均/最大耗时、上传 MB/s）。
* **窗口**：`DDSCL_NORMAL` 且 `normalmode=true`（默认）时，按 `SetCooperativeLevel` 当时的客户区大小模拟一个显示模式：主表面 = 客户区大小，坐标 API 按客户区相对值回答，窗口样式不动。游戏的 `WM_MOVING`（QEC 会把窗口夹在屏幕内）不再转给游戏，标题栏的命中测试 / 按下 / `SC_MOVE` 交给 `DefWindowProc`。`normalmode=false` 时主表面等于桌面大小，呈现窗口客户区下面那一块。`DDSCL_EXCLUSIVE` 时主表面 = 显示模式大小，窗口改为带边框窗口居中显示（或真切分辨率）。
* **显示方式（Q141）**：模拟开始后窗口处于三种状态之一——窗口（有边框或 `border=false` 无边框）、无边框全屏（铺满所在显示器，分辨率不变）、真全屏（`ChangeDisplaySettingsEx` 把该显示器切到游戏分辨率 + `refresh_rate`，`CDS_FULLSCREEN`；失去焦点时恢复桌面模式并最小化，回来再切；Alt+Enter 回窗口时恢复；窗口销毁 / 进程退出时恢复）。每次切换都让呈现线程重建纹理并整幅上传（日志 `render: rebuild #n`），按新的客户区算视口。切换期间游戏看不到真实的 `WM_SIZE/WM_MOVE`（报告游戏分辨率、(0,0)）。热键用两个线程钩子：`WH_KEYBOARD`（在游戏 / 扩展自己的键盘处理之前拿走热键的按下和抬起）和 `WH_GETMESSAGE`（Alt 抬起改成 `WM_KEYUP`，免得 `DefWindowProc` 进系统菜单循环；无边框窗口的拖动/改尺寸区点击转成 `WM_NCLBUTTONDOWN`，因为客户区被游戏的子对话框盖着）。
* **子窗口**：`window_attach` 去掉游戏窗口的 `WS_CLIPCHILDREN`（`window_detach` 时恢复）。YR 系游戏在客户区上盖着可见的 `#32770` 子对话框（简报 / 菜单），内容由游戏自己画进主表面；保留 `WS_CLIPCHILDREN` 时 GL 输出在子窗口下面被裁掉，QEC（`DDSCL_NORMAL`）整窗黑屏。
* **最小化**：呈现线程暂停；窗口大小变化时按 `scale` 重新计算视口。

### 光标 / 坐标重映射（`hookmouse`）

在窗口里模拟独占全屏时，游戏仍认为窗口在屏幕 (0,0) 且铺满屏幕。DLL 在启动时改写所有非 Windows 目录下模块（游戏 exe、游戏目录的 DLL、以及从别处注入的 Ares / Phobos / Quantum.dll）对 user32 的导入表项：
`GetCursorPos`、`SetCursorPos`、`ClipCursor`、`ScreenToClient`、`ClientToScreen`、`GetClientRect`、`GetWindowRect`、`MapWindowPoints`、`WindowFromPoint`、`GetSystemMetrics`（SM_CX/CYSCREEN）、`SetWindowPos` / `MoveWindow`（游戏不能移动/改尺寸主窗口；顶层弹窗按游戏坐标偏移）。
只有在模拟全屏生效时才改变行为；改写范围包括游戏窗口的子窗口（YR 的游戏内菜单是 `#32770` 子对话框，游戏按它的屏幕坐标把它画进主表面）。
光标不在视口内时，`GetCursorPos` 返回上一次的视口内位置，避免触发边缘卷屏。窗口内的鼠标消息坐标也会按视口换算。
`ClipCursor`：记住游戏要的矩形，每次窗口移动后按当前位置重新换算；向上扩到标题栏顶（光标能够到标题栏拖窗口；光标在边框/标题栏上时 `GetCursorPos` 报告最近的边缘点，边缘卷屏照常）；拖动期间（`WM_ENTERSIZEMOVE`…`WM_EXITSIZEMOVE`）解除。
**已知限制**：子对话框（游戏内菜单）的按钮点击位置只在 1:1 缩放时与画面完全对齐。

## 已实现接口表

`impl` = 有真实实现；`stub` = 只计数。对有输出指针的 stub 不返回 `DD_OK`，而是返回规范的错误码，避免调用方读到垃圾指针。

| 接口 | 已实现 | stub（返回值） |
|---|---|---|
| 导出 | `DirectDrawCreate`、`DirectDrawCreateEx`（IID_IDirectDraw7）、`DirectDrawCreateClipper`、`DirectDrawEnumerateA/ExA` | `DirectDrawEnumerateW/ExW`（DDERR_UNSUPPORTED，与系统 ddraw 相同） |
| IDirectDraw / 2 / 4 / 7 | QueryInterface、AddRef、Release、CreateClipper、CreatePalette、CreateSurface、EnumDisplayModes（常见分辨率 + 显卡模式 × 8/16/32 bpp）、GetCaps、GetDisplayMode、GetFourCCCodes（0 个）、GetGDISurface、GetMonitorFrequency（60）、RestoreDisplayMode、SetCooperativeLevel、SetDisplayMode（v1 三参 / v2+ 五参）、WaitForVerticalBlank（按 60 Hz 节拍 Sleep）、GetAvailableVidMem、GetDeviceIdentifier | Compact、EnumSurfaces、FlipToGDISurface（DD_OK + 日志）、GetVerticalBlankStatus、RestoreAllSurfaces、TestCooperativeLevel（DD_OK）；DuplicateSurface、GetScanLine、StartModeTest、EvaluateMode（DDERR_UNSUPPORTED）；GetSurfaceFromDC（DDERR_NOTFOUND）；Initialize（DDERR_ALREADYINITIALIZED） |
| IDirectDrawSurface v1–v7 | QueryInterface、AddRef、Release、AddAttachedSurface、Blt、BltFast、DeleteAttachedSurface、EnumAttachedSurfaces、Flip（轮换翻转链）、GetAttachedSurface、GetCaps、GetClipper、GetColorKey、GetDC、ReleaseDC、GetPalette、GetPixelFormat、GetSurfaceDesc、Lock、Unlock、SetClipper、SetColorKey、SetPalette、GetDDInterface | IsLost、Restore、GetBltStatus、GetFlipStatus、BltBatch、AddOverlayDirtyRect、SetOverlayPosition、UpdateOverlay*、EnumOverlayZOrders、PageLock/Unlock、Set/FreePrivateData、GetUniquenessValue、ChangeUniquenessValue、Set/GetPriority、Set/GetLOD（DD_OK）；GetOverlayPosition（DDERR_NOTAOVERLAYSURFACE）；GetPrivateData（DDERR_NOTFOUND）；SetSurfaceDesc（DDERR_UNSUPPORTED）；Initialize（DDERR_ALREADYINITIALIZED） |
| IDirectDrawPalette | QueryInterface、AddRef、Release、GetCaps、GetEntries、SetEntries | Initialize（DDERR_ALREADYINITIALIZED） |
| IDirectDrawClipper | QueryInterface、AddRef、Release、GetClipList（一个矩形：窗口客户区的屏幕坐标）、GetHWnd、SetHWnd | Initialize、IsClipListChanged（FALSE）、SetClipList（DD_OK） |

不支持：Z 缓冲表面（DDERR_UNSUPPORTED）、Direct3D、叠加层（overlay）、gamma 控制（QueryInterface 返回 E_NOINTERFACE 并记录 IID）、调用方提供显存（`DDSD_LPSURFACE` / SetSurfaceDesc）。

## 日志格式（`ddraw.log`，位于 DLL 所在目录）

每 5 秒整份重写一次（另外在 IDirectDraw 释放和 DLL 卸载时各写一次）：

```
QuantumDDraw probe log  (uptime 191.0s, rewritten every 5s)
frames presented: 11515  (62.4 fps last interval)  uploaded px: 8943304704
method                         impl      calls   per-sec      <- 只列调用次数 > 0 的方法；impl=yes/STUB；per-sec 是最近一个间隔的速率
S_Lock                          yes     161608     618.0
...
Blt rect sizes (top 16 of WxH): 501x19=630, ...                 <- 直方图：Blt、BltFast、Lock（非主表面）、Lock(primary)、Blt(COLORFILL)
Blt/BltFast flag bits seen: 0x00000400=12944 0x01000000=12945   <- 出现过的 DDBLT_* / DDBLTFAST_* 位及次数

--- events ---                                                  <- 少量事件：SetCooperativeLevel、SetDisplayMode、CreateSurface（前 400 个）、
[      0.937] SetDisplayMode 1024x768x16 refresh=0 flags=0x0       GL 驱动信息、窗口模式切换、WM_ACTIVATEAPP、不支持的 QI、首个异常…
```

计数都是 `InterlockedIncrement`；直方图和事件用一个临界区；落盘在后台线程完成，所以游戏线程上的开销可以忽略。
`EXCEPTION 0x........ (first chance) at ... = module+offset, thread N, read of/write to/execute at <地址>` 这一行来自向量化异常处理器：在 Syringe（调试器）下 Ares 的崩溃处理不会运行，这是崩溃唯一会留下的痕迹（最多记录 20 条）。后面跟一行 `regs EAX=… EIP=… EFL=…`，以及最多 8 行 `stack [ESP+0x..] 值 = 模块+偏移`（栈顶 32 个字里指向已加载模块的值，通常是返回地址）。

### 崩溃报告（Q143）

致命异常发生时写 `ddraw_crash_<日期_时间>.txt` 和 `.dmp`（DLL 所在目录），报告内容同时追加进 ddraw.log（事件日志末尾留有 16 KB 专用余量，长时间运行后也写得进去）：异常码和名称、出错模块+偏移、线程、访问违例的读/写地址、`EIP/ESP/EBP/EFL`、`EAX..EDI`、按 EBP 链走出的调用帧、栈上指向模块的值、**最近 32 条内部事件**（独立的环形缓冲，`log=false` 时也保留）、dump 的路径/大小/类型/耗时。

触发时机有三条路，都只报告一次：
1. **没有调试器**：未处理异常过滤器。DLL 加载时装上；`DirectDrawCreate` 时再装一次放到最上面，把游戏自己的过滤器接在后面（报告写完后照常交给它）；游戏模块此后调用 `SetUnhandledExceptionFilter` 也只会换掉“后面那个”（导入表钩子）。
2. **有调试器**（QEC 的 `EntropyLoader` 以调试器方式启动 `Entropy.exe`，同 Syringe）：Windows 不再调用未处理异常过滤器，第二次机会直接交给调试器并被它结束进程。所以在向量化处理器（第一次机会）里判断：本线程 SEH 链上除 ntdll/kernel32/kernelbase 外没有任何处理器 = 必定无人处理，立即写报告和 dump；链上有游戏的处理器时也写（只写第一个，报告里注明“游戏可能还会处理它”）。
3. **游戏自己的崩溃处理结束进程**：游戏模块在致命异常发生后 5 秒内、在同一线程上调用 `TerminateProcess(自己)` / `ExitProcess` 时先写报告（导入表钩子）。

dump 由单独线程调用 `MiniDumpWriteDump`（崩溃线程可能已经没有栈空间）；`dbghelp.dll` 只在这一刻从系统目录 `LoadLibrary`，不在导入表里（Windows 7 自带的 dbghelp 6.1 即可；新标志不被接受时依次退到旧的 dump 类型）。
