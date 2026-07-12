// ===================================================================================
// mimicx_version.h  —  MimicX ファームウェアの版数「単一ソース」
// ===================================================================================
// CH32 / ESP32(esp32/c3/c6) の両方がこの 1 ファイルを参照する。リリース版数は
// ここだけを書き換える。
//
//   - CH32 : main.c が include し FW_VERSION_* として使う (IDENTIFY で申告)。
//   - ESP32: CMake (common/mimicx_version.cmake) がこの値を解析し、
//            アプリ版数 PROJECT_VER と、内包 CH32 イメージの照合版数に注入する。
//
// リリース手順: この 3 値を上げてコミット → 同じ版数のタグ (vX.Y.Z) を push。
// タグと本ファイルの不一致は GitHub Actions がビルドエラーで弾く
// (.github/workflows/firmware-release.yml の "Verify version" ステップ)。
//
// ★ PROTOCOL_VERSION は別物 (配線/SysEx 互換の版数)。本ファイルには含めない。
// ===================================================================================
#pragma once

#define MIMICX_VERSION_MAJOR 0
#define MIMICX_VERSION_MINOR 9
#define MIMICX_VERSION_PATCH 5
