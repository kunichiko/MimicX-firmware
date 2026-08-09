#!/bin/sh
# ===================================================================================
# reboot_bridge.sh — ブリッジ (ESP32) を ROM ダウンロードモードで再起動させる (macOS)
# ===================================================================================
# BRIDGE_REBOOT_BOOTLOADER (protocol §6.4.6) を USB-MIDI で直接送る。基板上の
# BOOT ボタンを押さずに書き込みモードへ入れる。アプリを起動していなくても使える。
#
#   tools/reboot_bridge.sh                 # 既定の宛先 "Mimic X (Bridge)" へ
#   tools/reboot_bridge.sh "別の名前"       # 宛先名の部分一致を指定
#
# 成功すると ACK (F0 7D 01 06 <req_id> 00 0C F7) が表示され、約 300ms 後に
# デバイスが MIDI から消えて "ESP32_S3" として再列挙される。その後:
#
#   idf.py -C platforms/esp32s3 -p /dev/cu.usbmodemXXXX flash
#
# 通常動作に戻すには書き込むか電源を入れ直す。
#
# 応答が無い場合はアダプタが固まっている可能性が高い (IDENTIFY にも無応答なら確定)。
# 抜き差しすれば復旧する。切り分けには midiprobe を直接使う:
#
#   tools/midiprobe "Mimic X (Bridge)" 2 F0 7D 01 01 F7   # IDENTIFY (CH32 が応答)
#   tools/midiprobe "Mimic X (Bridge)" 2 F0 7D 01 0A F7   # BRIDGE_IDENTIFY (ESP32 が自答)
# ===================================================================================
set -e
dir=$(cd "$(dirname "$0")" && pwd)
target=${1:-"Mimic X (Bridge)"}

# 初回のみビルド (swiftc は Xcode / Command Line Tools 同梱)
if [ ! -x "$dir/midiprobe" ] || [ "$dir/midiprobe.swift" -nt "$dir/midiprobe" ]; then
    echo "building midiprobe..."
    swiftc -O "$dir/midiprobe.swift" -o "$dir/midiprobe"
fi

# F0 7D 01 0C <req_id> "BOT" F7  — マジックが違うと INVALID_VALUE で拒否される
"$dir/midiprobe" "$target" 2 F0 7D 01 0C 01 42 4F 54 F7

echo
echo "--- 列挙の変化を待っています (最大 10 秒) ---"
# ROM ダウンロードモードの製品名は一通りではない。実測で少なくとも
#   "ESP32_S3"                    (OTG 経由で落ちたとき)
#   "USB JTAG_serial debug unit"  (USB Serial/JTAG 経由で落ちたとき)
# の 2 通りがあり、名前だけで判定すると入っているのに失敗と誤判定する
# (実際に誤判定して原因調査を誤った)。新しいシリアルポートの出現も併せて見る。
before=$(ls /dev/cu.usbmodem* 2>/dev/null | sort | tr '\n' ' ')
i=0
while [ $i -lt 20 ]; do
    names=$(ioreg -p IOUSB -w0 -l 2>/dev/null | grep -oE '"USB Product Name" = "[^"]*"')
    now=$(ls /dev/cu.usbmodem* 2>/dev/null | sort | tr '\n' ' ')
    if echo "$names" | grep -qE 'ESP32_S3|USB JTAG' || [ "$now" != "$before" ]; then
        echo "ダウンロードモードに入りました:"
        ls /dev/cu.usbmodem* 2>/dev/null
        echo "(esptool で接続確認するとより確実です)"
        exit 0
    fi
    sleep 0.5
    i=$((i + 1))
done
echo "ダウンロードモードに入りませんでした。"
echo "ACK が返っていないならアダプタが無応答 (抜き差しで復旧)。"
exit 1
