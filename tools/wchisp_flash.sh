#!/bin/bash
# ===================================================================================
# wchisp_flash.sh
# ===================================================================================
# USB DFU bootloader (WCH ISP) でファームを書き込む。
# 通常 wchisp は実行時にデバイスがいなければ即座に失敗するが、
# BOOT 押下中の DFU モード保持時間 (~5 秒) と人間の操作タイミングが合わないことが
# 多いので、デバイスが現れるまでリトライし続ける。
#
# 使い方:
#   tools/wchisp_flash.sh                  # joystick env (default)
#   tools/wchisp_flash.sh x68k_keyboard    # 任意 env
#   tools/wchisp_flash.sh path/to/foo.bin  # bin 直接指定
#
# 操作手順:
#   1) このスクリプトを実行
#   2) 「Waiting for DFU device...」と出たら、BOOT ボタンを押しながら USB を抜き挿し
#   3) DFU 検出 → 書き込み開始
# ===================================================================================
set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WCHISP="${HOME}/.platformio/packages/tool-wchisp/wchisp"

# ---- 引数解決 -----------------------------------------------------------------
ARG="${1:-joystick}"
if [ -f "$ARG" ]; then
    BIN="$ARG"
else
    BIN="${REPO_ROOT}/.pio/build/${ARG}/firmware.bin"
fi

# ---- 前提チェック -------------------------------------------------------------
if [ ! -x "$WCHISP" ]; then
    echo "ERROR: wchisp not found at $WCHISP" >&2
    echo "       PlatformIO の tool-wchisp パッケージを確認してください" >&2
    exit 1
fi

if [ ! -f "$BIN" ]; then
    echo "ERROR: firmware bin not found: $BIN" >&2
    echo "       先に \`pio run -e <env>\` でビルドしてください" >&2
    exit 1
fi

echo "wchisp:   $WCHISP"
echo "bin:      $BIN ($(wc -c <"$BIN" | tr -d ' ') bytes)"
echo ""
echo "Waiting for DFU device... BOOT ボタンを押しながら USB を抜き挿ししてください"
echo "(Ctrl-C で中断)"
echo ""

# ---- リトライループ -----------------------------------------------------------
# DFU 窓は ~5 秒なので polling は短く (0.3s) して窓に当てる確率を上げる。
# 上限 300 回 = 約 90 秒、それを超えたら諦める。
MAX_ATTEMPTS=300
SLEEP_S=0.3

for i in $(seq 1 $MAX_ATTEMPTS); do
    OUT=$("$WCHISP" flash "$BIN" 2>&1)
    RC=$?
    if [ $RC -eq 0 ]; then
        echo ""
        echo "$OUT"
        echo ""
        echo "✓ Flash succeeded ($i attempt(s))"
        exit 0
    fi
    # 「device not found」以外のエラーは即座に止める (USB 検出後の本物のエラー)
    if ! echo "$OUT" | grep -qi "device not found"; then
        echo ""
        echo "$OUT" >&2
        echo "ERROR: unexpected wchisp error, aborting" >&2
        exit 1
    fi
    printf "."
    sleep "$SLEEP_S"
done

echo ""
echo "ERROR: timed out after $MAX_ATTEMPTS attempts (~$(echo "$MAX_ATTEMPTS * $SLEEP_S" | bc)s)" >&2
echo "  デバイスが DFU モードに入っていない可能性があります" >&2
echo "  - USB-C ケーブルがデータ線対応か確認" >&2
echo "  - BOOT ボタンを USB 挿入後 1 秒程度押し続ける" >&2
exit 1
