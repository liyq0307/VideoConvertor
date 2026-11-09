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

        # GLFW 3.4 只要检测到 Wayland 依赖就默认启用 Wayland 后端，但在缺少
        # wayland-scanner 的机器上会直接配置失败。因此仅在 wayland-scanner 可用时启用，
        # 否则退回纯 X11（X11 始终开启）。
        find_program(CONVERTER_WAYLAND_SCANNER wayland-scanner)
        if(CONVERTER_GLFW_WAYLAND AND CONVERTER_WAYLAND_SCANNER)
            set(GLFW_BUILD_WAYLAND ON CACHE BOOL "" FORCE)
            message(STATUS "GLFW: 启用 Wayland 后端（${CONVERTER_WAYLAND_SCANNER}）")
        else()
            set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
            message(STATUS "GLFW: 未找到 wayland-scanner，仅启用 X11 后端")
        endif()
        set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)

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
