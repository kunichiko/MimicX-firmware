idf_component_register(
    SRCS
        "main.c"
        "ble_midi.c"
        "mimicx_proto.c"
        "joystick.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash driver
)
