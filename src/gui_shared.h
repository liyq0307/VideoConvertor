// gui_shared.h — ImGui GUI 共享常量：媒体扩展名、码率档位、文件状态、nfd 过滤器文案。
// UI 层专用，转换引擎见 converter.h。
#pragma once

namespace gui
{
    // 可添加为转换任务的媒体扩展名（小写、含点）
    inline constexpr const char* kMediaExts[] = {".mp4",  ".m4v", ".mov", ".mkv", ".avi",
                                                 ".webm", ".flv", ".ts",  ".mpg", ".wmv"};

    // 音频码率档位（下拉框顺序）
    inline constexpr int kBitrates[] = {128, 192, 256, 320};

    // 文件状态码（与旧 Win32 版一致）
    enum FileState
    {
        StWait = 0,   // 等待中
        StRun = 1,    // 转换中（pct 0..100，-1 表示时长未知）
        StOk = 2,     // 完成
        StFail = 3,   // 失败
        StCancel = 4  // 已取消
    };

    // nfd 过滤器（UTF-8）
    inline constexpr const char* kMediaFilterName = "视频文件";
    inline constexpr const char* kMediaFilterSpec = "mp4,m4v,mov,mkv,avi,webm,flv,ts,mpg,wmv";
    inline constexpr const char* kAllFilterName = "所有文件";
    inline constexpr const char* kAllFilterSpec = "*";

    // 判断是否为可转换的媒体文件（按扩展名，大小写不敏感；p 为 UTF-8 / 窄字符路径字符串）
    inline bool IsMediaFileExt(const std::string& extLower)
    {
        for (auto* e : kMediaExts)
            if (extLower == e)
                return true;
        return false;
    }
}  // namespace gui
