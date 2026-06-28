idf_component_register(
    SRCS
        "main.c"
        "ble_midi.c"
        "i2c_bridge.c"
        "ch32_swd.c"
        "ch32_ota.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash driver
    # 内蔵 CH32 ファームイメージ (OTA 用)。variant ごとに追加していく。
    EMBED_FILES "ch32_joy_i2c.bin"
)

# bitbang_rvswdio.h (vendored, CNLohr) は未使用の static 関数群を含むため、
# ch32_swd.c のみ該当警告を無効化する (-Werror 回避)。
set_source_files_properties(ch32_swd.c PROPERTIES
    COMPILE_FLAGS "-Wno-unused-function -Wno-unused-but-set-variable")
