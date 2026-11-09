# ClangFormatOnBuild.cmake — 脚本模式：构建前检查源码是否符合 .clang-format，
# 仅当存在未格式化文件时才执行 clang-format -i（避免每次构建改写 mtime 导致全量重编）。
# 传入变量：CLANG_FORMAT_EXE = clang-format 可执行文件路径

if(NOT CLANG_FORMAT_EXE)
    message(FATAL_ERROR "ClangFormatOnBuild.cmake 需要 -DCLANG_FORMAT_EXE=<路径>")
endif()

set(_src_dir "${CMAKE_CURRENT_LIST_DIR}/../src")
file(GLOB_RECURSE _sources
    "${_src_dir}/*.cpp"
    "${_src_dir}/*.cc"
    "${_src_dir}/*.h"
    "${_src_dir}/*.hpp")
if(_sources STREQUAL "")
    return()
endif()

# --dry-run -Werror：文件均已格式化时退出码为 0
execute_process(
    COMMAND "${CLANG_FORMAT_EXE}" --dry-run -Werror -style=file ${_sources}
    OUTPUT_QUIET ERROR_QUIET
    RESULT_VARIABLE _check_result)
if(_check_result EQUAL 0)
    return()
endif()

execute_process(
    COMMAND "${CLANG_FORMAT_EXE}" -i -style=file ${_sources}
    OUTPUT_QUIET ERROR_QUIET
    RESULT_VARIABLE _fmt_result)
if(NOT _fmt_result EQUAL 0)
    message(WARNING "clang-format -i 执行失败（退出码 ${_fmt_result}），本次跳过自动格式化")
    return()
endif()
message(STATUS "clang-format: 已自动格式化 src/ 下未符合规范的源文件")
