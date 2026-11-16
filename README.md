<p align="center">
  <img src="assets/app_1024.png" width="128" alt="BiliFetch icon">
</p>

<h1 align="center">BiliFetch</h1>

<p align="center">
  <b>B站内容抓取 Windows 桌面应用（Qt6 + bili-digest）</b><br>
  一个输入框通吃 视频 / 专栏 / 动态 / UP主 / 关键词 · 官方 AI 总结与逐句字幕 ·
  评论树与楼中楼 · CSV/Markdown 导出 · 批量队列 · 扫码登录
</p>

<p align="center">
  <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white">
  <img alt="Qt6" src="https://img.shields.io/badge/Qt-6.11-41CD52?logo=qt&logoColor=white">
  <img alt="Python" src="https://img.shields.io/badge/Python-3.10%2B-3776AB?logo=python&logoColor=white">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-green">
</p>

结合 **bili-digest** 插件开发的 Windows 端 GUI：C++/Qt6 负责界面与交互，
Python 负责已验证的抓取管线（WBI 签名、buvid 指纹穿透、评论翻页、登录态），
两者通过 **子进程 + 行分隔 JSON** 协议解耦通信。

```
┌──────────────────────────────┐
│  Qt6 GUI (C++17)             │  评论树 / AI总结 / 字幕 / 视频信息
│  BridgeClient (QProcess)     │
├──────────────────────────────┤  stdin/stdout 一行一个 JSON（UTF-8）
│  bili_bridge.py（本项目）      │  JSON 协议适配层 ~300 行
├──────────────────────────────┤
│  bili_digest.py（插件本体）    │  WBI / 风控 / 翻页 / 凭据 —— 不修改它
└──────────────────────────────┘
```

## ⚠️ 外部依赖（不随本仓库分发）

本仓库只含 **GUI + 桥接层**，抓取管线在 **bili-digest 插件**（另行获取）：

| 依赖 | 说明 | 缺失时 |
|---|---|---|
| bili-digest 插件 | `bili_digest.py`，提供 WBI 签名 / 风控穿透 / 评论翻页 / 凭据存取 | 默认查找 `D:\软件\AI工具\依赖\bili-digest\`；其他位置请设环境变量 `BILI_DIGEST_DIR` |
| Python 3.10+ | 运行 `bridge/bili_bridge.py` 桥接子进程 | 可用 `BILI_BRIDGE_PYTHON` 指定解释器 |
| Qt 6.11 + MinGW 13.10 | 编译 GUI（运行期 DLL 由 windeployqt 收集） | — |

## 目录结构

```
BiliFetch/
├── CMakeLists.txt
├── LICENSE / .gitignore
├── bridge/bili_bridge.py   # JSON 服务桥（复用 bili_digest 内部函数）
├── assets/
│   ├── app.ico             # 应用图标（16~256 多尺寸，exe + 安装包共用）
│   ├── app.rc.in           # 图标 + 版本信息资源模板（CMake 注入路径 → windres 编译）
│   ├── app_1024.png        # 图标预览
│   └── gen_icon.py         # 图标生成脚本（Pillow 4096 超采样重绘）
├── installer/BiliFetch.iss # Inno Setup 安装脚本
├── src/
│   ├── main.cpp
│   ├── BridgeClient.h/.cpp # QProcess 子进程管理 + JSON 行协议
│   ├── LoginDialog.h/.cpp  # 扫码登录对话框（passport qrcode）
│   └── MainWindow.h/.cpp   # 主窗口：输入区 / 页签 / 菜单 / 状态栏
├── build/                  # CMake 构建产物（含 Qt 运行库，绿色可跑，已 gitignore）
└── dist/                   # 安装包输出（已 gitignore）
```

## 构建（本机已验证）

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/mingw_64" `
      -DCMAKE_CXX_COMPILER="C:/Qt/Tools/mingw1310_64/bin/g++.exe"
cmake --build build
# 运行（Qt 6.11 需绕过许可服务检查）
$env:QTFRAMEWORK_BYPASS_LICENSE_CHECK="1"; .\build\BiliFetch.exe
```

> 若 CMake 找不到 `windres`（图标资源编译器），追加
> `-DCMAKE_RC_COMPILER=C:/Qt/Tools/mingw1310_64/bin/windres.exe`。

构建后 `bili_bridge.py` 自动拷贝到 exe 同目录；`assets/app.rc.in` 由 CMake
`configure_file` 注入本机图标路径生成 `build/app.rc`，再由 windres 编译进 exe
（图标 + 中文版本信息）。RC 编译规则已在 CMakeLists 中精简为纯源文件编译
（windres 预处理器处理不了带空格的 Qt include 路径）。

## 📦 图标与安装包

- **图标**：`assets/app.ico`（bilibili 小电视 + 下载箭头，蓝渐变圆角徽章，
  16/24/32/48/64/128/256 七尺寸）。重绘改样式：`python assets/gen_icon.py`
- **打包**（Inno Setup 7，本机已验证）：

```powershell
& "D:\软件\AI工具\依赖\InnoSetup7\ISCC.exe" installer\BiliFetch.iss
# 产物：dist\BiliFetch-Setup-1.0.0.exe（约 12 MB，LZMA2/max）
```

安装包特性：按用户安装到 `%LOCALAPPDATA%\BiliFetch`（免管理员）、
中/英双语向导、桌面 + 开始菜单快捷方式、卸载器清理诊断日志（归档产物保留）、
装完自检 Python 3.10+ 与 bili-digest 插件目录（缺失弹中文提示）。
已实测：静默安装 → 启动 → 静默卸载 → 无残留。
> 注：安装包未签名，首次运行 SmartScreen 可能提示「未知发布者」，点「更多信息 → 仍要运行」即可。

## 使用

### 🎯 auto 模糊路由（单输入框通吃五类输入）

粘贴任意 B 站输入，点「抓取」即可自动识别分发：

| 识别类型 | 输入形态 | 行为 |
|---|---|---|
| 🎬 视频 | BV 号 / video 链接 / av 号 / b23.tv | 完整提取：元信息 + AI 总结 + 字幕 + 评论树 |
| 📖 专栏 | read/cv 链接、cv 号 | 正文全文 + 互动数据 + 评论树 |
| 💭 动态 | t.bilibili.com / opus / 纯数字动态 ID | 正文/配图/转发 + 评论树 |
| 👤 UP 主 | space.bilibili.com / 纯数字 UID | **全部投稿视频 + 全部专栏**双清单（按发布时间倒序） |
| 🔍 关键词 | 其余任意文本 | 搜索视频清单（可「→ 批量」） |

- 评论模式：🔥 hot_60 热门精选 / 🌐 all_100 全量 + 条数上限
- 「📥 导出 CSV」：当前评论树（含楼中楼）带 UTF-8 BOM 导出，Excel 直接打开
- 「📤 导出 MD」：当前评论树（含楼中楼）导出为独立 Markdown 文件
  （标题 + 抓取模式 + 每楼层 `#### #N` + 楼中楼引用块），默认存输出目录并自动打开
- 顶部「打开 Markdown」打开的是**完整归档 md**（视频信息 + AI 总结 + 字幕 + 全部评论）
- 「→ 批量」：把搜索结果一键加入批量队列

### 📂 UP 主内容页（全部视频 / 全部专栏）

输入 UP 主主页链接抓取后自动切换到该页签：

- 🎬 视频 / 📖 专栏 **两个独立清单**，各自显示发布时间、标题、时长/字数、播放/阅读、BV/cv 号
- 按发布时间**倒序**（表头可再点击切换排序）
- **行首复选框勾选**目标 →「☑ 全选当前页」「☐ 全不选」批量操作
- 「🚀 抓取选中 → 批量队列」把勾选项（BV + read/cvX）送入批量队列
- 清单拉取带逐页 -352 风控退避（6 次重试、3s→18s 递增），实测 654 视频 + 130 专栏全量到手
- **清单同样归档**：抓取成功即在输出目录生成 `<日期>_<UP名>_投稿清单.md`
  （视频/专栏两张全表：发布时间、可点标题、时长/字数、播放/阅读、BV/cv 号）；
  关键词搜索同理生成 `<日期>_搜索_<词>_结果清单.md`

### 🔐 登录（扫码 / 手动 Cookie，方案参考 bilibiliDownloader）

菜单 **账号 → 登录（Ctrl+L）** 打开登录对话框：

- **扫码登录**：B站官方 `passport x/passport-login/web/qrcode/generate` 拉码 →
  每 1.5s `poll` 轮询（86101 未扫码 / 86092 已扫待确认 / 86038 过期自动刷新 /
  0 成功）→ 从 **Set-Cookie 响应头**提取 `SESSDATA/bili_jct/DedeUserID…` →
  经 `x/web-interface/nav` 验证后写入共享凭据。二维码图片本地生成（Python `qrcode`，
  缺失时降级为「复制链接到手机打开」）
- **手动粘贴**：SESSDATA 或整串 Cookie，同样 nav 验证后保存
- 登录态写入 **`~/.dsh/bilibili_credentials.json`（与 DSH bili-digest 插件同一份）**，
  本应用与插件双向共享：GUI 扫码登录后 CLI/DSH 插件同样生效，反之亦然
- **账号 → 退出登录**清除共享凭据中的 Cookie（会同时影响插件，二次确认）
- 状态栏常驻显示 `👤 用户名 / 👤 未登录`（启动后自动 nav 查询）

### ⚙️ 自定义输出目录

菜单 **设置 → 输出目录…** 选择文件夹（QSettings 持久化，重启不丢）；
状态栏右侧始终显示当前输出目录（📁 输出: …）。恢复：**设置 → 恢复默认输出目录**。
自定义目录对单目标抓取与批量队列均生效；CSV 导出对话框也默认从该目录打开。


### 📦 批量任务队列

每行一个目标（BV/链接/任意可路由输入），串行抓取，复用 bili-batch 的防风控逻辑：

- **操作间强制休眠**（默认 30s，可调 0~3600）
- **412/-352 风控自动退避**（默认 420s，同 bili-batch `BACKOFF`）
- **单目标最多重试 3 次**；评论抓到 0 条但实际总量 >5 时自动退避补抓
- 进度条 + 状态栏实时 `[i/n]`；完成后结果表格，**双击行打开对应 Markdown**
- 失败项保留在输入框中可直接重试

### 其他

1. 登录态：菜单 **账号 → 登录** 扫码即可（或 `bili-digest config <cookie>` 手动配置），
   凭据存于 `~/.dsh/bilibili_credentials.json`，与 DSH 插件共享。
2. 单目标产物归档到 `D:\软件\AI工具\输出\B站\`（与插件产物同源），可用「设置 → 输出目录」改。

## bridge JSON 协议

请求/响应各一行 JSON（UTF-8），另有 `event: progress` 进度事件穿插：

```jsonc
// 请求
{"id": 1, "cmd": "digest", "params": {"target": "BV1GJ411x7h7", "comment_mode": "hot_60", "max_comments": 50}}
// 响应
{"id": 1, "ok": true, "data": {"video": {...}, "ai": {...}, "subtitles": {...}, "comments": {"items": [...]}, "markdown_file": "..."}, "elapsed_ms": 1956}
```

| 命令 | 说明 |
|---|---|
| `ping` | 连通性自检 |
| `status` | 凭据状态 / 输出目录 |
| `classify` | 输入类型识别（不联网） |
| `search` | 关键词搜视频 |
| `digest` | 视频完整提取（元信息 + AI 总结 + 字幕 + 结构化评论树） |
| `auto` | **模糊路由**：classify 后分发 video/article/dynamic/user/keyword，统一返回 `{"kind": ...}`；user 返回 `up_content`（全部视频+专栏双清单） |
| `batch` | **批量队列**：`targets[]` 串行抓取，`sleep` 节流 + `backoff` 风控退避 + `retries` 重试，逐项进度事件 |
| `qr_start` | 扫码登录：拉二维码 → `{qr_key, url, png_base64}`（本地 qrcode 生成 PNG） |
| `qr_poll` | 轮询扫码状态：`status = waiting/scanned/ok/expired/error`，成功时返回 `cookie`（Set-Cookie 提取） |
| `login_save` | 保存 Cookie（扫码或手动粘贴）：nav 验证 → 写共享凭据 `~/.dsh/bilibili_credentials.json` |
| `login_check` | 查询登录态：`{is_login, uname, mid, has_cookie}` |
| `logout` | 退出登录：清空共享凭据中的 Cookie |

通用参数 `output_dir`：指定本次抓取的输出目录（缺省用插件默认 `D:\软件\AI工具\输出\B站`），
对 `digest` / `auto` / `batch` 全部生效。

环境变量：`BILI_BRIDGE_PYTHON` 指定解释器，`BILI_DIGEST_DIR` 指定插件目录。

**诊断日志**：bridge 把每笔请求参数与落盘结果追加写入 exe 同目录 `bridge_calls.log`
（UTF-8；遇到"抓取成功但没看到文件"先看它，最后一行 `markdown_file=` 就是实际写入路径）。

**命令行直抓**：`BiliFetch.exe BV1GJ411x7h7`（或任意可路由输入），启动即自动抓取。

## 环境要求

- Qt 6.11.1（mingw_64）+ Qt Tools MinGW 13.10 + CMake 3.21+ + Ninja
- Python 3.10+（bili-digest 仅用标准库；扫码二维码图片依赖 `qrcode`，
  缺失时自动降级为复制链接方式：`pip install qrcode -i https://pypi.tuna.tsinghua.edu.cn/simple`）

## 路线图

- [x] MVP：digest 单视频抓取 + 四页签展示 + Markdown 归档
- [x] `auto` 模糊路由（视频/专栏/动态/UP主/关键词 单输入框自动识别）
- [x] UP 主全部视频 + 全部专栏双清单（按时间倒序、复选、抓取选中入批量队列）
- [x] 自定义输出目录（设置菜单 + QSettings 持久化 + 状态栏显示）
- [x] 批量任务队列（串行抓取 + 节流 + 412 退避，复用 bili-batch 逻辑）
- [x] `space` / `article` / `dynamic` 类型支持（经 auto 分发）
- [x] 评论导出 CSV（含楼中楼，UTF-8 BOM，Excel 直开）
- [x] 评论导出 MD（独立评论 Markdown，含楼中楼引用块）
- [x] 扫码登录 + 手动 Cookie（passport qrcode，登录态与 DSH 插件共享）
- [x] 清单类结果归档落盘（UP主投稿清单 / 关键词结果清单 → 输出目录）
- [x] 诊断日志 `bridge_calls.log` + 命令行直抓 `BiliFetch.exe <target>`
- [x] 应用图标（多尺寸 ico + exe 版本资源）+ Inno Setup 安装包
- [x] GitHub 发布整备（可移植资源模板 / LICENSE / .gitignore）
- [ ] `find-user` 跨视频用户足迹挖掘
- [ ] GitHub Actions 自动构建 + Releases 发布

## 许可证

[MIT](LICENSE) © 2026 BiliFetch contributors
