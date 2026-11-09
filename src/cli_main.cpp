// cli_main.cpp — Linux 入口（GCC）。转换引擎在 converter.cpp。
// 用法: converter-cli [-t 格式] [-b 码率] [-c] 文件...
//   -t mp4/mkv/mov/ts/flv/avi/webm/gif/mp3/m4a/ogg/flac/wav（默认 mp3）
//   -b 音频码率 kbps（默认 192）
//   -c 直接复制流（仅换容器，不转码）
#include <cstdlib>
#include <vector>

#include "converter.h"

int main(int argc, char** argv)
{
    std::vector<conv::fs::path> files;
    size_t fmtIdx = 0;
    int br = 192;
    bool copy = false;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "-t" && i + 1 < argc)
        {
            int f = conv::FindFormat(conv::Utf8ToWide(argv[i + 1]));
            if (f < 0)
            {
                conv::ConsolePrint(L"[错误] 未知格式\r\n");
                return 2;
            }
            fmtIdx = (size_t)f;
            ++i;
        }
        else if (a == "-b" && i + 1 < argc)
        {
            br = atoi(argv[i + 1]);
            ++i;
        }
        else if (a == "-c")
        {
            copy = true;
        }
        else
        {
            files.emplace_back(argv[i]);
        }
    }
    return conv::RunCli(std::move(files), fmtIdx, br, copy);
}
