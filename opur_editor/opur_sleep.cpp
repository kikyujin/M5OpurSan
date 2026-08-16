// opur_sleep.cpp — ライトスリープ / ディープスリープ（実機専用）
//
// ESP-IDF 4.4.7 / ESP32-S3。5.x とは API の置き場も enum も違うので注意:
//   - ヘッダは esp_hw_support/include/esp_sleep.h（esp_system/ ではない）
//   - ext1 のモードは ESP_EXT1_WAKEUP_ANY_LOW / ANY_HIGH。
//     ESP32 の ALL_LOW は deprecated な別名になっている
//   - esp_deep_sleep_enable_gpio_wakeup() は S3 では**存在しない**
//     （SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP が未定義）。deep は ext1 を使う
//   - RTC GPIO は 0〜21（SOC_RTCIO_PIN_COUNT 22）。
//     esp_sleep.h のコメントにある 0,2,4,12-15,25-27,32-39 は ESP32 の値で嘘

#include "opur_sleep.h"

#include <M5Unified.h>

#include <driver/gpio.h>
#include <esp_sleep.h>

namespace {

// Cardputer ADV の TCA8418（I2C キーボード）の INT ピン。
// M5Cardputer ライブラリの TCA8418.cpp が DEFAULT_TCA8418_INT_PIN 11 として
// 使っているのと同じ線。オープンドレインのアクティブ LOW で、
// ライブラリ側は pinMode(11, INPUT)（プルアップ無し）で受けている
// —— つまり基板にプルアップが載っている。
//
// **無印ではこの 11 番はキーマトリクスの出力ピンで、まったくの別用途。**
// 番号を決め打ちせず、必ず is_adv() を通すこと。
constexpr gpio_num_t kKeyIntPin = GPIO_NUM_11;

bool is_adv() {
    return M5.getBoard() == m5::board_t::board_M5CardputerADV;
}

// 寝る直前にキーが押されている（INT が LOW のまま）かどうか。
//
// TCA8418 の INT はレベル出力で、I2C で FIFO を読んで INT_STAT を
// クリアするまで LOW のままになる。その状態で寝ると即座に起きるので、
// 寝ずに戻して呼び出し側にキーを処理させる。
bool key_pending() {
    return gpio_get_level(kKeyIntPin) == 0;
}

int cause_to_wake(esp_sleep_wakeup_cause_t c) {
    switch (c) {
    case ESP_SLEEP_WAKEUP_GPIO:
    case ESP_SLEEP_WAKEUP_EXT1:  return OPUR_WAKE_KEY;
    case ESP_SLEEP_WAKEUP_TIMER: return OPUR_WAKE_TIMER;
    default:                     return OPUR_WAKE_OTHER;
    }
}

}  // namespace

// ---------------------------------------------------------------------------

int opur_sleep_key_wake_supported(void) {
    return is_adv() ? 1 : 0;
}

int opur_sleep_light(uint32_t timer_ms) {
    if (!is_adv())    return OPUR_WAKE_OTHER;   // 起きられないので寝ない
    if (key_pending()) return OPUR_WAKE_KEY;    // もう押されている

    // ライトスリープの GPIO ウェイクは RTC GPIO でなくても使えるが、
    // レベル指定しかできない（エッジ不可）。INT はアクティブ LOW。
    //
    // **この呼び出しはピンの割り込み種別そのものを LOW_LEVEL に書き換える。**
    // Arduino の attachInterrupt() が gpio_set_intr_type() と
    // gpio_wakeup_enable() を同じ intr_type で並べて呼んでいる
    // （cores/esp32/esp32-hal-gpio.c:191-193）のがその証拠。
    //
    // M5Cardputer のキーボードドライバは同じ GPIO11 に CHANGE（ANYEDGE）で
    // ISR を張っているので、**起きたあとに戻さないとキー入力が壊れる**。
    // レベル割り込みのままだと、INT が LOW の間 ISR が延々と再入する
    // （TCA8418.cpp の ISR はフラグを立てるだけで、INT を下げるのは
    //   I2C で FIFO を読む update() のほう）。
    gpio_wakeup_enable(kKeyIntPin, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    if (timer_ms > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)timer_ms * 1000ULL);
    }

    esp_light_sleep_start();

    const int wake = cause_to_wake(esp_sleep_get_wakeup_cause());

    // 起こした設定は毎回畳む。残したままだと、次に別の条件で寝たいときに
    // 意図しないソースが生き残る。
    gpio_wakeup_disable(kKeyIntPin);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    // 割り込み種別をキーボードドライバの張り方（CHANGE）に戻す。
    // gpio_wakeup_disable() は種別までは戻してくれない。
    gpio_set_intr_type(kKeyIntPin, GPIO_INTR_ANYEDGE);

    return wake;
}

void opur_sleep_deep(uint32_t timer_ms) {
    if (timer_ms > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)timer_ms * 1000ULL);
    }

    // ディープスリープでは GPIO ウェイクが使えないので ext1。
    // GPIO11 は RTC GPIO の範囲（0〜21）内なので指定できる。
    //
    // 内部プルアップは RTC ペリフェラルが落ちると効かないが、
    // この線は基板側でプルアップされている（上の注記）ので指定しない。
    if (is_adv()) {
        esp_sleep_enable_ext1_wakeup(1ULL << kKeyIntPin,
                                     ESP_EXT1_WAKEUP_ANY_LOW);
    }

    esp_deep_sleep_start();   // 戻らない
}
