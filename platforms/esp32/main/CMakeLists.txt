idf_component_register(
    SRCS
        "main.c"
        "ble_midi.c"
        "i2c_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash driver
)
