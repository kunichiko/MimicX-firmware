// ===================================================================================
// i2c_bridge.c  (ESP32 / I2C マスター — CH32 デバイスエンジンへの橋渡し, protocol §2.5)
// ===================================================================================
#include "i2c_bridge.h"

#include <string.h>
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "board_config.h"

static const char *TAG = "i2c_bridge";

// --- 配線 (ボード別。ピンは board_config.h で切替) ---
#define I2C_SDA_GPIO   BOARD_I2C_SDA_GPIO
#define I2C_SCL_GPIO   BOARD_I2C_SCL_GPIO
#define I2C_INT_GPIO   BOARD_I2C_INT_GPIO   // CH32 PB1(INT) ← 入力
#define CH32_ADDR      0x33
#define I2C_HZ         400000

#define FRAME_MAX      64               // 1 フレーム最大 ([LEN] + payload)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t       s_mutex;     // バス排他 (write と read タスク)
static TaskHandle_t            s_reader;
static void (*s_on_rx)(const uint8_t* midi, int len);

// 同期リクエスト (IDENTIFY 等) 用の一時キャプチャ。capturing 中は reader_task が
// 受信フレームを s_on_rx ではなくこのバッファへ渡し、セマフォで通知する。
static volatile bool      s_capturing;
static uint8_t            s_cap_buf[FRAME_MAX];
static volatile int       s_cap_len;
static volatile uint8_t   s_cap_match;    // 捕捉したい応答の cmd (SysEx[3])。0=何でも可
static SemaphoreHandle_t  s_cap_sem;

// host→device: [LEN][MIDI] を CH32 へ書き込む。
void i2c_bridge_write(const uint8_t* midi, int len) {
    if (len <= 0 || len > FRAME_MAX - 1) return;
    uint8_t frame[FRAME_MAX];
    frame[0] = (uint8_t)len;
    memcpy(&frame[1], midi, len);
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    esp_err_t e = i2c_master_transmit(s_dev, frame, len + 1, 50);
    xSemaphoreGive(s_mutex);
    if (e != ESP_OK) ESP_LOGW(TAG, "write err %d", (int)e);
}

// 同期リクエスト: req を送り、CH32 の応答フレーム 1 件を resp に取得する。
// 戻り値 = 応答バイト数 (>0) / タイムアウト・エラーは <=0。OTA の IDENTIFY 取得に使う。
int i2c_bridge_request(const uint8_t* req, int reqlen, uint8_t match_cmd,
                       uint8_t* resp, int respmax, int timeout_ms) {
    if (!s_cap_sem) return -1;
    s_cap_len = 0;
    s_cap_match = match_cmd;           // この cmd の応答だけ捕捉 (古い TARGET_RX 等はスキップ)
    xSemaphoreTake(s_cap_sem, 0);     // 残留シグナルをクリア
    s_capturing = true;
    i2c_bridge_write(req, reqlen);
    int got = -1;
    if (xSemaphoreTake(s_cap_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        got = s_cap_len;
        if (got > respmax) got = respmax;
        memcpy(resp, s_cap_buf, got);
    }
    s_capturing = false;
    return got;
}

// CH32 (I2C スレーブ) がアドレスを ACK するか。稼働中ファーム判定に使う
// (空チップ/未接続は ACK しない)。init 後に呼ぶこと。
bool i2c_bridge_probe(void) {
    if (!s_bus) return false;
    return i2c_master_probe(s_bus, CH32_ADDR, 100) == ESP_OK;
}

// device→host: CH32 から 1 フレーム読む。戻り値 = payload バイト数 (0=データ無し, <0=err)。
static int read_one_frame(uint8_t* out, int out_max) {
    uint8_t buf[FRAME_MAX];
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return -1;
    esp_err_t e = i2c_master_receive(s_dev, buf, sizeof(buf), 50);
    xSemaphoreGive(s_mutex);
    if (e != ESP_OK) return -1;
    int len = buf[0];                    // 先頭が LEN
    if (len <= 0) return 0;
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    if (len > out_max) len = out_max;
    memcpy(out, &buf[1], len);
    return len;
}

// INT (CH32→ESP32) Low で起こされ、CH32 のキューを空 (LEN=0) になるまで読み出す。
static void reader_task(void *arg) {
    (void)arg;
    uint8_t midi[FRAME_MAX];
    for (;;) {
        // INT がすでに Low なら即処理、そうでなければ通知待ち (フォールバックで定期起床)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        for (int guard = 0; guard < 32; guard++) {     // 1 起床で最大 32 フレーム
            int n = read_one_frame(midi, sizeof(midi));
            if (n <= 0) break;                          // 0=空, <0=err
            if (s_capturing) {
                // 期待する応答 (SysEx かつ cmd==s_cap_match) のフレームだけ捕捉。
                // 古い device→host フレーム (TARGET_RX 0x05 等) は読み捨てて待ち続ける。
                bool match = (s_cap_match == 0) ||
                             (n >= 4 && midi[0] == 0xF0 && midi[3] == s_cap_match);
                if (s_cap_len == 0 && match) {
                    memcpy(s_cap_buf, midi, n);
                    s_cap_len = n;
                    if (s_cap_sem) xSemaphoreGive(s_cap_sem);
                }
            } else if (s_on_rx) {
                s_on_rx(midi, n);
            }
        }
    }
}

static void IRAM_ATTR int_isr(void *arg) {
    (void)arg;
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_reader, &hp);
    portYIELD_FROM_ISR(hp);
}

void i2c_bridge_init(void (*on_rx)(const uint8_t* midi, int len)) {
    s_on_rx = on_rx;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_cap_sem) s_cap_sem = xSemaphoreCreateBinary();
    s_capturing = false;
    s_cap_len = 0;

    i2c_master_bus_config_t bus_cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = 0,
        .scl_io_num        = I2C_SCL_GPIO,
        .sda_io_num        = I2C_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   // 外付けプルアップ併用 (必須)
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = CH32_ADDR,
        .scl_speed_hz    = I2C_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev));

    // 起動時ヘルスチェック: CH32 (デバイスエンジン) が I2C で応答するか確認。
    esp_err_t probe = i2c_master_probe(s_bus, CH32_ADDR, 100);
    if (probe == ESP_OK) ESP_LOGI(TAG, "CH32 device engine found at 0x%02X", CH32_ADDR);
    else                 ESP_LOGW(TAG, "CH32 not responding at 0x%02X (rc=%d) - check wiring",
                                  CH32_ADDR, (int)probe);

    // INT 入力 (CH32 active-low / open-drain)。内蔵プルアップ有効、立ち下がり割り込み。
    gpio_config_t ic = {
        .pin_bit_mask = (1ULL << I2C_INT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&ic);

    xTaskCreate(reader_task, "i2c_rd", 4096, NULL, 8, &s_reader);

    // ISR サービスは一度だけインストール (再 init 時の二重 install は無視)。
    esp_err_t isr = gpio_install_isr_service(0);
    if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service rc=%d", (int)isr);
    }
    gpio_isr_handler_add(I2C_INT_GPIO, int_isr, NULL);

    ESP_LOGI(TAG, "I2C bridge: SDA=%d SCL=%d INT=%d addr=0x%02X",
             I2C_SDA_GPIO, I2C_SCL_GPIO, I2C_INT_GPIO, CH32_ADDR);
}

// I2C を完全に停止し SDA/SCL(GPIO21/22) を解放する。SWD でフラッシュを書く前に呼ぶ。
// (ISR サービス自体はアンインストールしない。再 init で再利用する)
void i2c_bridge_deinit(void) {
    if (s_reader) { vTaskDelete(s_reader); s_reader = NULL; }
    gpio_isr_handler_remove(I2C_INT_GPIO);
    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    if (s_bus) { i2c_del_master_bus(s_bus); s_bus = NULL; }  // SDA/SCL を解放
    if (s_mutex) { vSemaphoreDelete(s_mutex); s_mutex = NULL; }
    ESP_LOGI(TAG, "I2C bridge deinit (SDA/SCL released)");
}
