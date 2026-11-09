cmake_minimum_required(VERSION 3.24)

# 平台矩阵：Windows = MSVC (GUI + CLI)；Linux = GCC (CLI)；macOS 不支持
if(APPLE)
    message(FATAL_ERROR "本项目不支持 macOS（Windows: Visual Studio + GUI；Linux: GCC + CLI）")
endif()

project(Converter VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- ffmpeg 获取策略选项 ----
option(CONVERTER_FFMPEG_DOWNLOAD      "配置时检测不到 ffmpeg 则自动下载静态构建并打包" ON)
option(CONVERTER_FFMPEG_FORCE_DOWNLOAD "跳过系统检测，强制走下载流程（测试用）" OFF)
set(CONVERTER_FFMPEG_DIR  "${CMAKE_BINARY_DIR}/ffmpeg-bundle" CACHE PATH
    "ffmpeg 下载与解包目录")
set(CONVERTER_FFMPEG_EXE  "" CACHE FILEPATH
    "显式指定 ffmpeg 可执行文件（优先于 PATH 检测）")
set(CONVERTER_FFPROBE_EXE "" CACHE FILEPATH
    "显式指定 ffprobe 可执行文件")
set(CONVERTER_FFMPEG_URL  "" CACHE STRING
    "覆盖 ffmpeg 下载地址（内网镜像/测试用）")

# ---- MSVC：静态 CRT，产物不依赖 VC 运行库 ----
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---- GUI 第三方依赖：ImGui + nfd（Linux 追加 GLFW）----
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildGuiDeps.cmake")
converter_setup_gui_deps()

# ---- 跨平台核心 ----
add_library(conv_core STATIC src/converter.cpp)
target_include_directories(conv_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(MSVC)
    target_compile_definitions(conv_core PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(conv_core PRIVATE /utf-8 /W3)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(conv_core PRIVATE -Wall -Wextra)
endif()

# ---- 平台入口 ----
if(WIN32)
    # Windows: MSVC + ImGui（win32 + DX11）；双击为 GUI；带参数运行时自动切换命令行模式
    add_executable(Converter WIN32 src/gui_imgui.cpp src/app.manifest)
    target_compile_definitions(Converter PRIVATE
        UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX)
    target_compile_options(Converter PRIVATE /utf-8 /W3)
    # d3dcompiler=DX11 后端运行时编译着色器；dwmapi/imm32=imgui_impl_win32 所需；ole32/uuid=nfd COM
    target_link_libraries(Converter PRIVATE conv_core imgui nfd
        d3d11 dxgi d3dcompiler dwmapi imm32 ole32 uuid)
    target_link_options(Converter PRIVATE /ENTRY:wWinMainCRTStartup)
    set(CONVERTER_MAIN_TARGET Converter)
else()
    # Linux: GCC + CLI
    add_executable(converter-cli src/cli_main.cpp)
    target_compile_options(converter-cli PRIVATE -Wall -Wextra)
    target_link_libraries(converter-cli PRIVATE conv_core)

    # Linux: GLFW + OpenGL3 GUI
    add_executable(converter-gui src/gui_imgui.cpp)
    target_compile_options(converter-gui PRIVATE -Wall -Wextra)
    target_link_libraries(converter-gui PRIVATE conv_core imgui nfd)
    set(CONVERTER_MAIN_TARGET converter-gui)
endif()

# ---- ffmpeg：检测 → （必要时）下载 → 打包到可执行文件目录 ----
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FetchFFmpeg)
converter_bundle_ffmpeg(${CONVERTER_MAIN_TARGET})

# ---- install：安装可执行文件 + 随附的 ffmpeg/ffprobe（运行时优先使用同目录副本） ----
include(GNUInstallDirs)
install(TARGETS ${CONVERTER_MAIN_TARGET}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(NOT WIN32)
    install(TARGETS converter-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(CONVERTER_BUNDLED_FFMPEG)
    install(FILES "${CONVERTER_BUNDLED_FFMPEG}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(CONVERTER_BUNDLED_FFPROBE)
    install(FILES "${CONVERTER_BUNDLED_FFPROBE}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---- clang-format：format / check-format 手动 target + 构建时自动格式化 ----
find_program(CLANG_FORMAT_EXE NAMES clang-format)
if(CLANG_FORMAT_EXE)
    file(GLOB_RECURSE CONVERTER_FORMAT_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp")

    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXE} -i -style=file ${CONVERTER_FORMAT_SOURCES}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Running clang-format on all source files"
    )
    add_custom_target(check-format
        COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror -style=file ${CONVERTER_FORMAT_SOURCES}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Checking clang-format compliance"
    )

    # 构建前自动格式化：先 dry-run 检测，仅在存在未格式化文件时才 -i（不触发全量重编）
    add_custom_target(format_on_build ALL
        COMMAND ${CMAKE_COMMAND}
                -DCLANG_FORMAT_EXE=${CLANG_FORMAT_EXE}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ClangFormatOnBuild.cmake"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Auto-formatting sources with clang-format if needed"
    )
else()
    message(STATUS "clang-format not found — skipping format targets")
endif()
