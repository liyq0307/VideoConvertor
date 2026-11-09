@echo off
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul || (echo [ERROR] 未找到 cmake，请安装 CMake 并加入 PATH & exit /b 1)

rem Windows 默认使用 Visual Studio（cmake 会自动探测已安装的最新 VS）
cmake -S "%~dp0" -B "%~dp0build" %* || exit /b 1
cmake --build "%~dp0build" --config Release -j || exit /b 1

echo.
echo Build OK: %~dp0build\Release\Converter.exe
