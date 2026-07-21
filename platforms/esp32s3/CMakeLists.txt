# MimicX-ESP32-S3 (Seeed XIAO ESP32-S3) BLE-MIDI + USB-MIDI ファーム (ESP-IDF プロジェクト)
#
# ソースは platforms/esp32/main を「main コンポーネント」として流用する
# (プロジェクト分離の理由と差分方針は esp32c6/ と同じ)。S3 固有の差分:
#   - USB-OTG による USB-MIDI デバイス化 (usb_midi_bridge.c, S3 ビルドのみ有効)
#   - ログ/コンソールは UART0 (OTG が USB ピンを占有するため。README 参照)
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../esp32/main")

# アプリ版数 (PROJECT_VER) を共有の単一ソース common/mimicx_version.h から設定する。
include("${CMAKE_CURRENT_LIST_DIR}/../common/mimicx_version.cmake")
set(PROJECT_VER "${MIMICX_VERSION}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mimicx-esp32s3-ble-usb)
