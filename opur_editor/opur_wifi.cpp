// opur_wifi.cpp — WiFi 接続と NTP 同期の実装
//
// WiFi.h と configTzTime は framework-arduinoespressif32 に同梱されているので、
// platformio.ini の lib_deps に足すものは無い。
//
// 画面には何も描かない。表示は呼び出し側（main.cpp）の責任にしてある。
// 起動時とアイドル時では出し方が変わるはずで、それをこの層が決められないため。
// ただしログ（opur_log）はここから直接出す。何が起きたかを一番細かく
// 知っているのがこの層で、呼び出し側に返すには数が多すぎるため。
//
// --- AP 側の要件 -----------------------------------------------------------
// **AP は WPA2-PSK 専用にしておくこと。**
// WPA2/WPA3 混在モード（transition mode、encryptionType=7）の AP には、
// ESP32-S3 + ESP-IDF 4.4.7 の組み合わせで繋がらないことが実機で確認された。
// このとき成功も失敗も通知されず、STA_DISCONNECTED すら発火しないため、
// status() は WL_DISCONNECTED のまま張り付き、切断理由も取得できない
// （60 秒待っても変化なし）。WPA2 専用に変えれば既定設定のまま繋がる。
// 経緯は wifi_connection_problem.md を参照。

#include "opur_wifi.h"

#include "opur_log.h"

#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include <time.h>

namespace {

bool     g_ntp_done    = false;
uint32_t g_heap_before = 0;
uint32_t g_heap_after  = 0;
int      g_last_status = -1;
uint32_t g_elapsed_ms  = 0;

// NTP サーバ。1 つ目が落ちていても止まらないように 2 つ書く。
constexpr const char *kNtp1 = "ntp.nict.jp";
constexpr const char *kNtp2 = "pool.ntp.org";

// TZ 文字列。JST-9 = UTC+9（POSIX の TZ は符号が逆）。夏時間なし。
constexpr const char *kTz = "JST-9";

// 直近の切断理由（wifi_err_reason_t の生値）。WiFi イベントタスクから書かれ、
// メインタスクから読まれる。int の読み書きは分割されないので volatile で足りる。
volatile int g_disc_reason = 0;

void on_wifi_event(WiFiEvent_t ev, WiFiEventInfo_t info) {
    if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        g_disc_reason = info.wifi_sta_disconnected.reason;
    }
}

}  // namespace

const char *opur_wifi_reason_text(int r) {
    switch (r) {
    case 201: return "AP見つからず";      // NO_AP_FOUND     → 5GHz / 圏外 / 名前違い
    case 202: return "認証失敗";          // AUTH_FAIL
    case 203: return "アソシ失敗";        // ASSOC_FAIL
    case 15:  return "鍵違い";            // 4WAY_HANDSHAKE_TIMEOUT
    case 2:   return "認証期限切れ";      // AUTH_EXPIRE
    case 4:   return "無通信で切断";      // ASSOC_EXPIRE
    case 8:   return "AP側から切断";      // ASSOC_LEAVE
    case 205: return "接続失敗";          // CONNECTION_FAIL
    // 切断イベントが来ないまま時間切れ。AP が WPA2/WPA3 混在モードだと
    // これになる（ファイル先頭のコメントを参照）。
    case 0:   return "無反応(AP設定?)";
    default:  return "";
    }
}

int opur_wifi_last_reason(void) { return g_disc_reason; }

int opur_wifi_is_connected(void) {
    return WiFi.status() == WL_CONNECTED ? 1 : 0;
}

int opur_wifi_connect(const OpurConfig *cfg) {
    if (opur_wifi_is_connected()) return 1;
    if (!opur_config_has_wifi(cfg)) return 0;

    g_disc_reason = 0;
    g_heap_before = ESP.getFreeHeap();

    const bool mode_ok = WiFi.mode(WIFI_STA);

    // 省電力を切る。入れたままだと取りこぼしが増え、接続にも時間がかかる。
    WiFi.setSleep(false);

    // 切れたときに黙って復帰してほしいので明示しておく（既定でも true）。
    WiFi.setAutoReconnect(true);

    // 切断理由はイベントでしか取れない。status() は WL_DISCONNECTED に
    // 丸めてしまい、「AP が無い」と「鍵が違う」を区別できないため。
    WiFi.onEvent(on_wifi_event, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    opur_log_add("wifi: mode=%s heap%uK",
                 mode_ok ? "OK" : "NG", (unsigned)(g_heap_before / 1024));

    WiFi.begin(cfg->wifi_ssid, cfg->wifi_pass);

    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - start) < OPUR_WIFI_TIMEOUT_MS) {
        delay(50);
    }

    g_elapsed_ms  = millis() - start;
    g_last_status = (int)WiFi.status();
    g_heap_after  = ESP.getFreeHeap();

    return opur_wifi_is_connected();
}

void opur_wifi_disconnect(void) {
    // 自動再接続を先に切る。切らずに disconnect すると、裏で即座に
    // 繋ぎ直しにいく（opur_wifi_connect() が毎回 true を立てているので、
    // 対で使うぶんには次の接続で戻る）。
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(true);      // true = WiFi を落とす
    WiFi.mode(WIFI_OFF);

    // 時刻は ESP32 の RTC が持っているので、繋ぎ直しても NTP は要らない。
    // g_ntp_done は落とさない。
}

int opur_wifi_ntp_sync(uint32_t timeout_ms) {
    struct tm t;

    if (g_ntp_done) return 1;
    if (!opur_wifi_is_connected()) return 0;

    // 3 番目にゲートウェイ（＝ルーター）を入れる。名前解決が要らないので、
    // DNS がまだ引けない起動直後でも当たる可能性がある。
    // ルーターが NTP を返さなければ単に無視されるだけで、害はない。
    const String gw = WiFi.gatewayIP().toString();

    configTzTime(kTz, kNtp1, kNtp2, gw.c_str());

    const uint32_t start = millis();

    // 同期できたかは getLocalTime() の戻り値で判定する。
    // 中で tm_year > 2016 を見ているので、指示の「2026 年以降か」を
    // 自前で書くより確実（RTC 未設定なら 1970 年のままになる）。
    const bool ok = getLocalTime(&t, timeout_ms);

    opur_log_add("ntp %s %ums (gw %s)", ok ? "OK" : "NG",
                 (unsigned)(millis() - start), gw.c_str());

    if (!ok) return 0;

    g_ntp_done = true;
    return 1;
}

int opur_wifi_ntp_synced(void) {
    return g_ntp_done ? 1 : 0;
}

int opur_wifi_last_pending(void) {
    return (g_last_status == WL_IDLE_STATUS ||
            g_last_status == WL_DISCONNECTED) ? 1 : 0;
}

const char *opur_wifi_ip(void) {
    static char buf[16];

    if (!opur_wifi_is_connected()) {
        strncpy(buf, "0.0.0.0", sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        return buf;
    }

    const IPAddress ip = WiFi.localIP();
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return buf;
}

int      opur_wifi_last_status(void) { return g_last_status; }
uint32_t opur_wifi_elapsed_ms(void)  { return g_elapsed_ms; }

uint32_t opur_wifi_heap_before(void) { return g_heap_before; }
uint32_t opur_wifi_heap_after(void)  { return g_heap_after; }
