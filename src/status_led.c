// ===================================================================================
// status_led.c
// ===================================================================================
// PB0 WS2812B-2020 ドライバ + ステータスマシン。
//
// 構造:
//   - app_state            : WAITING / SCANNED / CONNECTED
//   - activity_blink       : CONNECTED 時、最近 Note/CC を受信した = 青点滅
//   - override_rgb_active  : ホスト発の色 override が有効か
//   - override_blink_speed : ホスト発の点滅 override 速度 (None/Slow/Mid/High)
//
// レンダリング:
//   poll() 内で「desired (色, 点滅周期)」を上記から計算し、現在描画中の値と
//   異なる場合のみ WS2812 を更新する。これにより毎ループの送信を避けつつ
//   状態遷移には即追従する。
//
// 点滅:
//   blink_half_period_ms != 0 のとき、SysTick の進み具合で ON/OFF をトグル。
//   OFF 時は (0,0,0) を送る。
//
// HB 監視:
//   poll() で SysTick - last_heartbeat_tick > 3 秒なら CONNECTED → WAITING に
//   戻し override を reset。
//
// 前提: funconfig.h で FUNCONF_SYSTICK_USE_HCLK = 1 (SysTick = HCLK 48MHz)
// ===================================================================================
#include "status_led.h"
#include "ch32fun.h"
#include "funconfig.h"

#define WS2812BSIMPLE_IMPLEMENTATION
#include "ws2812b_simple.h"

#define LED_PIN  0  // PB0

// 1ms 相当の SysTick tick 数 (HCLK = 48MHz)
#define MS_TO_TICKS(ms)  ((uint64_t)(ms) * (FUNCONF_SYSTEM_CORE_CLOCK / 1000))

// アクティビティ点滅 (CONNECTED + 最近 Note/CC 受信) の入力タイムアウト
#define ACTIVITY_IDLE_TIMEOUT_MS  500

// HEART_BEAT 受信間隔の上限。これを超えると WAITING に戻る。
#define HEARTBEAT_TIMEOUT_MS      3000

// 状態色 (8bit RGB)
//   人間の視覚は色によって明るさ感度が違うので、各色 ~25% 程度に揃えてある。
#define COLOR_WAITING_R   64
#define COLOR_WAITING_G   32
#define COLOR_WAITING_B    0
#define COLOR_SCANNED_R    0
#define COLOR_SCANNED_G   64
#define COLOR_SCANNED_B    0
#define COLOR_CONNECTED_R  0
#define COLOR_CONNECTED_G  0
#define COLOR_CONNECTED_B 64

// ---------------------------------------------------------------------------
// 内部状態
// ---------------------------------------------------------------------------

static status_led_state_t app_state;
static uint64_t           last_heartbeat_tick;
static uint64_t           last_activity_tick;
static uint8_t            activity_blink_active;

static uint8_t  override_rgb_active;
static uint8_t  override_r, override_g, override_b;
static uint8_t  override_blink_speed;  // LED_BLINK_*

// 点滅エンジン (現在の desired)
static uint16_t blink_half_period_ms;  // 0 = solid
static uint8_t  blink_on_phase;         // 1=ON, 0=OFF
static uint64_t next_toggle_tick;

// 直近に WS2812 に流したカラー (差分検出用)。on_phase との組合せで「OFF だから 0,0,0
// を出した状態」も区別する。
static uint8_t  disp_r, disp_g, disp_b;
static uint8_t  disp_initialized;

// ---------------------------------------------------------------------------
// 低レベル送信
// ---------------------------------------------------------------------------

static void ws2812_send(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t buf[3] = { g, r, b };  // GRB 順
    WS2812BSimpleSend(GPIOB, LED_PIN, buf, sizeof(buf));
    Delay_Us(60);  // latch: >50µs LOW
}

// 必要なときだけ送信。差分が無ければ no-op。
static void ws2812_send_if_changed(uint8_t r, uint8_t g, uint8_t b) {
    if (disp_initialized && r == disp_r && g == disp_g && b == disp_b) return;
    ws2812_send(r, g, b);
    disp_r = r; disp_g = g; disp_b = b;
    disp_initialized = 1;
}

// ---------------------------------------------------------------------------
// 状態 → desired (色, 点滅) 計算
// ---------------------------------------------------------------------------

static uint16_t blink_speed_to_half_ms(uint8_t speed) {
    switch (speed) {
        case LED_BLINK_SLOW: return 500;
        case LED_BLINK_MID:  return 250;
        case LED_BLINK_HIGH: return 125;
        case LED_BLINK_NONE:
        default:             return 0;
    }
}

static void compute_desired(uint8_t* r, uint8_t* g, uint8_t* b, uint16_t* half_ms) {
    if (override_rgb_active) {
        *r = override_r; *g = override_g; *b = override_b;
        *half_ms = blink_speed_to_half_ms(override_blink_speed);
        return;
    }
    switch (app_state) {
        case STATUS_LED_STATE_WAITING:
            *r = COLOR_WAITING_R; *g = COLOR_WAITING_G; *b = COLOR_WAITING_B;
            *half_ms = 0;
            break;
        case STATUS_LED_STATE_SCANNED:
            *r = COLOR_SCANNED_R; *g = COLOR_SCANNED_G; *b = COLOR_SCANNED_B;
            *half_ms = 0;
            break;
        case STATUS_LED_STATE_CONNECTED:
        default:
            *r = COLOR_CONNECTED_R; *g = COLOR_CONNECTED_G; *b = COLOR_CONNECTED_B;
            *half_ms = activity_blink_active ? blink_speed_to_half_ms(LED_BLINK_HIGH) : 0;
            break;
    }
}

// desired を再評価して LED を反映する。点滅中の OFF 位相は (0,0,0) を流す。
static void render(void) {
    uint8_t r, g, b;
    uint16_t half_ms;
    compute_desired(&r, &g, &b, &half_ms);

    // 点滅周期が変わったら ON 位相にリセットして見栄えを揃える
    if (half_ms != blink_half_period_ms) {
        blink_half_period_ms = half_ms;
        blink_on_phase = 1;
        if (half_ms) next_toggle_tick = SysTick->CNT + MS_TO_TICKS(half_ms);
    }

    if (blink_half_period_ms == 0 || blink_on_phase) {
        ws2812_send_if_changed(r, g, b);
    } else {
        ws2812_send_if_changed(0, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// 公開 API
// ---------------------------------------------------------------------------

void status_led_init(void) {
    // PB0: push-pull 50MHz
    GPIOB->CFGLR &= ~(0xfu << (4 * LED_PIN));
    GPIOB->CFGLR |=  ((uint32_t)(GPIO_Speed_50MHz | GPIO_CNF_OUT_PP)) << (4 * LED_PIN);
    GPIOB->BCR = (1u << LED_PIN);

    app_state            = STATUS_LED_STATE_WAITING;
    last_heartbeat_tick  = 0;
    last_activity_tick   = 0;
    activity_blink_active = 0;

    override_rgb_active  = 0;
    override_r = override_g = override_b = 0;
    override_blink_speed = LED_BLINK_NONE;

    blink_half_period_ms = 0;
    blink_on_phase       = 1;
    next_toggle_tick     = 0;

    disp_initialized = 0;
    render();  // initial yellow
}

void status_led_set_state(status_led_state_t state) {
    app_state = state;
    if (state != STATUS_LED_STATE_CONNECTED) {
        activity_blink_active = 0;
    }
    render();
}

void status_led_on_identify_request(void) {
    // IDENTIFY_REQ はいつでも SCANNED に戻す (rescan = 接続リセット)。
    // override は触らない (rename 中の rescan は想定しないが、念のため壊さない)。
    status_led_set_state(STATUS_LED_STATE_SCANNED);
}

void status_led_on_heart_beat(void) {
    last_heartbeat_tick = SysTick->CNT;
    if (app_state != STATUS_LED_STATE_CONNECTED) {
        status_led_set_state(STATUS_LED_STATE_CONNECTED);
    }
}

void status_led_on_disconnect_request(void) {
    // override をクリアして即座に SCANNED (緑) に戻す。
    // HB タイムアウト復帰 (黄/WAITING) との違いは「アプリがまだ生きている」ことを
    // 表現できる点。
    override_rgb_active   = 0;
    override_blink_speed  = LED_BLINK_NONE;
    activity_blink_active = 0;
    status_led_set_state(STATUS_LED_STATE_SCANNED);
}

void status_led_on_input_activity(void) {
    if (app_state != STATUS_LED_STATE_CONNECTED) return;
    if (override_rgb_active) return;  // override 中は activity を無視

    last_activity_tick = SysTick->CNT;
    if (!activity_blink_active) {
        activity_blink_active = 1;
        render();
    }
}

void status_led_set_override_rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (r == 0xFF && g == 0xFF && b == 0xFF) {
        // sentinel: reset override
        status_led_reset_override();
        return;
    }
    override_rgb_active = 1;
    override_r = r; override_g = g; override_b = b;
    render();
}

void status_led_set_override_blink(uint8_t speed) {
    if (speed > LED_BLINK_HIGH) speed = LED_BLINK_NONE;
    override_blink_speed = speed;
    render();
}

void status_led_reset_override(void) {
    override_rgb_active  = 0;
    override_blink_speed = LED_BLINK_NONE;
    render();
}

// ---------------------------------------------------------------------------
// poll: HB タイムアウト / activity タイムアウト / blink toggle
// ---------------------------------------------------------------------------

void status_led_poll(void) {
    uint64_t now = SysTick->CNT;

    // HB タイムアウト: CONNECTED でのみ意味がある
    if (app_state == STATUS_LED_STATE_CONNECTED) {
        if ((int64_t)(now - last_heartbeat_tick) > (int64_t)MS_TO_TICKS(HEARTBEAT_TIMEOUT_MS)) {
            app_state = STATUS_LED_STATE_WAITING;
            activity_blink_active = 0;
            // ホスト側が落ちたので override も明示クリアして黄に戻す
            override_rgb_active  = 0;
            override_blink_speed = LED_BLINK_NONE;
            render();
        }
    }

    // Activity タイムアウト
    if (activity_blink_active) {
        if ((int64_t)(now - last_activity_tick) > (int64_t)MS_TO_TICKS(ACTIVITY_IDLE_TIMEOUT_MS)) {
            activity_blink_active = 0;
            render();
        }
    }

    // Blink toggle
    if (blink_half_period_ms != 0) {
        if ((int64_t)(now - next_toggle_tick) >= 0) {
            next_toggle_tick += MS_TO_TICKS(blink_half_period_ms);
            blink_on_phase = !blink_on_phase;
            // render は差分のみ送るので毎 toggle で呼んで OK
            render();
        }
    }
}
