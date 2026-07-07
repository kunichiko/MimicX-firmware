# MimicX-ESP32-C6 (Seeed XIAO ESP32-C6) BLE-MIDI ファーム (ESP-IDF プロジェクト)
#
# ソースは platforms/esp32/main を「main コンポーネント」として流用する
# (同一バイナリでは動かないためプロジェクトは分離、差分は sdkconfig.defaults と
#  board_config.h のターゲット分岐のみ)。
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../esp32/main")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mimicx-esp32c6-ble)
