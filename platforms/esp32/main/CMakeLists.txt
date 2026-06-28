idf_component_register(
    SRCS
        "main.c"
        "ble_midi.c"
        "i2c_bridge.c"
        "ch32_swd.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash driver
)

# bitbang_rvswdio.h (vendored, CNLohr) は未使用の static 関数群を含むため、
# ch32_swd.c のみ該当警告を無効化する (-Werror 回避)。
set_source_files_properties(ch32_swd.c PROPERTIES
    COMPILE_FLAGS "-Wno-unused-function -Wno-unused-but-set-variable")
