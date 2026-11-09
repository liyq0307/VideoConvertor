# cmake/FetchFFmpeg.cmake
#
# converter_bundle_ffmpeg(<target>)
#   1) 检测系统中的 ffmpeg / ffprobe
#      （缓存变量 CONVERTER_FFMPEG_EXE / CONVERTER_FFPROBE_EXE 可显式指定，优先于 PATH 检测）
#   2) 检测不到且 CONVERTER_FFMPEG_DOWNLOAD=ON 时，下载对应平台的静态构建：
#      · Windows x64:  BtbN/FFmpeg-Builds (github)
#      · Linux x86_64 / aarch64: johnvansickle.com 静态版
#      （macOS 不支持本项目）
#   3) 构建完成后把 ffmpeg / ffprobe 复制到目标可执行文件同目录，
#      程序运行时优先使用同目录副本，从而“编译时决定、运行时零配置”。

function(converter_bundle_ffmpeg TARGET)
    set(_dir "${CONVERTER_FFMPEG_DIR}")
    file(MAKE_DIRECTORY "${_dir}")

    set(_ffmpeg "")
    set(_ffprobe "")

    # ---- 1) 检测 ----
    if(NOT CONVERTER_FFMPEG_FORCE_DOWNLOAD)
        if(CONVERTER_FFMPEG_EXE)
            set(_ffmpeg "${CONVERTER_FFMPEG_EXE}")
        else()
            find_program(CONV_FFMPEG_SYSTEM ffmpeg)
            if(CONV_FFMPEG_SYSTEM)
                set(_ffmpeg "${CONV_FFMPEG_SYSTEM}")
            endif()
        endif()
        if(CONVERTER_FFPROBE_EXE)
            set(_ffprobe "${CONVERTER_FFPROBE_EXE}")
        else()
            find_program(CONV_FFPROBE_SYSTEM ffprobe)
            if(CONV_FFPROBE_SYSTEM)
                set(_ffprobe "${CONV_FFPROBE_SYSTEM}")
            endif()
        endif()
    endif()

    # ---- 2) 下载 ----
    if(NOT _ffmpeg AND CONVERTER_FFMPEG_DOWNLOAD)
        set(_url "")
        set(_archive "")
        if(WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|x64")
            set(_url "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip")
            set(_archive "ffmpeg-win64.zip")
        elseif(UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            set(_url "https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz")
            set(_archive "ffmpeg-linux-amd64.tar.xz")
        elseif(UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
            set(_url "https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-arm64-static.tar.xz")
            set(_archive "ffmpeg-linux-arm64.tar.xz")
        endif()

        if(CONVERTER_FFMPEG_URL)
            set(_url "${CONVERTER_FFMPEG_URL}")  # 允许覆盖下载地址（内网镜像/测试）
        endif()

        if(_url STREQUAL "")
            message(WARNING "[ffmpeg] 平台 \"${CMAKE_SYSTEM_PROCESSOR}\" 暂无自动下载源，"
                            "请自行安装 ffmpeg 或设置 -DCONVERTER_FFMPEG_EXE=<路径>")
        else()
            set(_dest "${_dir}/${_archive}")
            if(NOT EXISTS "${_dest}")
                message(STATUS "[ffmpeg] 未检测到系统 ffmpeg，下载静态构建: ${_url}")
                file(DOWNLOAD "${_url}" "${_dest}" SHOW_PROGRESS STATUS _st TLS_VERIFY ON)
                list(GET _st 0 _code)
                if(NOT _code EQUAL 0)
                    file(REMOVE "${_dest}")
                    message(FATAL_ERROR "[ffmpeg] 下载失败: ${_st}")
                endif()
            else()
                message(STATUS "[ffmpeg] 使用已下载的包: ${_dest}")
            endif()

            set(_xdir "${_dir}/extract-${_archive}")
            if(NOT EXISTS "${_xdir}")
                file(MAKE_DIRECTORY "${_xdir}")
                message(STATUS "[ffmpeg] 解压 ${_dest}")
                file(ARCHIVE_EXTRACT INPUT "${_dest}" DESTINATION "${_xdir}")
            endif()

            file(GLOB_RECURSE _bins LIST_DIRECTORIES false "${_xdir}/*")
            foreach(_f IN LISTS _bins)
                get_filename_component(_n "${_f}" NAME)
                if(_n STREQUAL "ffmpeg.exe" OR _n STREQUAL "ffmpeg")
                    set(_ffmpeg "${_f}")
                elseif(_n STREQUAL "ffprobe.exe" OR _n STREQUAL "ffprobe")
                    set(_ffprobe "${_f}")
                endif()
            endforeach()
        endif()
    endif()

    # ---- 3) 汇报 ----
    if(NOT _ffmpeg)
        message(WARNING
            "[ffmpeg] 未找到 ffmpeg —— 程序运行时需自行提供："
            "放到 exe 同目录、加入 PATH，或在 GUI 中手动指定。"
            "（或改用 -DCONVERTER_FFMPEG_DOWNLOAD=ON 重新配置）")
        set(CONVERTER_BUNDLED_FFMPEG "" PARENT_SCOPE)
        set(CONVERTER_BUNDLED_FFPROBE "" PARENT_SCOPE)
        return()
    endif()
    message(STATUS "[ffmpeg] 使用: ${_ffmpeg}")
    if(_ffprobe)
        message(STATUS "[ffmpeg] 使用: ${_ffprobe}")
    else()
        message(STATUS "[ffmpeg] 未找到 ffprobe（进度条将退化为滚动模式）")
    endif()

    # ---- 4) 打包到可执行文件目录 ----
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_ffmpeg}" "$<TARGET_FILE_DIR:${TARGET}>"
        COMMENT "[ffmpeg] 打包 ffmpeg 到输出目录" VERBATIM)
    if(_ffprobe)
        add_custom_command(TARGET ${TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_ffprobe}" "$<TARGET_FILE_DIR:${TARGET}>"
            COMMENT "[ffmpeg] 打包 ffprobe 到输出目录" VERBATIM)
    endif()

    # 暴露给顶层 CMakeLists 的 install 规则使用
    set(CONVERTER_BUNDLED_FFMPEG "${_ffmpeg}" PARENT_SCOPE)
    set(CONVERTER_BUNDLED_FFPROBE "${_ffprobe}" PARENT_SCOPE)
endfunction()
