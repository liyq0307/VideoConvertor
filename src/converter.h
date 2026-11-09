// converter.h — 跨平台转换核心：查找/调用外部 ffmpeg 子进程完成音视频格式转换。
// Windows: MSVC (GUI 在 gui_win32.cpp)；Linux: GCC (入口在 cli_main.cpp)。
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace conv
{
    namespace fs = std::filesystem;

    // 输出格式定义。args 为 ffmpeg 编码参数（不含 -b:a，进度条外的东西）。
    struct OutputFormat
    {
        const wchar_t* name;  // 显示名
        const wchar_t* ext;   // 输出扩展名（含点）
        bool video;           // true: 视频输出；false: 纯音频输出
        bool audioBitrate;    // “码率”选项是否适用于该格式
        bool canCopy;         // 是否支持“直接复制流”（仅换容器）
        const wchar_t* args;  // 编码参数
    };

    // 内置格式表（顺序即 GUI 下拉顺序）
    const std::vector<OutputFormat>& GetFormats();

    // 按 "mp4" / ".mp4" / 显示名 查找格式索引，失败返回 -1
    int FindFormat(const std::wstring& nameOrExt);

    struct Options
    {
        int bitrateKbps = 192;     // 音频码率（audioBitrate 格式生效）
        bool copyStreams = false;  // 直接复制流（仅换容器，不转码）
    };

    // 进度回调：0..100（99% 封顶，progress=end 时到 100）
    using ProgressFn = std::function<void(int percent)>;

    // 当前使用的 ffmpeg / ffprobe 路径（未设置时为空）
    fs::path GetFFmpeg();
    fs::path GetFFprobe();

    // 手动指定 ffmpeg（同时探测同目录的 ffprobe）
    void SetFFmpegOverride(const fs::path& ffmpegExe);

    // 查找系统 ffmpeg：本程序所在目录 → PATH
    fs::path FindSystemFFmpeg();

    // 若尚未设置则尝试系统查找；找到返回 true
    bool EnsureFFmpeg();

    bool FileExists(const fs::path& p);

    // 生成输出路径：outDir 为空时与源文件同目录；ext 为输出扩展名（含点）
    fs::path MakeOutPath(const fs::path& input, const fs::path& outDir, const std::wstring& ext);

    // 阻塞转换单个文件。cancel 置 true 会终止子进程。errOut 为失败原因。
    bool ConvertOne(const fs::path& in, const fs::path& out, const OutputFormat& fmt, const Options& opt,
                    const ProgressFn& onPercent, const std::atomic<bool>& cancel, std::wstring& errOut);

    // 命令行批量转换（按顺序执行），返回进程退出码
    int RunCli(std::vector<fs::path> files, size_t fmtIndex, int bitrateKbps, bool copyStreams = false);

    // 控制台输出（Windows 走 WriteConsoleW 支持中文；其他平台写 UTF-8）
    void ConsolePrint(const std::wstring& s);

    std::wstring Utf8ToWide(const std::string& s);
    std::string WideToUtf8(const std::wstring& s);
}  // namespace conv
