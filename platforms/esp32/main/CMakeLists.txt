# ESP32 が内包する CH32 イメージの variant を選択する (joystick/keyboard/combined)。
# 例: idf.py -DMIMICX_CH32_VARIANT=joystick build  (既定 joystick)
if(NOT DEFINED MIMICX_CH32_VARIANT)
    set(MIMICX_CH32_VARIANT "joystick")
endif()
if(MIMICX_CH32_VARIANT STREQUAL "joystick")
    set(CH32_EMBED "ch32_joy_i2c.bin")
    set(CH32_DEF   "MIMICX_CH32_JOYSTICK=1")
elseif(MIMICX_CH32_VARIANT STREQUAL "keyboard")
    set(CH32_EMBED "ch32_kbd_i2c.bin")
    set(CH32_DEF   "MIMICX_CH32_KEYBOARD=1")
elseif(MIMICX_CH32_VARIANT STREQUAL "combined")
    set(CH32_EMBED "ch32_combo_i2c.bin")
    set(CH32_DEF   "MIMICX_CH32_COMBINED=1")
else()
    message(FATAL_ERROR "MIMICX_CH32_VARIANT は joystick/keyboard/combined のいずれか: ${MIMICX_CH32_VARIANT}")
endif()
message(STATUS "MimicX: 内包 CH32 イメージ variant = ${MIMICX_CH32_VARIANT} (${CH32_EMBED})")

# 内包 CH32 イメージの照合版数は「単一ソース」= common/mimicx_version.h から取得する。
# CH32 firmware も同じヘッダを FW_VERSION_* として参照するため、両者は常に一致する。
# これにより版数を手書き同期していた頃の漏れ (0.9.2 のまま → 毎起動で強制再書込) を防ぐ。
include("${CMAKE_CURRENT_LIST_DIR}/../../common/mimicx_version.cmake")
# ヘッダが変わったら再 configure して版数を取り直す
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../../common/mimicx_version.h")
message(STATUS "MimicX: 内包 CH32 照合版数 = ${MIMICX_VERSION} (from common/mimicx_version.h)")

idf_component_register(
    SRCS
        "main.c"
        "ble_midi.c"
        "i2c_bridge.c"
        "ch32_swd.c"
        "ch32_ota.c"
    INCLUDE_DIRS "." "../../common"
    REQUIRES bt nvs_flash driver
    # 内蔵 CH32 ファームイメージ (variant により切替)。
    EMBED_FILES "${CH32_EMBED}"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ${CH32_DEF}
    CH32_IMG_VER_MAJ=${MIMICX_VER_MAJOR}
    CH32_IMG_VER_MIN=${MIMICX_VER_MINOR}
    CH32_IMG_VER_PATCH=${MIMICX_VER_PATCH})

# bitbang_rvswdio.h (vendored, CNLohr) は未使用の static 関数群を含むため、
# ch32_swd.c のみ該当警告を無効化する (-Werror 回避)。
set_source_files_properties(ch32_swd.c PROPERTIES
    COMPILE_FLAGS "-Wno-unused-function -Wno-unused-but-set-variable")
