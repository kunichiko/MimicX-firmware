// ===================================================================================
// ch32_ota.c  (ESP32 / CH32 ファーム自動更新オーケストレーション)
// ===================================================================================
#include "ch32_ota.h"
#include "i2c_bridge.h"
#include "ch32_swd.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ch32_ota";

// CH32 user flash 先頭。
#define CH32_FLASH_BASE 0x08000000

// 内蔵イメージ (EMBED_FILES, CMakeLists)。今は joystick-i2c のみ。
// keyboard/combined の *-i2c 版ができたら同様に追加する。
//
// 再生成手順 (CH32 を更新したら必ず実施):
//   pio run -d platforms/ch32x035 -e joystick-i2c
//   cp platforms/ch32x035/.pio/build/joystick-i2c/firmware.bin \
//      platforms/esp32/main/ch32_joy_i2c.bin
//   → 下の IMAGES[] の version も CH32 の FW_VERSION に合わせて更新する。
extern const uint8_t joy_i2c_start[] asm("_binary_ch32_joy_i2c_bin_start");
extern const uint8_t joy_i2c_end[]   asm("_binary_ch32_joy_i2c_bin_end");

typedef struct {
    const char    *board_name;        // IDENTIFY の BOARD_NAME と一致させる
    uint8_t        maj, min, patch;   // 内蔵イメージのバージョン
    const uint8_t *start, *end;       // 埋め込みバイナリ
} ota_image_t;

// ⚠ version は埋め込み bin (ch32_joy_i2c.bin) の FW_VERSION と一致させること。
static const ota_image_t IMAGES[] = {
    { "mimic-x-joy", 0, 8, 2, joy_i2c_start, joy_i2c_end },
};

// a - b 相当 (>0: a が新しい)
static int ver_cmp(uint8_t aM, uint8_t am, uint8_t ap,
                   uint8_t bM, uint8_t bm, uint8_t bp) {
    if (aM != bM) return (int)aM - bM;
    if (am != bm) return (int)am - bm;
    return (int)ap - bp;
}

void ch32_ota_check_and_update(void (*rx_cb)(const uint8_t* midi, int len)) {
    // 1) IDENTIFY を I2C 同期取得 (F0 7D 01 01 F7)。
    static const uint8_t idreq[] = { 0xF0, 0x7D, 0x01, 0x01, 0xF7 };
    uint8_t resp[64];
    int n = i2c_bridge_request(idreq, sizeof(idreq), resp, sizeof(resp), 500);
    if (n < 11 || resp[0] != 0xF0 || resp[3] != 0x02) {
        ESP_LOGW(TAG, "IDENTIFY 取得失敗 (n=%d) - OTA スキップ", n);
        return;
    }

    // 2) variant (BOARD_NAME) と version を取り出す。
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

    // 3) テーブル照合 + 版判定。
    for (unsigned k = 0; k < sizeof(IMAGES) / sizeof(IMAGES[0]); k++) {
        const ota_image_t *im = &IMAGES[k];
        if (strcmp(name, im->board_name) != 0) continue;

        if (ver_cmp(im->maj, im->min, im->patch, rmaj, rmin, rpatch) <= 0) {
            ESP_LOGI(TAG, "最新 (内蔵 %d.%d.%d <= 実機 %d.%d.%d) - 更新不要",
                     im->maj, im->min, im->patch, rmaj, rmin, rpatch);
            return;
        }

        // 4) 更新 (ハンドオフ): ENTER_SWD → I2C teardown → SWD 書込 → reboot → ブリッジ復帰
        size_t len = (size_t)(im->end - im->start);
        ESP_LOGI(TAG, "更新開始: %s %d.%d.%d -> %d.%d.%d (%u bytes)",
                 name, rmaj, rmin, rpatch, im->maj, im->min, im->patch, (unsigned)len);

        static const uint8_t enter_swd[] = { 0xF0, 0x7D, 0x01, 0x7E, 0x00, 0xF7 };
        i2c_bridge_write(enter_swd, sizeof(enter_swd));
        vTaskDelay(pdMS_TO_TICKS(100));      // CH32 が SDI へ切替える猶予
        i2c_bridge_deinit();                 // SDA/SCL を解放

        bool ok = ch32_swd_flash(CH32_FLASH_BASE, im->start, len);
        ESP_LOGI(TAG, "更新 %s", ok ? "成功" : "失敗");

        vTaskDelay(pdMS_TO_TICKS(300));      // CH32 reboot → I2C スレーブで起き直る猶予
        i2c_bridge_init(rx_cb);              // ブリッジ復帰
        return;
    }

    ESP_LOGI(TAG, "内蔵イメージ該当なし (%s) - OTA スキップ", name);
}
