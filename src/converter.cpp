// converter.cpp — 跨平台转换核心。
// ffmpeg 以外部子进程方式调用：stdout 接 -progress 进度流，stderr 接错误输出。
#include "converter.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>

#include <sys/wait.h>
#include <unistd.h>
#endif

namespace conv
{
    namespace fs = std::filesystem;

    // ---------------- 状态 ----------------
    static fs::path g_ffmpeg, g_ffprobe;

    fs::path GetFFmpeg()
    {
        return g_ffmpeg;
    }
    fs::path GetFFprobe()
    {
        return g_ffprobe;
    }

#ifdef _WIN32
    static const char* kFFmpegName = "ffmpeg.exe";
    static const char* kFFprobeName = "ffprobe.exe";
#else
    static const char* kFFmpegName = "ffmpeg";
    static const char* kFFprobeName = "ffprobe";
#endif

    // ---------------- 字符串工具 ----------------
    std::wstring Utf8ToWide(const std::string& s)
    {
#ifdef _WIN32
        if (s.empty())
            return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n > 0 ? (size_t)n : 0, L'\0');
        if (n > 0)
            MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
        return w;
#else
        std::wstring w;
        w.reserve(s.size());
        size_t i = 0;
        while (i < s.size())
        {
            unsigned char c = (unsigned char)s[i];
            uint32_t cp = 0xFFFD;
            int len = 1;
            if (c < 0x80)
            {
                cp = c;
            }
            else if ((c & 0xE0) == 0xC0 && i + 1 < s.size())
            {
                cp = c & 0x1F;
                len = 2;
            }
            else if ((c & 0xF0) == 0xE0 && i + 2 < s.size())
            {
                cp = c & 0x0F;
                len = 3;
            }
            else if ((c & 0xF8) == 0xF0 && i + 3 < s.size())
            {
                cp = c & 0x07;
                len = 4;
            }
            for (int k = 1; k < len; ++k)
            {
                unsigned char cc = (unsigned char)s[i + k];
                if ((cc & 0xC0) != 0x80)
                {
                    cp = 0xFFFD;
                    len = 1;
                    break;
                }
                cp = (cp << 6) | (cc & 0x3F);
            }
            i += len;
            w.push_back((wchar_t)cp);
        }
        return w;
#endif
    }

    std::string WideToUtf8(const std::wstring& w)
    {
#ifdef _WIN32
        if (w.empty())
            return "";
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? (size_t)n : 0, '\0');
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
        return s;
#else
        std::string s;
        s.reserve(w.size());
        for (wchar_t wc : w)
        {
            uint32_t cp = (uint32_t)(unsigned int)wc;
            if (cp < 0x80)
            {
                s.push_back((char)cp);
            }
            else if (cp < 0x800)
            {
                s.push_back((char)(0xC0 | (cp >> 6)));
                s.push_back((char)(0x80 | (cp & 0x3F)));
            }
            else if (cp < 0x10000)
            {
                s.push_back((char)(0xE0 | (cp >> 12)));
                s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                s.push_back((char)(0x80 | (cp & 0x3F)));
            }
            else
            {
                s.push_back((char)(0xF0 | (cp >> 18)));
                s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                s.push_back((char)(0x80 | (cp & 0x3F)));
            }
        }
        return s;
#endif
    }

    void ConsolePrint(const std::wstring& s)
    {
#ifdef _WIN32
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!h || h == INVALID_HANDLE_VALUE)
            return;
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode))
        {
            DWORD wr = 0;
            WriteConsoleW(h, s.c_str(), (DWORD)s.size(), &wr, nullptr);
        }
        else
        {
            // 输出被重定向时按 UTF-8 写入
            std::string u = WideToUtf8(s);
            DWORD wr = 0;
            WriteFile(h, u.data(), (DWORD)u.size(), &wr, nullptr);
        }
#else
        std::string u = WideToUtf8(s);
        fwrite(u.data(), 1, u.size(), stdout);
        fflush(stdout);
#endif
    }

    // ---------------- 文件工具 ----------------
    bool FileExists(const fs::path& p)
    {
        std::error_code ec;
        auto st = fs::status(p, ec);
        return !ec && fs::is_regular_file(st);
    }

    fs::path MakeOutPath(const fs::path& input, const fs::path& outDir, const std::wstring& ext)
    {
        fs::path dir = outDir.empty() ? input.parent_path() : outDir;
        if (dir.empty())
            dir = fs::path(".");
        fs::path out = dir / input.stem();
        out += ext;
        return out;
    }

    static fs::path SelfExePath()
    {
#ifdef _WIN32
        wchar_t buf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return fs::path(buf);
#elif defined(__linux__)
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0)
        {
            buf[n] = '\0';
            return fs::path(buf);
        }
        return {};
#else
        return {};
#endif
    }

    fs::path FindSystemFFmpeg()
    {
        // 1) 本程序所在目录
        fs::path self = SelfExePath();
        if (!self.empty())
        {
            fs::path f = self.parent_path() / kFFmpegName;
            if (FileExists(f))
                return f;
        }
        // 2) PATH
#ifdef _WIN32
        wchar_t buf[MAX_PATH * 2] = {};
        if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH * 2, buf, nullptr) > 0)
            return fs::path(buf);
#else
        const char* path = getenv("PATH");
        if (path)
        {
            std::string p(path);
            size_t a = 0;
            while (a <= p.size())
            {
                size_t b = p.find(':', a);
                std::string dir = p.substr(a, b == std::string::npos ? std::string::npos : b - a);
                if (!dir.empty())
                {
                    fs::path f = fs::path(dir) / kFFmpegName;
                    if (FileExists(f))
                        return f;
                }
                if (b == std::string::npos)
                    break;
                a = b + 1;
            }
        }
#endif
        return {};
    }

    void SetFFmpegOverride(const fs::path& ffmpegExe)
    {
        g_ffmpeg = ffmpegExe;
        g_ffprobe.clear();
        fs::path fp = ffmpegExe.parent_path() / kFFprobeName;
        if (FileExists(fp))
            g_ffprobe = fp;
    }

    bool EnsureFFmpeg()
    {
        if (!g_ffmpeg.empty())
            return true;
        fs::path f = FindSystemFFmpeg();
        if (f.empty())
            return false;
        SetFFmpegOverride(f);
        return true;
    }

    // ---------------- 内置格式表 ----------------
    static const std::vector<OutputFormat> kFormats = {
        //  名称                    扩展名   视频   码率   可复制  编码参数
        {L"MP4  (H.264 + AAC)", L".mp4", true, true, true,
         L"-c:v libx264 -preset medium -crf 23 -pix_fmt yuv420p -c:a aac"},
        {L"MKV  (H.264 + AAC)", L".mkv", true, true, true,
         L"-c:v libx264 -preset medium -crf 23 -pix_fmt yuv420p -c:a aac"},
        {L"MOV  (H.264 + AAC)", L".mov", true, true, true,
         L"-c:v libx264 -preset medium -crf 23 -pix_fmt yuv420p -c:a aac"},
        {L"TS   (H.264 + AAC)", L".ts", true, true, true,
         L"-c:v libx264 -preset medium -crf 23 -pix_fmt yuv420p -c:a aac"},
        {L"FLV  (H.264 + AAC)", L".flv", true, true, true,
         L"-c:v libx264 -preset medium -crf 23 -pix_fmt yuv420p -c:a aac"},
        {L"AVI  (MPEG4 + MP3)", L".avi", true, true, true, L"-c:v mpeg4 -q:v 5 -c:a libmp3lame"},
        {L"WebM (VP9 + Opus)", L".webm", true, true, true,
         L"-c:v libvpx-vp9 -crf 30 -b:v 0 -deadline good -cpu-used 2 -row-mt 1 -c:a libopus"},
        {L"GIF  (动图 12fps)", L".gif", true, false, false, L"-vf \"fps=12,scale=480:-1:flags=lanczos\" -loop 0 -an"},
        {L"MP3  (音频)", L".mp3", false, true, false, L"-c:a libmp3lame -vn"},
        {L"M4A  (AAC 音频)", L".m4a", false, true, false, L"-c:a aac -vn"},
        {L"OGG  (Opus 音频)", L".ogg", false, true, false, L"-c:a libopus -vn"},
        {L"FLAC (无损音频)", L".flac", false, false, false, L"-c:a flac -vn"},
        {L"WAV  (无损音频)", L".wav", false, false, false, L"-c:a pcm_s16le -vn"},
    };

    const std::vector<OutputFormat>& GetFormats()
    {
        return kFormats;
    }

    int FindFormat(const std::wstring& nameOrExt)
    {
        std::wstring s = nameOrExt;
        for (auto& c : s)
            c = (wchar_t)towlower(c);
        if (!s.empty() && s[0] == L'.')
            s.erase(0, 1);
        if (s.empty())
            return -1;
        const auto& fmts = GetFormats();
        for (size_t i = 0; i < fmts.size(); ++i)
        {
            std::wstring ext = fmts[i].ext;
            for (auto& c : ext)
                c = (wchar_t)towlower(c);
            if (!ext.empty() && ext[0] == L'.')
                ext.erase(0, 1);
            if (s == ext)
                return (int)i;
        }
        for (size_t i = 0; i < fmts.size(); ++i)
            if (s == fmts[i].name)
                return (int)i;
        return -1;
    }

    // ---------------- 子进程封装 ----------------
    namespace detail
    {

        struct CmdSpec
        {
#ifdef _WIN32
            std::wstring line;  // 整条命令行（已加引号）
#else
            std::vector<std::string> argv;  // 参数数组（execv 直调，不经 shell）
#endif
        };

        struct StreamResult
        {
            bool started = false;
            int exitCode = -1;
            std::wstring startErr;
        };

        struct CaptureResult
        {
            bool started = false;
            int exitCode = -1;
            std::string out;
        };

#ifdef _WIN32
        static std::wstring Quote(const std::wstring& s)
        {
            return L"\"" + s + L"\"";
        }
#endif

        // 把参数串拆成 argv（处理双引号，用于 GIF 的 -vf "..." 等）
        static std::vector<std::string> SplitArgs(const std::string& s)
        {
            std::vector<std::string> out;
            std::string cur;
            bool inQ = false;
            for (char c : s)
            {
                if (c == '"')
                {
                    inQ = !inQ;
                }
                else if (!inQ && (c == ' ' || c == '\t'))
                {
                    if (!cur.empty())
                    {
                        out.push_back(cur);
                        cur.clear();
                    }
                }
                else
                {
                    cur.push_back(c);
                }
            }
            if (!cur.empty())
                out.push_back(cur);
            return out;
        }

        static CmdSpec BuildConvertCmd(const fs::path& ffmpeg, const fs::path& in, const fs::path& out,
                                       const OutputFormat& fmt, const Options& opt)
        {
            std::wstring args;
            if (opt.copyStreams)
            {
                args = L"-c copy";  // 仅换容器，不转码
            }
            else
            {
                args = fmt.args;
                if (fmt.audioBitrate)
                    args += L" -b:a " + std::to_wstring(opt.bitrateKbps) + L"k";
            }
            args += L" -sn";  // 字幕流默认不处理，避免个别容器/字幕编码不支持导致失败
#ifdef _WIN32
            CmdSpec c;
            c.line = Quote(ffmpeg.wstring()) + L" -y -hide_banner -nostdin -nostats -i " + Quote(in.wstring()) + L" " +
                     args + L" -progress pipe:1 " + Quote(out.wstring());
            return c;
#else
            std::vector<std::string> argv;
            argv.push_back(ffmpeg.string());
            argv.push_back("-y");
            argv.push_back("-hide_banner");
            argv.push_back("-nostdin");
            argv.push_back("-nostats");
            argv.push_back("-i");
            argv.push_back(in.string());
            for (auto& t : SplitArgs(WideToUtf8(args)))
                argv.push_back(t);
            argv.push_back("-progress");
            argv.push_back("pipe:1");
            argv.push_back(out.string());
            return {argv};
#endif
        }

        // 运行命令并捕获其全部输出（stdout+stderr 合并）
        static CaptureResult RunCapture(const CmdSpec& cmd)
        {
            CaptureResult r;
#ifdef _WIN32
            SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            HANDLE rd = nullptr, wr = nullptr;
            if (!CreatePipe(&rd, &wr, &sa, 0))
                return r;
            SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = wr;
            si.hStdError = wr;
            si.hStdInput = nullptr;
            PROCESS_INFORMATION pi{};
            std::wstring line = cmd.line;
            if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                                &pi))
            {
                CloseHandle(rd);
                CloseHandle(wr);
                return r;
            }
            CloseHandle(wr);  // 父进程先关写端，子进程退出后才能读到 EOF
            char buf[4096];
            DWORD n;
            while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0)
                r.out.append(buf, buf + n);
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            r.exitCode = (int)code;
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(rd);
            r.started = true;
            return r;
#else
            int fds[2];
            if (pipe(fds) != 0)
                return r;
            std::vector<char*> argv;
            argv.reserve(cmd.argv.size() + 1);
            for (const auto& a : cmd.argv)
                argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            pid_t pid = fork();
            if (pid < 0)
            {
                close(fds[0]);
                close(fds[1]);
                return r;
            }
            if (pid == 0)
            {
                dup2(fds[1], STDOUT_FILENO);
                dup2(fds[1], STDERR_FILENO);
                close(fds[0]);
                close(fds[1]);
                execv(argv[0], argv.data());
                _exit(127);
            }
            close(fds[1]);
            char buf[4096];
            ssize_t n;
            while ((n = read(fds[0], buf, sizeof(buf))) > 0)
                r.out.append(buf, buf + n);
            close(fds[0]);
            int st = 0;
            waitpid(pid, &st, 0);
            r.exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            r.started = true;
            return r;
#endif
        }

        // 流式运行：逐行回调 stdout，块回调 stderr，cancel 置 true 时终止子进程
        static StreamResult StreamRun(const CmdSpec& cmd, const std::function<void(const std::string&)>& onLine,
                                      const std::function<void(const std::string&)>& onErrChunk,
                                      const std::atomic<bool>& cancel)
        {
            StreamResult r;
#ifdef _WIN32
            SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
            if (!CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0))
            {
                if (outR)
                    CloseHandle(outR);
                if (outW)
                    CloseHandle(outW);
                if (errR)
                    CloseHandle(errR);
                if (errW)
                    CloseHandle(errW);
                r.startErr = L"创建管道失败";
                return r;
            }
            SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = outW;
            si.hStdError = errW;
            si.hStdInput = nullptr;
            PROCESS_INFORMATION pi{};
            std::wstring line = cmd.line;
            if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                                &pi))
            {
                r.startErr = L"无法启动 ffmpeg（错误码 " + std::to_wstring(GetLastError()) + L"）";
                CloseHandle(outR);
                CloseHandle(outW);
                CloseHandle(errR);
                CloseHandle(errW);
                return r;
            }
            CloseHandle(outW);
            CloseHandle(errW);

            std::atomic<bool> exited{false};
            std::thread watcher(
                [&]
                {
                    while (!exited.load(std::memory_order_relaxed))
                    {
                        if (cancel.load(std::memory_order_relaxed))
                        {
                            TerminateProcess(pi.hProcess, 1);
                            return;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                });
            std::thread errThr(
                [&]
                {
                    char b[4096];
                    DWORD n;
                    while (ReadFile(errR, b, sizeof(b), &n, nullptr) && n > 0)
                        onErrChunk(std::string(b, b + n));
                    CloseHandle(errR);
                });

            {
                std::string acc;
                char b[8192];
                DWORD n;
                while (ReadFile(outR, b, sizeof(b), &n, nullptr) && n > 0)
                {
                    acc.append(b, b + n);
                    size_t pos;
                    while ((pos = acc.find('\n')) != std::string::npos)
                    {
                        std::string l = acc.substr(0, pos);
                        acc.erase(0, pos + 1);
                        if (!l.empty() && l.back() == '\r')
                            l.pop_back();
                        onLine(l);
                    }
                }
                if (!acc.empty())
                    onLine(acc);
            }

            WaitForSingleObject(pi.hProcess, INFINITE);
            exited.store(true);
            watcher.join();
            errThr.join();
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            r.exitCode = (int)code;
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(outR);
            r.started = true;
            return r;
#else
            int outPipe[2] = {-1, -1}, errPipe[2] = {-1, -1};
            if (pipe(outPipe) != 0 || pipe(errPipe) != 0)
            {
                if (outPipe[0] >= 0)
                    close(outPipe[0]);
                if (outPipe[1] >= 0)
                    close(outPipe[1]);
                if (errPipe[0] >= 0)
                    close(errPipe[0]);
                if (errPipe[1] >= 0)
                    close(errPipe[1]);
                r.startErr = L"创建管道失败";
                return r;
            }
            std::vector<char*> argv;
            argv.reserve(cmd.argv.size() + 1);
            for (const auto& a : cmd.argv)
                argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);

            pid_t pid = fork();
            if (pid < 0)
            {
                close(outPipe[0]);
                close(outPipe[1]);
                close(errPipe[0]);
                close(errPipe[1]);
                r.startErr = L"fork 失败";
                return r;
            }
            if (pid == 0)
            {
                dup2(outPipe[1], STDOUT_FILENO);
                dup2(errPipe[1], STDERR_FILENO);
                close(outPipe[0]);
                close(outPipe[1]);
                close(errPipe[0]);
                close(errPipe[1]);
                execv(argv[0], argv.data());
                _exit(127);
            }
            close(outPipe[1]);
            close(errPipe[1]);

            std::atomic<bool> exited{false};
            std::thread watcher(
                [&]
                {
                    while (!exited.load(std::memory_order_relaxed))
                    {
                        if (cancel.load(std::memory_order_relaxed))
                        {
                            kill(pid, SIGKILL);
                            return;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                });
            std::thread errThr(
                [&]
                {
                    char b[4096];
                    ssize_t n;
                    while ((n = read(errPipe[0], b, sizeof(b))) > 0)
                        onErrChunk(std::string(b, b + n));
                    close(errPipe[0]);
                });

            {
                std::string acc;
                char b[8192];
                ssize_t n;
                while ((n = read(outPipe[0], b, sizeof(b))) > 0)
                {
                    acc.append(b, b + n);
                    size_t pos;
                    while ((pos = acc.find('\n')) != std::string::npos)
                    {
                        std::string l = acc.substr(0, pos);
                        acc.erase(0, pos + 1);
                        if (!l.empty() && l.back() == '\r')
                            l.pop_back();
                        onLine(l);
                    }
                }
                if (!acc.empty())
                    onLine(acc);
            }
            close(outPipe[0]);

            int st = 0;
            waitpid(pid, &st, 0);
            exited.store(true);
            watcher.join();
            errThr.join();
            r.exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1);
            r.started = true;
            return r;
#endif
        }

    }  // namespace detail

    // ---------------- 进度/错误解析 ----------------
    struct MediaInfo
    {
        double containerDuration = 0, containerStart = 0;
        double videoDuration = 0, videoStart = 0;
        double audioDuration = 0, audioStart = 0;
    };

    // 严格解析一行数字（"N/A"、空行、杂字符均视为无效）
    static bool ParseDouble(const std::string& s, double& out)
    {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            return false;
        size_t b = s.find_last_not_of(" \t\r\n");
        std::string t = s.substr(a, b - a + 1);
        char* end = nullptr;
        double v = strtod(t.c_str(), &end);
        if (end != t.c_str() + t.size())
            return false;
        if (!(v == v) || v > 1e15 || v < -1e15)
            return false;
        out = v;
        return true;
    }

    // ffprobe 不可用时，从 ffmpeg stderr 的输入信息行兜底解析：
    // "Duration: 00:03:21.04, start: -0.023000, bitrate: ..."
    static MediaInfo ScanErrMedia(const std::string& acc, std::mutex& m)
    {
        MediaInfo mi;
        std::lock_guard<std::mutex> lk(m);
        size_t p = acc.find("Duration: ");
        if (p == std::string::npos)
            return mi;
        const char* s = acc.c_str() + p + 10;
        int h = 0, mm = 0;
        double sec = 0, start = 0;
        int n = sscanf(s, "%d:%d:%lf, start: %lf", &h, &mm, &sec, &start);
        if (n >= 3)
        {
            double d = h * 3600 + mm * 60 + sec;
            if (d > 0)
                mi.containerDuration = d;
            if (n == 4 && start > 0 && mi.containerDuration > start)
                mi.containerStart = start;
        }
        return mi;
    }

    static std::vector<std::string> SplitLines(const std::string& s)
    {
        std::vector<std::string> v;
        size_t a = 0;
        while (true)
        {
            size_t p = s.find('\n', a);
            std::string l = (p == std::string::npos) ? s.substr(a) : s.substr(a, p - a);
            if (!l.empty() && l.back() == '\r')
                l.pop_back();
            v.push_back(l);
            if (p == std::string::npos)
                break;
            a = p + 1;
        }
        return v;
    }

    // 用 ffprobe 探测媒体信息（-of flat 按键名解析，不依赖字段输出顺序）。
    // 按流类型分别记录时长/起点，供不同输出类型选择进度基准：
    //   视频输出 → 容器（或视频流）时长；音频输出 → 音频流（或容器）时长。
    static MediaInfo ProbeMedia(const fs::path& in)
    {
        MediaInfo mi;
        if (g_ffprobe.empty())
            return mi;
        detail::CmdSpec cmd;
#ifdef _WIN32
        cmd.line = detail::Quote(g_ffprobe.wstring()) +
                   L" -v error"
                   L" -show_entries stream=codec_type,duration,start_time"
                   L" -show_entries format=duration,start_time"
                   L" -of flat " +
                   detail::Quote(in.wstring());
#else
        cmd.argv = {g_ffprobe.string(),
                    "-v",
                    "error",
                    "-show_entries",
                    "stream=codec_type,duration,start_time",
                    "-show_entries",
                    "format=duration,start_time",
                    "-of",
                    "flat",
                    in.string()};
#endif
        auto r = detail::RunCapture(cmd);
        if (!r.started)
            return mi;

        // flat 输出形如：
        //   streams.stream.0.codec_type="video"   streams.stream.0.duration="120.000000"
        //   format.duration="200.000000"          format.start_time="0.000000"
        struct StreamRec
        {
            std::string type;
            double dur = 0, start = 0;
            bool hasDur = false, hasStart = false;
        };
        std::map<int, StreamRec> streams;
        for (auto& l : SplitLines(r.out))
        {
            size_t eq = l.find('=');
            if (eq == std::string::npos)
                continue;
            std::string key = l.substr(0, eq);
            std::string val = l.substr(eq + 1);
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
                val = val.substr(1, val.size() - 2);

            if (key == "format.duration")
            {
                double d;
                if (ParseDouble(val, d))
                    mi.containerDuration = d;
            }
            else if (key == "format.start_time")
            {
                double d;
                if (ParseDouble(val, d))
                    mi.containerStart = d;
            }
            else if (key.rfind("streams.stream.", 0) == 0)
            {
                static const std::string kPrefix = "streams.stream.";
                size_t dot = key.find('.', kPrefix.size());
                if (dot == std::string::npos)
                    continue;
                int idx = atoi(key.substr(kPrefix.size(), dot - kPrefix.size()).c_str());
                std::string field = key.substr(dot + 1);
                StreamRec& s = streams[idx];
                if (field == "codec_type")
                {
                    s.type = val;
                }
                else if (field == "duration")
                {
                    if (ParseDouble(val, s.dur))
                        s.hasDur = true;
                }
                else if (field == "start_time")
                {
                    if (ParseDouble(val, s.start))
                        s.hasStart = true;
                }
            }
        }
        for (auto& kv : streams)
        {
            const StreamRec& s = kv.second;
            if (s.type == "video" && s.hasDur && s.dur > mi.videoDuration)
            {
                mi.videoDuration = s.dur;
                mi.videoStart = s.hasStart ? s.start : 0;
            }
            else if (s.type == "audio" && s.hasDur && s.dur > mi.audioDuration)
            {
                mi.audioDuration = s.dur;
                mi.audioStart = s.hasStart ? s.start : 0;
            }
        }
        return mi;
    }

    static std::wstring ExtractError(const std::string& acc, int code)
    {
        auto lines = SplitLines(acc);
        std::vector<std::string> errs;
        for (auto& l : lines)
            if (l.find("Error") != std::string::npos || l.find("error") != std::string::npos ||
                l.find("Invalid") != std::string::npos || l.find("Unable") != std::string::npos)
                errs.push_back(l);
        std::string pick;
        if (errs.empty())
        {
            for (auto it = lines.rbegin(); it != lines.rend(); ++it)
                if (!it->empty())
                {
                    pick = *it;
                    break;
                }
        }
        else
        {
            size_t start = errs.size() > 2 ? errs.size() - 2 : 0;
            for (size_t i = start; i < errs.size(); ++i)
            {
                if (!pick.empty())
                    pick += " | ";
                pick += errs[i];
            }
        }
        if (pick.empty())
            pick = "ffmpeg 退出码 " + std::to_string(code);
        std::wstring w = Utf8ToWide(pick);
        for (auto& c : w)
            if (c < 0x20 && c != L'\t')
                c = L' ';
        if (w.size() > 160)
            w = w.substr(0, 160) + L"…";
        return w;
    }

    // ---------------- 对外接口 ----------------
    bool ConvertOne(const fs::path& in, const fs::path& out, const OutputFormat& fmt, const Options& opt,
                    const ProgressFn& onPercent, const std::atomic<bool>& cancel, std::wstring& errOut)
    {
        errOut.clear();
        if (g_ffmpeg.empty())
        {
            errOut = L"未找到 ffmpeg";
            return false;
        }
        MediaInfo mi = ProbeMedia(in);
        // 进度基准时长：视频输出对齐容器/视频流（视频流主导编码耗时）；
        // 音频输出对齐音频流（-vn 时只编码音轨）。
        double dur = 0, start = 0;
        if (fmt.video)
        {
            if (mi.containerDuration > 0)
            {
                dur = mi.containerDuration;
                start = mi.containerStart;
            }
            else if (mi.videoDuration > 0)
            {
                dur = mi.videoDuration;
                start = mi.videoStart;
            }
        }
        else
        {
            if (mi.audioDuration > 0)
            {
                dur = mi.audioDuration;
                start = mi.audioStart;
            }
            else if (mi.containerDuration > 0)
            {
                dur = mi.containerDuration;
                start = mi.containerStart;
            }
        }
        if (!(start > 0 && dur > start))
            start = 0;
        if (dur < 0)
            dur = 0;

        std::mutex errMtx;
        std::string errAcc;
        auto onErrChunk = [&](const std::string& chunk)
        {
            std::lock_guard<std::mutex> lk(errMtx);
            if (errAcc.size() < 262144)
                errAcc.append(chunk);
        };

        int lastPct = -1;
        auto onLine = [&](const std::string& l)
        {
            double t = -1.0;
            if (l.rfind("out_time_us=", 0) == 0)
                t = atof(l.c_str() + 12) / 1e6;  // out_time_us 单位是微秒
            else if (l.rfind("out_time_ms=", 0) == 0)
                t = atof(l.c_str() + 12) / 1e6;  // ffmpeg 的 out_time_ms 实为微秒
            if (dur <= 0 && t >= 0)
            {
                MediaInfo m2 = ScanErrMedia(errAcc, errMtx);
                if (m2.containerDuration > 0)
                {
                    dur = m2.containerDuration;
                    start = m2.containerStart;
                }
            }
            if (l.rfind("progress=end", 0) == 0)
            {
                onPercent(100);
                return;
            }
            if (dur > 0 && t >= 0)
            {
                double span = dur - start;
                if (span <= 0.01)
                    span = dur;
                double rel = t - start;
                if (rel < 0)
                    rel = 0;
                int p = (int)(rel / span * 100.0 + 0.5);
                if (p < 0)
                    p = 0;
                if (p > 99)
                    p = 99;  // 时长被高估时不提前停在 100%，progress=end 时才到 100
                if (p != lastPct)
                {
                    lastPct = p;
                    onPercent(p);
                }
            }
        };

        auto r = detail::StreamRun(detail::BuildConvertCmd(g_ffmpeg, in, out, fmt, opt), onLine, onErrChunk, cancel);
        if (!r.started)
        {
            errOut = r.startErr.empty() ? std::wstring(L"无法启动 ffmpeg") : r.startErr;
            return false;
        }
        if (cancel.load())
        {
            errOut = L"已取消";
            std::error_code ec;
            fs::remove(out, ec);
            return false;
        }

        std::error_code ec;
        uintmax_t sz = fs::file_size(out, ec);
        if (r.exitCode == 0 && !ec && sz > 0)
            return true;

        errOut = ExtractError(errAcc, r.exitCode);
        fs::remove(out, ec);  // 清理半成品
        return false;
    }

    int RunCli(std::vector<fs::path> files, size_t fmtIndex, int bitrateKbps, bool copyStreams)
    {
        if (!EnsureFFmpeg())
        {
            ConsolePrint(L"[错误] 未找到 ffmpeg（请加入 PATH 或放到程序同目录）\r\n");
            return 2;
        }
        const auto& fmts = GetFormats();
        if (fmtIndex >= fmts.size())
        {
            ConsolePrint(L"[错误] 未知输出格式\r\n");
            return 2;
        }
        const OutputFormat& fmt = fmts[fmtIndex];
        Options opt;
        opt.bitrateKbps = bitrateKbps > 0 ? bitrateKbps : 192;
        opt.copyStreams = copyStreams;
        int fails = 0;
        for (const auto& in : files)
        {
            if (!FileExists(in))
            {
                ConsolePrint(L"[失败] 文件不存在: " + in.wstring() + L"\r\n");
                ++fails;
                continue;
            }
            fs::path out = MakeOutPath(in, {}, fmt.ext);
            ConsolePrint(L"[转换] " + in.wstring() + L"  ->  " + out.wstring() + L"\r\n");
            auto t0 = std::chrono::steady_clock::now();
            std::atomic<bool> cancel{false};
            std::wstring err;
            bool ok = ConvertOne(
                in, out, fmt, opt,
                [](int p)
                {
                    wchar_t tmp[32];
                    swprintf(tmp, 32, L"\r  进度 %3d%%", p);
                    ConsolePrint(tmp);
                    if (p >= 100)
                        ConsolePrint(L"\r\n");
                },
                cancel, err);
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (ok)
                ConsolePrint(L"[完成] " + out.wstring() + L"（用时 " + std::to_wstring((int)(secs + 0.5)) +
                             L" 秒）\r\n");
            else
            {
                ConsolePrint(L"[失败] " + (err.empty() ? std::wstring(L"未知错误") : err) + L"\r\n");
                ++fails;
            }
        }
        return fails ? 1 : 0;
    }

}  // namespace conv
