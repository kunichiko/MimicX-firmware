# MimicX-ESP32 BLE-MIDI ファーム (ESP-IDF プロジェクト)
cmake_minimum_required(VERSION 3.16)

# アプリ版数 (PROJECT_VER) を共有の単一ソース common/mimicx_version.h から設定する。
# CH32 firmware も同じヘッダ由来なので、ESP アプリ版数と CH32 版数は常に一致する。
include("${CMAKE_CURRENT_LIST_DIR}/../common/mimicx_version.cmake")
set(PROJECT_VER "${MIMICX_VERSION}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mimicx-esp32-ble)
