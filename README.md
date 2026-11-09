# 格式转换器（Converter）

每次转换视频的时候都要找半天，有些转换器还收费，索性自己写一个跨平台的音视频格式转换工具。基于 ffmpeg，C++20 + Dear ImGui，Windows 与 Linux 都提供图形界面，命令行与 GUI 共用同一套转换核心。暂不支持 macOS。

## 平台矩阵

| 平台 | 编译器 | 界面技术 | 产物 |
|---|---|---|---|
| Windows | Visual Studio 2022 (MSVC) | Win32 + Direct3D 11 | `Converter.exe` — 双击为 GUI；带参数运行时为命令行模式 |
| Linux | GCC | GLFW + OpenGL3 | `converter-gui`（图形界面）、`converter-cli`（命令行） |
| macOS | — | — | 不支持（CMake 配置阶段直接报错） |

## 界面

图形界面按三个卡片分区：

- **文件列表** — 多选添加或直接把文件拖进窗口，实时显示每个文件的转换状态（等待 / 转换中 / 完成 / 失败 / 已取消，按状态着色）
- **转换设置** — 输出目录、输出格式、音频码率、直接复制流
- **日志** — 运行时的进度条（含百分比；时长未知时为滚动形态）与历史日志（最多保留 300 条），转换中可随时「取消」

采用自绘亮色主题（白色卡片 + 浅灰底 + 蓝色主按钮），中文字体自动加载系统字体（微软雅黑 / Noto CJK / 文泉驿）。

## 支持的输出格式

| 类型 | 格式（编解码器） |
|---|---|
| 视频 | MP4 / MKV / MOV / TS / FLV（H.264 + AAC）、AVI（MPEG4 + MP3）、WebM（VP9 + Opus）、GIF（12fps 动图） |
| 音频 | MP3、M4A（AAC）、OGG（Opus）、FLAC、WAV |

- 输入支持 mp4 / m4v / mov / mkv / avi / webm / flv / ts / mpg / wmv 等常见容器；
- 视频输出固定用 CRF 质量模式（H.264 CRF 23 / VP9 CRF 30 / MPEG4 q5），「码率」作用于其中的**音频轨**；
- 「直接复制流」：仅更换容器不转码（快、无损）；源编解码器与目标容器不兼容时该文件会报错，换普通转码即可。

## 构建（Windows）

需要 CMake ≥ 3.24 与 Visual Studio 2022（含"使用 C++ 的桌面开发"）：

```
build.bat
```

等价手动命令：

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release -j
```

产物：`build\Release\Converter.exe`（静态链接 CRT，不依赖 VC 运行库）。

## 构建（Linux）

系统依赖（Debian / Ubuntu）：

```
sudo apt install build-essential cmake libgtk-3-dev libgl1-mesa-dev \
                 libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
```

> · X11 开发包（`libx11-dev` 等五项，或直接装元包 `xorg-dev`）供 GLFW 与 OpenGL3 后端使用；
> · `libgtk-3-dev` 供文件对话框库 nfd 使用（没有 GTK3 开发环境时可改用 `-DCONVERTER_NFD_PORTAL=ON`，走 xdg-desktop-portal）；
> · 想在 Wayland 下运行（可选）：额外装 `libwayland-dev wayland-protocols libwayland-bin libxkbcommon-dev`；缺少 `wayland-scanner` 时 GLFW 自动只编 X11 后端，也可显式 `-DCONVERTER_GLFW_WAYLAND=OFF`；
> · 两者都探测不到时 CMake 会直接报错并列出安装命令（GUI 依赖窗口系统后端，无法跳过）。

```
cmake -S . -B build
cmake --build build -j
```

产物：`build/converter-gui`、`build/converter-cli`。

## 安装

```
cmake --install build --prefix <安装目录>
```

安装可执行文件（Linux 为 GUI + CLI 两个）以及随附的 ffmpeg / ffprobe。

## 第三方依赖（配置期自动拉取）

| 依赖 | 版本 | 用途 |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | v1.93.0 | GUI 框架（1.92+ 动态字体按需加载 CJK 字形，无需手工配置字形范围） |
| [nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended) | v1.2.1 | 跨平台文件对话框（Windows: IFileDialog；Linux: GTK3 / Portal） |
| [GLFW](https://github.com/glfw/glfw) | 3.4 | 仅 Linux：窗口与输入 |

全部通过 CMake `FetchContent` 在配置阶段获取，无需手工安装。第三方源码不参与主工程的告警等级。

## ffmpeg 获取策略（配置期自动完成）

`cmake/FetchFFmpeg.cmake` 在 **configure 阶段**按以下顺序处理：

1. 显式指定 `-DCONVERTER_FFMPEG_EXE=<路径>` → 直接使用；
2. `find_program` 检测系统 PATH 中的 ffmpeg / ffprobe；
3. 都没有且 `CONVERTER_FFMPEG_DOWNLOAD=ON`（默认）→ **自动下载对应平台的静态构建**：
   - Windows x64：[BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds)（约 120 MB）
   - Linux x86_64 / aarch64：[johnvansickle.com](https://johnvansickle.com/ffmpeg/)（约 40 MB）
4. 构建完成后自动把 ffmpeg / ffprobe **复制到可执行文件同目录**——运行时零配置。

程序运行时的查找顺序：**exe 同目录 → PATH →（GUI）弹窗手动指定**。

| CMake 选项 | 默认 | 说明 |
|---|---|---|
| `CONVERTER_FFMPEG_DOWNLOAD` | `ON` | 检测不到时自动下载 |
| `CONVERTER_FFMPEG_FORCE_DOWNLOAD` | `OFF` | 跳过检测，强制走下载流程（测试用） |
| `CONVERTER_FFMPEG_DIR` | `build/ffmpeg-bundle` | 下载/解包目录 |
| `CONVERTER_FFMPEG_EXE` / `CONVERTER_FFPROBE_EXE` | 空 | 显式指定可执行文件 |
| `CONVERTER_FFMPEG_URL` | 空 | 覆盖下载地址（内网镜像/测试） |
| `CONVERTER_NFD_PORTAL` | `OFF` | Linux 文件对话框改用 xdg-desktop-portal |
| `CONVERTER_GLFW_WAYLAND` | `ON` | Linux 在找到 `wayland-scanner` 时启用 GLFW 的 Wayland 后端，否则自动退回纯 X11 |

## 使用（GUI）

- 「添加文件…」多选，或直接把文件**拖进窗口**；
- 选择**输出格式**与**码率**（音频轨），勾选"直接复制流"可跳过转码；
- 输出目录默认与源文件相同，可「浏览…」或直接粘贴路径；
- 运行中进度实时显示在「日志」卡片，可随时「取消」；已完成的文件不会重复转换；
- 转换中关闭窗口会弹出确认框。

## 使用（CLI）

```
Converter.exe [-t 格式] [-b 码率] [-c] 文件1 [文件2 ...]      # Windows
./converter-cli [-t 格式] [-b 码率] [-c] 文件1 [文件2 ...]    # Linux
```

- `-t`：mp4 / mkv / mov / ts / flv / avi / webm / gif / mp3 / m4a / ogg / flac / wav（默认 mp3）
- `-b`：音频码率 kbps（默认 192）
- `-c`：直接复制流（仅换容器）
- 输出到各源文件所在目录；全部成功返回 0。

## 源码结构

```
src/converter.h / converter.cpp   跨平台核心：格式表、ffmpeg 子进程调用、进度/错误解析、取消
src/gui_imgui.cpp                 ImGui GUI（Windows: Win32+DX11；Linux: GLFW+OpenGL3）
src/gui_shared.h                  GUI 共享常量：媒体扩展名、码率档位、文件状态、对话框过滤器
src/cli_main.cpp                  Linux 命令行入口（Windows 的 CLI 模式内置于 GUI 可执行文件）
src/app.manifest                  Windows 清单（DPI 感知）
cmake/BuildGuiDeps.cmake          ImGui / nfd / GLFW 依赖接入
cmake/FetchFFmpeg.cmake           ffmpeg 检测 / 下载 / 打包
cmake/ClangFormatOnBuild.cmake    构建时增量格式化脚本
```

## 代码风格

使用仓库根目录的 `.clang-format`：

| 目标 | 作用 |
|---|---|
| `format_on_build`（ALL） | 构建前先 `--dry-run` 检测，仅在确有未格式化文件时才原地格式化（避免无谓重编） |
| `format` | 手动格式化所有源文件 |
| `check-format` | 仅检查，不符合则报错（可用于 CI） |

未找到 `clang-format` 时这些目标会自动跳过，不影响正常构建。
