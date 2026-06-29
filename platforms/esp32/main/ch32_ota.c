// ===================================================================================
// ch32_ota.c  (ESP32 / CH32 ファーム強制同期)
// ===================================================================================
// ESP32 は単一 variant の CH32 イメージを内包する (ch32_image_config.h)。起動時に
// 接続中の CH32 を調べ、「内包イメージと完全一致 (variant + version) でなければ」
// 強制的に書き込む。量産時の空チップや、版・種類の食い違いをまとめて吸収する。
//
// 強制書込の条件 (いずれか):
//   - CH32 が I2C 応答なし (空チップ / 異常)        → SWD 直書き (空チップは SDI ON)
//   - variant (BOARD_NAME) が内包イメージと異なる   → ENTER_SWD ハンドオフ後に書込
//   - version が内包イメージと異なる (新旧どちらでも) → 同上 (ダウングレードも許容)
// ===================================================================================
#include "ch32_ota.h"
#include "ch32_image_config.h"
#include "i2c_bridge.h"
#include "ch32_swd.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ch32_ota";

#define CH32_FLASH_BASE 0x08000000

// 強制書込を実行する。via_handoff=true なら ENTER_SWD で SDI を起こしてから書く
// (CH32 が I2C で正常動作中=SDI off のケース)。false なら SWD 直 (空チップ=SDI on)。
static void do_flash(void (*rx_cb)(const uint8_t*, int), bool via_handoff) {
    const uint8_t *blob = ch32_img_start;
    size_t len = (size_t)(ch32_img_end - ch32_img_start);

    if (via_handoff) {
        static const uint8_t enter_swd[] = { 0xF0, 0x7D, 0x01, 0x7E, 0x00, 0xF7 };
        i2c_bridge_write(enter_swd, sizeof(enter_swd));
        vTaskDelay(pdMS_TO_TICKS(100));   // CH32 が SDI へ切替える猶予
    }
    i2c_bridge_deinit();                  // SDA/SCL を解放

    bool ok = ch32_swd_flash(CH32_FLASH_BASE, blob, len);
    if (ok) {
        ESP_LOGI(TAG, "書込成功: %s %d.%d.%d (%u bytes)",
                 CH32_IMG_NAME, CH32_IMG_MAJ, CH32_IMG_MIN, CH32_IMG_PATCH, (unsigned)len);
    } else if (via_handoff) {
        ESP_LOGE(TAG, "書込失敗 (ENTER_SWD 後)。CH32 が SDI へ移行していない可能性");
    } else {
        ESP_LOGE(TAG, "書込失敗 (SWD 直)。CH32 未接続 or SDI off の異常ファーム。BOOT+USB 復旧が必要");
    }

    vTaskDelay(pdMS_TO_TICKS(300));       // CH32 reboot → I2C スレーブで起き直る猶予
    i2c_bridge_init(rx_cb);               // ブリッジ復帰
}

void ch32_ota_check_and_update(void (*rx_cb)(const uint8_t* midi, int len)) {
    ESP_LOGI(TAG, "内包イメージ: %s %d.%d.%d",
             CH32_IMG_NAME, CH32_IMG_MAJ, CH32_IMG_MIN, CH32_IMG_PATCH);

    // 1) IDENTIFY を I2C 同期取得 (F0 7D 01 01 F7)。応答 cmd=0x02 のフレームだけ捕捉
    //    (古い TARGET_RX 等は読み捨て)。起動直後の CH32 未準備に備えてリトライする。
    static const uint8_t idreq[] = { 0xF0, 0x7D, 0x01, 0x01, 0xF7 };
    uint8_t resp[64];
    int n = -1;
    for (int attempt = 0; attempt < 5; attempt++) {
        n = i2c_bridge_request(idreq, sizeof(idreq), /*match_cmd=*/0x02,
                               resp, sizeof(resp), 300);
        if (n >= 11 && resp[0] == 0xF0 && resp[3] == 0x02) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 2) IDENTIFY が取れない場合の分岐:
    //    - I2C アドレスを ACK する  → 稼働中ファーム (SDI off)。ENTER_SWD ハンドオフで書込
    //    - ACK しない               → 空チップ/未接続。SWD 直書き (空チップは SDI on)
    if (n < 11 || resp[0] != 0xF0 || resp[3] != 0x02) {
        bool present = i2c_bridge_probe();
        ESP_LOGW(TAG, "IDENTIFY 取得失敗 (n=%d, addr_ack=%d) - %s",
                 n, present, present ? "稼働中とみなし ENTER_SWD ハンドオフ書込" : "空チップとみなし SWD 直書き");
        do_flash(rx_cb, /*via_handoff=*/present);
        return;
    }

    // 3) variant (BOARD_NAME) と version を取り出す。
    //    [6..8]=fw maj/min/patch, [9]=num_ch, [10..]=ch_map(num_ch*3), +serial[16], +name, F7
    uint8_t rmaj = resp[6], rmin = resp[7], rpatch = resp[8];
    int numch = resp[9];
    int name_start = 10 + numch * 3 + 16;
    char name[24] = {0};
    int nl = 0;
    for (int i = name_start; i < n - 1 && nl < (int)sizeof(name) - 1; i++) {
        name[nl++] = (char)resp[i];
    }
    ESP_LOGI(TAG, "CH32 検出: '%s' fw %d.%d.%d", name, rmaj, rmin, rpatch);

    // 4) 完全一致 (variant + version) でなければ強制書込 (新旧どちらでも上書き)。
    bool variant_ok = (strcmp(name, CH32_IMG_NAME) == 0);
    bool version_ok = (rmaj == CH32_IMG_MAJ && rmin == CH32_IMG_MIN && rpatch == CH32_IMG_PATCH);
    if (variant_ok && version_ok) {
        ESP_LOGI(TAG, "一致 - 書込不要");
        return;
    }

    ESP_LOGI(TAG, "不一致 (%s%s) - 強制書込 -> %s %d.%d.%d",
             variant_ok ? "" : "variant ",
             version_ok ? "" : "version ",
             CH32_IMG_NAME, CH32_IMG_MAJ, CH32_IMG_MIN, CH32_IMG_PATCH);
    do_flash(rx_cb, /*via_handoff=*/true);
}
