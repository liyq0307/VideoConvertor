# cmake/BuildGuiDeps.cmake
#
# converter_setup_gui_deps()
#   GUI 第三方依赖接入（ImGui + nativefiledialog-extended，Linux 追加 GLFW）：
#   · Dear ImGui v1.93.0：仓库无 CMakeLists，FetchContent 拉源码后自建 STATIC target `imgui`，
#     1.92+ 动态字体按需加载 CJK 字形（无需手工 glyph ranges）
#   · nativefiledialog-extended v1.2.1：自带 CMake，STATIC target `nfd`（Windows: IFileDialog；
#     Linux 默认 GTK3，可 -DNFD_PORTAL=ON 切 xdg-desktop-portal）
#   · Linux: GLFW 3.4（imgui_impl_glfw 所需），FetchContent 编 STATIC

function(converter_setup_gui_deps)
    include(FetchContent)

    # Linux: 是否在 wayland-scanner 可用时启用 GLFW 的 Wayland 后端（否则纯 X11）
    option(CONVERTER_GLFW_WAYLAND "GLFW 启用 Wayland 后端（需 wayland-scanner）" ON)

    # ---- Dear ImGui ----
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.93.0
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(imgui)  # 无 CMakeLists → 仅 populate 源码

    add_library(imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp")
    target_include_directories(imgui PUBLIC
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends")
    # 不编 imgui_demo.cpp：/W3 下告警噪音多且无用

    if(WIN32)
        target_sources(imgui PRIVATE
            "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
            "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp")
    else()
        set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

        # ---- 窗口系统后端探测：X11 / Wayland 至少可用其一 ----
        # GLFW 3.4 的后端是硬依赖：启用了却缺开发包就会直接 CMake Error。
        # 这里先在外部探测，只启用真正可编译的后端；两者都没有时给出明确的安装提示。
        find_package(X11 QUIET)
        set(_glfw_x11 OFF)
        if(X11_FOUND AND X11_Xrandr_FOUND AND X11_Xinerama_FOUND AND X11_Xcursor_FOUND AND X11_Xi_FOUND)
            set(_glfw_x11 ON)
        endif()

        set(_glfw_wayland OFF)
        if(CONVERTER_GLFW_WAYLAND)
            find_program(CONVERTER_WAYLAND_SCANNER wayland-scanner)
            find_package(PkgConfig QUIET)
            if(CONVERTER_WAYLAND_SCANNER AND PkgConfig_FOUND)
                pkg_check_modules(CONVERTER_WAYLAND_CLIENT QUIET
                    wayland-client wayland-cursor wayland-egl xkbcommon)
                if(CONVERTER_WAYLAND_CLIENT_FOUND)
                    set(_glfw_wayland ON)
                endif()
            endif()
        endif()

        if(NOT _glfw_x11 AND NOT _glfw_wayland)
            message(FATAL_ERROR
                "未找到可用的窗口系统开发包，GUI 无法构建。请安装其中之一：\n"
                "  · X11     : sudo apt install libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev（或 xorg-dev）\n"
                "  · Wayland : sudo apt install libwayland-dev wayland-protocols libwayland-bin libxkbcommon-dev")
        endif()

        set(GLFW_BUILD_X11 ${_glfw_x11} CACHE BOOL "" FORCE)
        set(GLFW_BUILD_WAYLAND ${_glfw_wayland} CACHE BOOL "" FORCE)
        if(_glfw_x11)
            message(STATUS "GLFW: 启用 X11 后端")
        endif()
        if(_glfw_wayland)
            message(STATUS "GLFW: 启用 Wayland 后端（${CONVERTER_WAYLAND_SCANNER}）")
        endif()

        FetchContent_Declare(glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG        3.4
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(glfw)
        target_sources(imgui PRIVATE
            "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
            "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
        find_package(OpenGL REQUIRED)
        target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
    endif()

    # 第三方源不参与主工程的告警等级
    if(MSVC)
        target_compile_options(imgui PRIVATE /W0)
        target_compile_definitions(imgui PRIVATE IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    else()
        target_compile_options(imgui PRIVATE -w)
    endif()

    # ---- nativefiledialog-extended ----
    # Linux 默认用 GTK3 实现（需 libgtk-3-dev）；无 GTK3 开发环境时可开 PORTAL（走 xdg-desktop-portal）
    option(CONVERTER_NFD_PORTAL "nfd 使用 xdg-desktop-portal 后端（Linux）" OFF)
    if(CONVERTER_NFD_PORTAL)
        set(NFD_PORTAL ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(nfd
        GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
        GIT_TAG        v1.2.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(nfd)
    if(MSVC)
        set_property(TARGET nfd PROPERTY COMPILE_OPTIONS /W0)
    else()
        set_property(TARGET nfd PROPERTY COMPILE_OPTIONS -w)
    endif()
endfunction()
