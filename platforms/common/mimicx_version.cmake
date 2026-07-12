# ===================================================================================
# mimicx_version.cmake  —  mimicx_version.h を解析して版数変数を提供する
# ===================================================================================
# include すると次を定義する:
#   MIMICX_VER_MAJOR / MIMICX_VER_MINOR / MIMICX_VER_PATCH  (整数)
#   MIMICX_VERSION                                          ("MAJOR.MINOR.PATCH")
#
# ESP32 側の 2 用途で使う:
#   - top-level CMakeLists: project() 前に set(PROJECT_VER "${MIMICX_VERSION}")
#   - main コンポーネント : 内包 CH32 イメージの照合版数 (CH32_IMG_VER_*) に注入
#
# 本ファイルが変わったら再 configure されるよう、呼び出し側は
# set_property(... CMAKE_CONFIGURE_DEPENDS mimicx_version.h) を付けること。
# ===================================================================================
set(_mimicx_version_h "${CMAKE_CURRENT_LIST_DIR}/mimicx_version.h")
if(NOT EXISTS "${_mimicx_version_h}")
    message(FATAL_ERROR "mimicx_version.h が見つからない: ${_mimicx_version_h}")
endif()
file(READ "${_mimicx_version_h}" _mimicx_version_src)
foreach(_part MAJOR MINOR PATCH)
    if(_mimicx_version_src MATCHES "#define[ \t]+MIMICX_VERSION_${_part}[ \t]+([0-9]+)")
        set(MIMICX_VER_${_part} "${CMAKE_MATCH_1}")
    else()
        message(FATAL_ERROR "MIMICX_VERSION_${_part} を ${_mimicx_version_h} から抽出できない")
    endif()
endforeach()
set(MIMICX_VERSION "${MIMICX_VER_MAJOR}.${MIMICX_VER_MINOR}.${MIMICX_VER_PATCH}")
