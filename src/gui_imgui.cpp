// gui_imgui.cpp — 跨平台 ImGui GUI（Windows: Win32 + DX11；Linux: GLFW + OpenGL3）。
// 转换引擎在 converter.cpp（跨平台）。Windows 带参数运行时自动切换命令行模式。
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "converter.h"
#include "gui_shared.h"

#include "imgui.h"

#ifdef _WIN32
// windows.h 必须在其余 Win32 头之前
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>

#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#else
#include <GLFW/glfw3.h>

#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#endif

#include <nfd.h>

// ================================================================ 公共区（无 ifdef）

namespace fs = std::filesystem;

// 布局尺寸按 DPI 缩放（逻辑像素 → 物理像素），平台入口启动时设置
static float g_dpiScale = 1.0f;
static int S(int v)
{
    return (int)(v * g_dpiScale + 0.5f);
}
static float Sf(int v)
{
    return v * g_dpiScale;
}

struct FileItem
{
    fs::path path;
    std::string nameUtf8;    // 显示用文件名（插入时缓存）
    std::string statusUtf8;  // 状态文案（状态变化时生成）
    int pct = -1;            // StRun 时 0..100；-1 表示时长未知（marquee）
    gui::FileState state = gui::StWait;
};

struct RunSnap  // 开始转换时对设置的快照（运行中 UI 禁改这些控件）
{
    size_t fmtIdx = 0;
    int bitrate = 192;
    bool copy = false;
    fs::path outDir;
};

struct AppState
{
    std::mutex mtx;  // 保护 items / statusLine / logLines / 计数
    std::vector<FileItem> items;
    std::string statusLine;             // 当前状态（日志模块顶部显示）
    std::vector<std::string> logLines;  // 历史日志（最近 kMaxLogLines 条）
    int total = 0, ok = 0, fail = 0, cancelled = 0;

    // 仅 UI 线程访问，无需加锁
    int selected = -1;
    char outDirBuf[1024] = {};
    int bitrateSel = 1;  // gui::kBitrates 下标，默认 192
    size_t fmtIdx = 0;
    bool copy = false;

    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};

    // 模态标志（置位后由 DrawUI 打开弹窗并清除）
    bool showCloseConfirm = false;
    bool showNoFFmpeg = false;
    bool showInfo = false;
    std::string infoMsg;
};

static AppState g_app;

// 按钮动作（DrawUI 只置位，RunActions 在无锁环境执行——文件对话框是阻塞模态，
// 不能在持有 AppState 锁时打开，否则会卡住 worker 的进度回调）
enum Action
{
    ActNone = 0,
    ActAdd,
    ActRemove,
    ActClear,
    ActBrowseOut,
    ActStart,
    ActPickFFmpeg,
};
static Action g_action = ActNone;
static bool g_exitConfirmed = false;  // 关闭确认模态点击"确定"后置位

static std::vector<std::string> g_fmtNames;   // 格式显示名（启动时从 wchar_t* 转好 UTF-8）
static std::vector<const char*> g_fmtNamesC;  // Combo 接口所需的 char* 数组

static constexpr size_t kMaxLogLines = 300;

// 日志追加：LogLocked 要求调用方已持有 g_app.mtx
static void LogLocked(const std::string& line)
{
    if (g_app.logLines.size() >= kMaxLogLines)
        g_app.logLines.erase(g_app.logLines.begin(),
                             g_app.logLines.begin() + (g_app.logLines.size() - kMaxLogLines + 1));
    g_app.logLines.push_back(line);
}

static void Log(const std::string& line)
{
    std::lock_guard<std::mutex> lk(g_app.mtx);
    LogLocked(line);
}

// ---------------------------------------------------------------- 平台无关小工具

static std::string TrimUtf8(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// UTF-8 → fs::path（Windows 经宽字符，Linux 直接窄字符）
static fs::path Utf8ToPath(const std::string& u8)
{
#ifdef _WIN32
    return fs::path(conv::Utf8ToWide(u8));
#else
    return fs::path(u8);
#endif
}

// fs::path → UTF-8（显示用）
static std::string PathToUtf8(const fs::path& p)
{
#ifdef _WIN32
    return conv::WideToUtf8(p.wstring());
#else
    return p.string();
#endif
}

static std::string PathNameUtf8(const fs::path& p)
{
#ifdef _WIN32
    return conv::WideToUtf8(p.filename().wstring());
#else
    return p.filename().string();
#endif
}

static bool SamePath(const fs::path& a, const fs::path& b)
{
#ifdef _WIN32
    return _wcsicmp(a.wstring().c_str(), b.wstring().c_str()) == 0;
#else
    return a == b;
#endif
}

static bool IsMediaFile(const fs::path& p)
{
    std::string e;
#ifdef _WIN32
    e = conv::WideToUtf8(p.extension().wstring());
#else
    e = p.extension().string();
#endif
    for (auto& c : e)
        c = (char)tolower((unsigned char)c);
    return gui::IsMediaFileExt(e);
}

static void BuildFormatNames()
{
    for (const auto& f : conv::GetFormats())
        g_fmtNames.push_back(conv::WideToUtf8(f.name));
    for (auto& s : g_fmtNames)
        g_fmtNamesC.push_back(s.c_str());
}

// ---------------------------------------------------------------- 文件对话框（nfd，统一 UTF-8 API）

static bool g_nfdInited = false;

static std::vector<fs::path> PickMediaFiles()
{
    std::vector<fs::path> res;
    const nfdu8filteritem_t filters[] = {
        {gui::kMediaFilterName, gui::kMediaFilterSpec},
        {gui::kAllFilterName, gui::kAllFilterSpec},
    };
    const nfdpathset_t* paths = nullptr;
    if (NFD_OpenDialogMultipleU8(&paths, filters, 2, nullptr) != NFD_OKAY)
        return res;  // 取消/错误均静默（与原对话框行为一致）
    nfdpathsetsize_t n = 0;
    if (NFD_PathSet_GetCount(paths, &n) == NFD_OKAY)
    {
        for (nfdpathsetsize_t i = 0; i < n; ++i)
        {
            nfdu8char_t* p = nullptr;
            if (NFD_PathSet_GetPathU8(paths, i, &p) == NFD_OKAY && p)
            {
                res.push_back(Utf8ToPath(p));
                NFD_PathSet_FreePathU8(p);
            }
        }
    }
    NFD_PathSet_Free(paths);
    return res;
}

static std::string PickFolderUtf8()
{
    nfdu8char_t* p = nullptr;
    std::string res;
    if (NFD_PickFolderU8(&p, nullptr) == NFD_OKAY && p)
    {
        res = p;
        NFD_FreePathU8(p);
    }
    return res;
}

static fs::path PickFFmpegExe()
{
#ifdef _WIN32
    const nfdu8filteritem_t filters[] = {{"ffmpeg.exe", "ffmpeg.exe"}, {gui::kAllFilterName, gui::kAllFilterSpec}};
#else
    const nfdu8filteritem_t filters[] = {{"ffmpeg", "ffmpeg"}, {gui::kAllFilterName, gui::kAllFilterSpec}};
#endif
    nfdu8char_t* p = nullptr;
    if (NFD_OpenDialogU8(&p, filters, 2, nullptr) == NFD_OKAY && p)
    {
        fs::path res = Utf8ToPath(p);
        NFD_FreePathU8(p);
        return res;
    }
    return {};
}

// ---------------------------------------------------------------- 列表操作

static void AddFiles(const std::vector<fs::path>& paths)
{
    int added = 0;
    for (auto& p : paths)
    {
        if (!conv::FileExists(p))
            continue;
        bool dup = false;
        {
            std::lock_guard<std::mutex> lk(g_app.mtx);
            for (auto& f : g_app.items)
                if (SamePath(f.path, p))
                {
                    dup = true;
                    break;
                }
        }
        if (dup)
            continue;
        FileItem it;
        it.path = p;
        it.nameUtf8 = PathNameUtf8(p);
        std::lock_guard<std::mutex> lk(g_app.mtx);
        g_app.items.push_back(std::move(it));
        LogLocked("已添加: " + PathNameUtf8(p));
        ++added;
    }
    if (paths.empty() || added == 0)
        return;
    std::lock_guard<std::mutex> lk(g_app.mtx);
    LogLocked("新加入 " + std::to_string(added) + " 个文件，列表共 " + std::to_string(g_app.items.size()) + " 个");
}

static void RemoveSelected()
{
    if (g_app.selected < 0 || g_app.selected >= (int)g_app.items.size())
        return;
    std::string name;
    {
        std::lock_guard<std::mutex> lk(g_app.mtx);
        name = g_app.items[g_app.selected].nameUtf8;
        g_app.items.erase(g_app.items.begin() + g_app.selected);
        g_app.selected = -1;
        LogLocked("已移除: " + name);
    }
}

static void ClearList()
{
    std::lock_guard<std::mutex> lk(g_app.mtx);
    g_app.items.clear();
    g_app.selected = -1;
    LogLocked("已清空文件列表");
}

// ---------------------------------------------------------------- 转换执行（worker 线程）

static void Worker(std::vector<size_t> idx, RunSnap snap)
{
    AppState& st = g_app;
    const conv::OutputFormat& fmt = conv::GetFormats()[snap.fmtIdx];
    conv::Options opt{snap.bitrate, snap.copy};

    for (size_t i : idx)
    {
        if (st.cancel.load())
            break;
        fs::path in;
        {
            std::lock_guard<std::mutex> lk(st.mtx);
            if (i < st.items.size())
                in = st.items[i].path;
        }
        if (in.empty())
            continue;
        {
            std::lock_guard<std::mutex> lk(st.mtx);
            FileItem& it = st.items[i];
            it.state = gui::StRun;
            it.pct = -1;
            it.statusUtf8 = "转换中…";
        }

        fs::path out = conv::MakeOutPath(in, snap.outDir, fmt.ext);
        auto onPct = [i](int p)
        {
            std::lock_guard<std::mutex> lk(g_app.mtx);
            FileItem& it = g_app.items[i];
            it.pct = p;
            it.statusUtf8 = "转换中 " + std::to_string(p) + "%";
        };
        std::wstring err;
        bool ok = conv::ConvertOne(in, out, fmt, opt, onPct, st.cancel, err);

        {
            std::lock_guard<std::mutex> lk(st.mtx);
            FileItem& it = st.items[i];
            it.pct = -1;
            if (ok)
            {
                it.state = gui::StOk;
                it.statusUtf8 = "完成";
                ++st.ok;
                LogLocked("完成: " + it.nameUtf8);
            }
            else if (st.cancel.load())
            {
                it.state = gui::StCancel;
                it.statusUtf8 = "已取消";
                ++st.cancelled;
                LogLocked("已取消: " + it.nameUtf8);
            }
            else
            {
                it.state = gui::StFail;
                std::wstring e = err;
                if (e.size() > 120)
                    e = e.substr(0, 120) + L"…";
                it.statusUtf8 = "失败 — " + conv::WideToUtf8(e);
                ++st.fail;
                LogLocked("失败: " + it.nameUtf8 + " — " + conv::WideToUtf8(e));
            }
            st.statusLine =
                "正在转换 " + std::to_string(st.ok + st.fail + st.cancelled) + "/" + std::to_string(st.total) + " …";
        }
    }

    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.running.store(false);
        std::string outInfo =
            snap.outDir.empty() ? "输出保存在各源文件所在目录" : ("输出目录: " + PathToUtf8(snap.outDir));
        st.statusLine = "全部完成 — 成功 " + std::to_string(st.ok) + "，失败 " + std::to_string(st.fail) + "，取消 " +
                        std::to_string(st.cancelled) + "。  " + outInfo;
        LogLocked(st.statusLine);
    }
#ifdef _WIN32
    MessageBeep(MB_OK);
#endif
}

static void StartConversion()
{
    AppState& st = g_app;
    if (st.running.load())
        return;

    std::vector<size_t> idx;
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        for (size_t i = 0; i < st.items.size(); ++i)
            if (st.items[i].state == gui::StWait || st.items[i].state == gui::StFail ||
                st.items[i].state == gui::StCancel)
                idx.push_back(i);
    }
    if (idx.empty())
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.infoMsg = st.items.empty() ? "请先添加要转换的文件。"
                                      : "列表中没有待转换的文件（已完成的不会重复转换；如需重转请清空后重新添加）。";
        st.showInfo = true;
        return;
    }
    if (!conv::EnsureFFmpeg())
    {
        st.showNoFFmpeg = true;
        return;
    }

    RunSnap snap;
    snap.fmtIdx = st.fmtIdx < conv::GetFormats().size() ? st.fmtIdx : 0;
    snap.bitrate = gui::kBitrates[st.bitrateSel];
    snap.copy = st.copy;
    std::string dirU8 = TrimUtf8(st.outDirBuf);
    if (!dirU8.empty())
    {
        snap.outDir = Utf8ToPath(dirU8);
        std::error_code ec;
        fs::create_directory(snap.outDir, ec);  // 已存在则忽略错误
    }

    {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.total = (int)idx.size();
        st.ok = st.fail = st.cancelled = 0;
        st.statusLine = "正在转换 0/" + std::to_string(idx.size()) + " …";
        LogLocked("开始转换，共 " + std::to_string(idx.size()) + " 个文件 → " +
                  conv::WideToUtf8(conv::GetFormats()[snap.fmtIdx].name));
    }
    st.cancel.store(false);
    st.running.store(true);
    std::thread(Worker, std::move(idx), snap).detach();
}

// ---------------------------------------------------------------- CJK 字体加载

static ImFont* g_fontHeading = nullptr;  // 稍大的标题字体（与正文字体同源）
static float g_fontHeadingSize = 0.0f;

// 候选字体依次尝试（Windows: 雅黑/黑体/宋体；Linux: Noto CJK/文泉驿）。
// ImGui 1.92+ 动态字体按需加载字形，单个候选即可覆盖全部中文字符。
static bool LoadCjkFont(float sizePx)
{
    static constexpr const char* kCandidates[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",    // 微软雅黑
        "C:\\Windows\\Fonts\\simhei.ttf",  // 黑体
        "C:\\Windows\\Fonts\\simsun.ttc",  // 宋体
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
    };
    ImGuiIO& io = ImGui::GetIO();
    for (const char* p : kCandidates)
    {
        ImFont* base = io.Fonts->AddFontFromFileTTF(p, sizePx);
        if (base)
        {
            g_fontHeadingSize = sizePx * 1.2f;
            g_fontHeading = io.Fonts->AddFontFromFileTTF(p, g_fontHeadingSize);
            return true;
        }
    }
    io.Fonts->AddFontDefault();
    return false;
}

// ---------------------------------------------------------------- 视觉风格（自定义亮色主题）

static void SetupStyle()
{
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsLight(&s);

    s.WindowRounding = 0.0f;
    s.ChildRounding = 6.0f;
    s.FrameRounding = 6.0f;
    s.PopupRounding = 8.0f;
    s.GrabRounding = 4.0f;
    s.ScrollbarRounding = 8.0f;
    s.TabRounding = 6.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;  // 输入框/下拉框带浅色描边
    s.WindowPadding = ImVec2(12, 12);
    s.FramePadding = ImVec2(10, 7);
    s.ItemSpacing = ImVec2(8, 9);
    s.ItemInnerSpacing = ImVec2(6, 6);
    s.ScrollbarSize = 10.0f;
    s.PopupBorderSize = 1.0f;
    s.GrabMinSize = 10.0f;

    const ImVec4 bg(0.949f, 0.957f, 0.969f, 1.0f);      // #F2F4F7 页面底色
    const ImVec4 card(1.0f, 1.0f, 1.0f, 1.0f);          // 白色卡片
    const ImVec4 border(0.816f, 0.843f, 0.871f, 1.0f);  // #D0D7DE
    const ImVec4 text(0.122f, 0.137f, 0.157f, 1.0f);    // #1F2328
    const ImVec4 muted(0.478f, 0.510f, 0.557f, 1.0f);   // #7A828E
    const ImVec4 accent(0.145f, 0.388f, 0.922f, 1.0f);  // #2563EB
    const ImVec4 accentHi(0.094f, 0.325f, 0.835f, 1.0f);
    const ImVec4 accentLo(0.863f, 0.910f, 1.0f, 1.0f);  // #DCE8FF 选中底色

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = card;
    c[ImGuiCol_PopupBg] = card;
    c[ImGuiCol_FrameBg] = card;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.969f, 0.976f, 0.984f, 1.0f);  // #F7F9FB
    c[ImGuiCol_FrameBgActive] = ImVec4(0.949f, 0.961f, 0.973f, 1.0f);
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = muted;
    c[ImGuiCol_Button] = card;  // 次要按钮：白底 + 细描边
    c[ImGuiCol_ButtonHovered] = ImVec4(0.961f, 0.969f, 0.980f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.925f, 0.937f, 0.949f, 1.0f);
    c[ImGuiCol_Header] = accentLo;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.816f, 0.878f, 1.0f, 1.0f);
    c[ImGuiCol_HeaderActive] = accentLo;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accentHi;
    c[ImGuiCol_PlotHistogram] = accent;  // 进度条填充
    c[ImGuiCol_PlotHistogramHovered] = accentHi;
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_ScrollbarBg] = bg;
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.780f, 0.800f, 0.831f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.702f, 0.722f, 0.761f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.35f);
}

// ---------------------------------------------------------------- UI 绘制（每帧）

// 进度条底色（浅灰轨道）
static constexpr ImU32 kTrackU32 = IM_COL32(227, 231, 237, 255);

static void DrawMarquee(float height)
{
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImVec2(ImGui::GetContentRegionAvail().x, height);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), kTrackU32, Sf(4));
    float frac = fmodf((float)ImGui::GetTime() * 0.5f, 1.0f);
    float w = sz.x * 0.3f;
    float x0 = p0.x + (sz.x - w) * frac;
    dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x0 + w, p0.y + sz.y), ImGui::GetColorU32(ImGuiCol_PlotHistogram), Sf(4));
    ImVec2 ts = ImGui::CalcTextSize("转换中…");
    dl->AddText(ImVec2(p0.x + (sz.x - ts.x) * 0.5f, p0.y + (sz.y - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text),
                "转换中…");
    ImGui::Dummy(sz);
}

static void DrawBar(float frac, const char* text, float height)
{
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImVec2(ImGui::GetContentRegionAvail().x, height);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), kTrackU32, Sf(6));
    float f = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
    float fw = sz.x * f;
    if (fw >= Sf(6))
        dl->AddRectFilled(p0, ImVec2(p0.x + fw, p0.y + sz.y), ImGui::GetColorU32(ImGuiCol_PlotHistogram), Sf(6));
    ImVec2 ts = ImGui::CalcTextSize(text);
    float tx = p0.x + (sz.x - ts.x) * 0.5f;
    const bool covered = (tx + ts.x <= p0.x + fw);  // 文字完全落在填充区内时用白色保证对比度
    dl->AddText(ImVec2(tx, p0.y + (sz.y - ts.y) * 0.5f),
                covered ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(ImGuiCol_Text), text);
    ImGui::Dummy(sz);
}

// 文件状态对应的状态文字颜色
static ImU32 StatusColorU32(gui::FileState state)
{
    switch (state)
    {
        case gui::StRun:
            return IM_COL32(37, 99, 235, 255);  // 蓝
        case gui::StOk:
            return IM_COL32(22, 138, 74, 255);  // 绿
        case gui::StFail:
            return IM_COL32(200, 38, 38, 255);  // 红
        case gui::StWait:
            return IM_COL32(87, 96, 106, 255);  // 深灰
        case gui::StCancel:
        default:
            return IM_COL32(122, 130, 142, 255);  // 浅灰
    }
}

// 按像素宽度截断 UTF-8 文本（末尾加 "…"，不切断多字节码点）
static std::string TruncateToWidth(const std::string& s, float maxW)
{
    if (ImGui::CalcTextSize(s.c_str()).x <= maxW)
        return s;
    size_t cut = s.size();
    while (cut > 0)
    {
        --cut;
        while (cut > 0 && (s[cut] & 0xC0) == 0x80)  // 回退到码点起点
            --cut;
        std::string t = s.substr(0, cut) + "…";
        if (ImGui::CalcTextSize(t.c_str()).x <= maxW)
            return t;
    }
    return "…";
}

// 卡片标题：左侧蓝色小竖条 + 标题文字
static void SectionTitle(const char* title)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (g_fontHeading)
        ImGui::PushFont(g_fontHeading, g_fontHeadingSize);
    float lh = ImGui::GetTextLineHeight();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Sf(9));
    ImGui::TextUnformatted(title);
    if (g_fontHeading)
        ImGui::PopFont();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + Sf(4)), ImVec2(p.x + Sf(3), p.y + lh - Sf(4)),
                                              IM_COL32(37, 99, 235, 255), Sf(2));
}

// 设置行标签：与控件垂直居中对齐，并统一标签列宽
static void RowLabel(const char* label)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(Sf(72));
}

// 下拉框：把 ImGui 默认的灰色箭头按钮压成与输入框同色的透明区，视觉更统一
static void ComboFlat(const char* id, int* cur, const char* const items[], int n, float width)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.949f, 0.961f, 0.988f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.910f, 0.937f, 0.996f, 1.0f));
    ImGui::SetNextItemWidth(width);
    ImGui::Combo(id, cur, items, n);
    ImGui::PopStyleColor(3);
}

static void DrawUI()
{
    AppState& st = g_app;
    const bool running = st.running.load();

    // 快照共享状态（worker 可能随时改写；文件数少，整表拷贝廉价）
    std::vector<FileItem> items;
    std::string statusLine;
    std::vector<std::string> logLines;
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        items = st.items;
        statusLine = st.statusLine;
        logLines = st.logLines;
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##main", nullptr, wf);

    const float cardPad = Sf(10);
    const float rowH = ImGui::GetTextLineHeight() + Sf(10);

    // ============================================ 模块一：文件列表
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(cardPad, cardPad));
    ImGui::BeginChild("##card_files", ImVec2(0.0f, Sf(250)), ImGuiChildFlags_Borders);
    ImGui::PopStyleVar();
    {
        SectionTitle("文件列表");
        if (!items.empty())
        {
            char cnt[32];
            snprintf(cnt, sizeof(cnt), "共 %d 个", (int)items.size());
            ImVec2 ts = ImGui::CalcTextSize(cnt);
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ts.x - Sf(12));
            ImGui::TextDisabled("%s", cnt);
        }

        // 列表主体：可滚动的浅色列表区（预留下方按钮行高度）
        const float reserve = Sf(28) + ImGui::GetStyle().ItemSpacing.y;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.969f, 0.973f, 0.980f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Sf(6), Sf(6)));
        ImGui::BeginChild("##files", ImVec2(0.0f, -reserve), ImGuiChildFlags_Borders);
        ImGui::PopStyleVar();
        if (items.empty())
        {
            const char* hint = "将媒体文件拖到此窗口，或点击「添加文件…」";
            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 cp = ImGui::GetCursorPos();
            ImVec2 ts = ImGui::CalcTextSize(hint);
            ImGui::SetCursorPos(ImVec2(cp.x + (avail.x - ts.x) * 0.5f, cp.y + (avail.y - ts.y) * 0.5f));
            ImGui::TextDisabled("%s", hint);
        }
        for (int i = 0; i < (int)items.size(); ++i)
        {
            ImGui::PushID(i);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 rowSz = ImVec2(ImGui::GetContentRegionAvail().x, rowH);
            ImGui::InvisibleButton("##row", rowSz);
            const bool hovered = ImGui::IsItemHovered();
            const bool clicked = ImGui::IsItemClicked();
            if (clicked)
                st.selected = i;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (st.selected == i)  // 选中行：圆角高亮
                dl->AddRectFilled(p0, ImVec2(p0.x + rowSz.x, p0.y + rowH), ImGui::GetColorU32(ImGuiCol_Header), Sf(6));
            else if (hovered)
                dl->AddRectFilled(p0, ImVec2(p0.x + rowSz.x, p0.y + rowH), IM_COL32(0, 0, 0, 12), Sf(6));
            ImVec2 stsTs = ImGui::CalcTextSize(items[i].statusUtf8.c_str());
            float maxNameW = rowSz.x - stsTs.x - Sf(28);
            std::string name = TruncateToWidth(items[i].nameUtf8, maxNameW);
            float ty = p0.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
            dl->AddText(ImVec2(p0.x + Sf(8), ty), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
            dl->AddText(ImVec2(p0.x + rowSz.x - stsTs.x - Sf(8), ty), StatusColorU32(items[i].state),
                        items[i].statusUtf8.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::BeginDisabled(running);
        if (ImGui::Button("＋ 添加文件…", ImVec2(Sf(112), Sf(28))))
            g_action = ActAdd;
        ImGui::SameLine();
        if (ImGui::Button("－ 移除选中", ImVec2(Sf(112), Sf(28))))
            g_action = ActRemove;
        ImGui::SameLine();
        if (ImGui::Button("清空列表", ImVec2(Sf(92), Sf(28))))
            g_action = ActClear;
        ImGui::EndDisabled();
    }
    ImGui::EndChild();

    // ============================================ 模块二：转换设置
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(cardPad, cardPad));
    ImGui::BeginChild("##card_settings", ImVec2(0.0f, Sf(176)), ImGuiChildFlags_Borders);
    ImGui::PopStyleVar();
    {
        SectionTitle("转换设置");

        // 输出目录
        ImGui::BeginDisabled(running);
        RowLabel("输出目录");
        ImGui::SetNextItemWidth(Sf(348));
        ImGui::InputTextWithHint("##outdir", "默认与源文件同目录", st.outDirBuf, sizeof(st.outDirBuf));
        ImGui::SameLine();
        if (ImGui::Button("浏览…", ImVec2(Sf(56), 0)))
            g_action = ActBrowseOut;
        ImGui::EndDisabled();

        const auto& fmts = conv::GetFormats();
        const bool brEnabled = (st.fmtIdx < fmts.size()) && fmts[st.fmtIdx].audioBitrate && !running;
        const bool copyEnabled = (st.fmtIdx < fmts.size()) && fmts[st.fmtIdx].canCopy && !running;
        if (!copyEnabled)
            st.copy = false;  // 格式不支持时强制取消勾选（与原 UpdateFormatUI 一致）

        // 输出格式 / 码率（同一行两列）
        RowLabel("输出格式");
        ImGui::BeginDisabled(running);
        ComboFlat("##fmt", (int*)&st.fmtIdx, g_fmtNamesC.data(), (int)g_fmtNamesC.size(), Sf(176));
        ImGui::EndDisabled();

        ImGui::SameLine(Sf(300));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("码率");
        ImGui::SameLine(0.0f, Sf(8));
        ImGui::BeginDisabled(!brEnabled);
        static constexpr const char* kBitrateItems[] = {"128 kbps", "192 kbps", "256 kbps", "320 kbps"};
        ComboFlat("##br", &st.bitrateSel, kBitrateItems, IM_ARRAYSIZE(kBitrateItems), Sf(110));
        ImGui::EndDisabled();

        // 直接复制流
        RowLabel("复制方式");
        ImGui::BeginDisabled(!copyEnabled);
        ImGui::Checkbox("直接复制流(快)", &st.copy);
        ImGui::EndDisabled();

        // 开始 / 取消（右对齐，等宽等高）
        const float btnW = Sf(96);
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - btnW - Sf(8) - btnW);
        ImGui::BeginDisabled(running);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.145f, 0.388f, 0.922f, 1.0f));  // 主按钮：蓝色
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.094f, 0.325f, 0.835f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.075f, 0.267f, 0.706f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.145f, 0.388f, 0.922f, 1.0f));
        if (ImGui::Button("开始转换", ImVec2(btnW, Sf(32))))
            g_action = ActStart;
        ImGui::PopStyleColor(5);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!running);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.353f, 0.388f, 0.435f, 1.0f));
        if (ImGui::Button("取消", ImVec2(btnW, Sf(32))))
            st.cancel.store(true);  // 引擎内部会终止子进程
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
    }
    ImGui::EndChild();

    // ============================================ 模块三：日志
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(cardPad, cardPad));
    ImGui::BeginChild("##card_log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    ImGui::PopStyleVar();
    {
        SectionTitle("日志");

        // 进度条：运行中显示当前文件进度；时长未知时为滚动（marquee）模式
        if (running)
        {
            int pct = -1;
            for (auto& it : items)
                if (it.state == gui::StRun)
                {
                    pct = it.pct;
                    break;
                }
            char overlay[16] = "";
            if (pct >= 0)
            {
                snprintf(overlay, sizeof(overlay), "%d%%", pct);
                DrawBar(pct / 100.0f, overlay, Sf(16));
            }
            else
            {
                DrawMarquee(Sf(16));
            }
        }

        // 当前状态
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.310f, 0.345f, 0.396f, 1.0f));
        ImGui::TextWrapped("%s", statusLine.c_str());
        ImGui::PopStyleColor();

        // 历史日志（可滚动，自动跟随最新一条）
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.969f, 0.973f, 0.980f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Sf(8), Sf(6)));
        ImGui::BeginChild("##logbody", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
        ImGui::PopStyleVar();
        for (auto& l : logLines)
            ImGui::TextWrapped("%s", l.c_str());
        static size_t lastLogCount = 0;
        if (logLines.size() > lastLogCount)
        {
            ImGui::SetScrollHereY(1.0f);
            lastLogCount = logLines.size();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    // ---- 模态：ffmpeg 缺失 ----
    if (st.showNoFFmpeg)
    {
        ImGui::OpenPopup("未找到 ffmpeg");
        st.showNoFFmpeg = false;
    }
    if (ImGui::BeginPopupModal("未找到 ffmpeg", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
#ifdef _WIN32
        const char* exeName = "ffmpeg.exe";
#else
        const char* exeName = "ffmpeg";
#endif
        ImGui::Text(
            "未找到 %s。\n\n可以：\n  · 将 %s 放到本程序同一目录\n  · 或将 ffmpeg 所在目录加入系统 PATH\n  · "
            "或现在浏览选择 %s 的位置\n\n是否现在选择？",
            exeName, exeName, exeName);
        if (ImGui::Button("现在选择…", ImVec2(Sf(110), 0)))
        {
            g_action = ActPickFFmpeg;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(Sf(80), 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- 模态：提示信息（无待转换文件等） ----
    if (st.showInfo)
    {
        ImGui::OpenPopup("提示");
        st.showInfo = false;
    }
    if (ImGui::BeginPopupModal("提示", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(st.infoMsg.c_str());
        ImGui::Separator();
        if (ImGui::Button("确定", ImVec2(Sf(80), 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- 模态：退出确认 ----
    if (st.showCloseConfirm)
    {
        ImGui::OpenPopup("退出确认");
        st.showCloseConfirm = false;
    }
    if (ImGui::BeginPopupModal("退出确认", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("正在转换，确定要退出吗？");
        ImGui::Separator();
        if (ImGui::Button("确定", ImVec2(Sf(80), 0)))
        {
            st.cancel.store(true);
            g_exitConfirmed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(Sf(80), 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::End();
}

// 动作执行（不持有 AppState 锁：文件对话框是阻塞模态）
static void RunActions()
{
    AppState& st = g_app;
    switch (g_action)
    {
        case ActAdd:
            AddFiles(PickMediaFiles());
            break;
        case ActRemove:
            RemoveSelected();
            break;
        case ActClear:
            ClearList();
            break;
        case ActBrowseOut:
        {
            std::string d = PickFolderUtf8();
            if (!d.empty())
                snprintf(st.outDirBuf, sizeof(st.outDirBuf), "%s", d.c_str());
            break;
        }
        case ActStart:
            StartConversion();
            break;
        case ActPickFFmpeg:
        {
            fs::path f = PickFFmpegExe();
            if (!f.empty())
            {
                conv::SetFFmpegOverride(f);
                StartConversion();  // 选好 ffmpeg 后重入启动流程
            }
            break;
        }
        case ActNone:
        default:
            break;
    }
    g_action = ActNone;
}

// ================================================================ Windows 平台段

#ifdef _WIN32

static HWND g_hwnd = nullptr;
static bool g_exitRequested = false;  // 退出确认后由主循环销毁窗口

// D3D11（imgui example_win32_directx11 标准样板）
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
                                                featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                                &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == E_INVALIDARG)  // DirectX 11.0 平台不支持双 level 数组，退回单个
    {
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
                                            &featureLevelArray[1], 1, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    if (FAILED(res))
        return false;

    D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
    rtvd.Format = sd.BufferDesc.Format;
    rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)) != S_OK || pBackBuffer == nullptr)
        return false;
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, &rtvd, &g_mainRenderTargetView);
    pBackBuffer->Release();
    return g_mainRenderTargetView != nullptr;
}

static void CleanupRenderTarget()
{
    if (g_mainRenderTargetView)
    {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain)
    {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }
    if (g_pd3dDeviceContext)
    {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice)
    {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return 1;
    switch (msg)
    {
        case WM_SIZE:
            if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
            {
                CleanupRenderTarget();
                g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
                rtvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                ID3D11Texture2D* pBackBuffer = nullptr;
                if (g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)) == S_OK && pBackBuffer)
                {
                    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, &rtvd, &g_mainRenderTargetView);
                    pBackBuffer->Release();
                }
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU)  // 禁用 Alt 菜单 beep
                return 0;
            break;
        case WM_DROPFILES:
        {
            HDROP h = (HDROP)wParam;
            UINT n = DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
            std::vector<fs::path> add;
            for (UINT i = 0; i < n; ++i)
            {
                wchar_t buf[MAX_PATH * 4] = {};
                if (DragQueryFileW(h, i, buf, MAX_PATH * 4) > 0)
                {
                    fs::path p(buf);
                    if (IsMediaFile(p))
                        add.push_back(p);
                }
            }
            DragFinish(h);
            AddFiles(add);
            return 0;
        }
        case WM_CLOSE:
            if (g_app.running.load())
            {
                g_app.showCloseConfirm = true;  // 弹确认模态，不销毁窗口
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DPICHANGED:
        {
            RECT* r = (RECT*)lParam;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow)
{
    // 带参数 → 命令行模式（无 GUI）：-t <格式> -b <码率> -c(复制流) 文件...
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1)
    {
        std::vector<fs::path> files;
        size_t fmtIdx = 0;
        int br = 192;
        bool copy = false;
        for (int i = 1; i < argc; ++i)
        {
            if (wcscmp(argv[i], L"-t") == 0 && i + 1 < argc)
            {
                int f = conv::FindFormat(argv[i + 1]);
                if (f < 0)
                {
                    conv::ConsolePrint(L"[错误] 未知格式: " + std::wstring(argv[i + 1]) +
                                       L"（可用: mp4/mkv/mov/ts/flv/avi/webm/gif/mp3/m4a/ogg/flac/wav）\r\n");
                    LocalFree(argv);
                    return 2;
                }
                fmtIdx = (size_t)f;
                ++i;
            }
            else if (wcscmp(argv[i], L"-b") == 0 && i + 1 < argc)
            {
                br = _wtoi(argv[i + 1]);
                ++i;
            }
            else if (wcscmp(argv[i], L"-c") == 0)
            {
                copy = true;
            }
            else
            {
                files.emplace_back(argv[i]);
            }
        }
        LocalFree(argv);
        return conv::RunCli(std::move(files), fmtIdx, br, copy);
    }
    if (argv)
        LocalFree(argv);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    conv::EnsureFFmpeg();  // 启动即查找：exe 同目录 → PATH，状态行显示实际使用的 ffmpeg

    g_dpiScale = GetDpiForSystem() / 96.0f;
    if (g_dpiScale <= 0.0f)
        g_dpiScale = 1.0f;

    if (!conv::GetFFmpeg().empty())
        g_app.statusLine = "就绪。ffmpeg: " + PathToUtf8(conv::GetFFmpeg());
    else
        g_app.statusLine = "就绪。未找到 ffmpeg.exe — 开始转换时会提示选择（也可将 ffmpeg.exe 放到程序目录）。";

    // 窗口（样式与旧版一致；客户区 560x436 逻辑尺寸）
    HINSTANCE hThis = hInst;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hThis;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ConverterImGuiWnd";
    RegisterClassExW(&wc);

    // 窗口：三个模块纵向排列（客户区 560x700 逻辑尺寸）
    RECT rc{0, 0, S(560), S(700)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, GetDpiForSystem());
    int ww = (int)(rc.right - rc.left), wh = (int)(rc.bottom - rc.top);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"格式转换器", style, CW_USEDEFAULT, CW_USEDEFAULT, ww, wh, nullptr,
                             nullptr, hThis, nullptr);
    if (!g_hwnd)
        return 1;

    DragAcceptFiles(g_hwnd, TRUE);
    ChangeWindowMessageFilterEx(g_hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);  // 提权进程也能收到拖放

    if (!CreateDeviceD3D(g_hwnd))
        return 1;
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Tab 键焦点切换
    io.IniFilename = nullptr;
    if (!LoadCjkFont(16.0f * g_dpiScale))
        g_app.statusLine += "（警告：未找到中文字体，界面可能显示为 ?）";
    SetupStyle();
    BuildFormatNames();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    g_nfdInited = (NFD_Init() == NFD_OKAY);

    bool done = false;
    while (!done)
    {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
            if (m.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;
        if (g_exitConfirmed && !g_exitRequested)
        {
            g_exitRequested = true;
            DestroyWindow(g_hwnd);  // → WM_DESTROY → PostQuitMessage
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUI();
        ImGui::Render();

        const float clear[4] = {0.94f, 0.94f, 0.94f, 1.00f};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);  // vsync 限帧

        RunActions();
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    if (g_nfdInited)
        NFD_Quit();
    CoUninitialize();
    return 0;
}

#else  // ============================================================ Linux 平台段

static GLFWwindow* g_window = nullptr;

static void DropCB(GLFWwindow*, int count, const char** paths)
{
    std::vector<fs::path> add;
    for (int i = 0; i < count; ++i)
    {
        fs::path p(paths[i]);
        if (IsMediaFile(p))
            add.push_back(p);
    }
    AddFiles(add);
}

static void CloseCB(GLFWwindow* w)
{
    if (g_app.running.load() && !g_exitConfirmed)
    {
        glfwSetWindowShouldClose(w, GLFW_FALSE);
        g_app.showCloseConfirm = true;  // 弹确认模态
    }
}

int main(int, char**)
{
    if (!glfwInit())
        return 1;

    float xs = 1.0f, ys = 1.0f;
    if (GLFWmonitor* mon = glfwGetPrimaryMonitor())
        glfwGetMonitorContentScale(mon, &xs, &ys);
    if (xs > 0.0f)
        g_dpiScale = xs;

    g_window = glfwCreateWindow(S(560), S(700), "格式转换器", nullptr, nullptr);
    if (!g_window)
    {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(g_window);
    glfwSwapInterval(1);
    glfwSetDropCallback(g_window, DropCB);
    glfwSetWindowCloseCallback(g_window, CloseCB);

    conv::EnsureFFmpeg();  // 启动即查找：程序同目录 → PATH
    if (!conv::GetFFmpeg().empty())
        g_app.statusLine = "就绪。ffmpeg: " + PathToUtf8(conv::GetFFmpeg());
    else
        g_app.statusLine = "就绪。未找到 ffmpeg — 开始转换时会提示选择（也可将 ffmpeg 放到程序目录）。";

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    if (!LoadCjkFont(16.0f * g_dpiScale))
        g_app.statusLine += "（警告：未找到中文字体，界面可能显示为 ?）";
    SetupStyle();
    BuildFormatNames();

    ImGui_ImplGlfw_InitForOpenGL(g_window, true);
    ImGui_ImplOpenGL3_Init("#version 130");
    g_nfdInited = (NFD_Init() == NFD_OKAY);

    while (!glfwWindowShouldClose(g_window))
    {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        DrawUI();
        ImGui::Render();

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(g_window);

        RunActions();
        if (g_exitConfirmed)
            glfwSetWindowShouldClose(g_window, GLFW_TRUE);  // 关闭确认后真正退出
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    if (g_nfdInited)
        NFD_Quit();
    glfwDestroyWindow(g_window);
    glfwTerminate();
    return 0;
}

#endif  // _WIN32
