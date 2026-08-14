// wifi_probe.cpp — WiFi だけを切り分けるための最小スケッチ
//
// 本体（main.cpp）では WiFi に繋がらない。原因がアプリ側の文脈にあるのか、
// ボード / AP / 電源にあるのかを分けるために、余計なものを全部外して試す。
//
// main.cpp との違い:
//   - スプラッシュを出さない（PNG デコーダを動かさない）
//   - M5Canvas を作らない（32KB のスプライトを確保しない）
//   - 14MB の辞書を開かない
//   - 候補バー（84KB の静的領域）をリンクしない
//   - **config.txt を読んだあと SD を切り離してから** WiFi を上げる
//     （SD が SPI を掴んだままなことの影響を外すため）
//
// これで繋がればアプリ側が原因、繋がらなければアプリは無罪。
//
// 使い方（platformio.ini の build_src_filter を差し替える）:
//   -<main.cpp>
//   +<wifi_probe.cpp>
// 戻すときは逆にする。

#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>

#include "opur_config.h"

#define SD_MOUNT "/sdcard"

static OpurConfig g_cfg;

// --- 画面出力 -------------------------------------------------------------
// Canvas を使わず Display に直接書く。ちらつくが、ここでは見えれば十分。

// 起動時に調べた内容は行 0-5 に固定で置き、行 6-7 だけを更新し続ける。
// 全部を流していくと、肝心の調査結果が状態表示に押し流されて読めなくなる。
static void at(int row, const char *fmt, ...) {
    char    buf[64];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    auto &d = M5Cardputer.Display;

    // 行ごと消してから書く。前の内容が長いと残ってしまうため。
    d.fillRect(0, row * 16, 240, 16, TFT_BLACK);
    d.setCursor(0, row * 16);
    d.print(buf);

    Serial.println(buf);
}

// 行 6-7 を交互に使う簡易スクロール。直近 2 件が常に見える。
static int g_tail = 6;

static void tail(const char *fmt, ...) {
    char    buf[64];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    at(g_tail, "%s", buf);
    g_tail = (g_tail == 6) ? 7 : 6;
}

// --- WiFi イベントの足跡 ---------------------------------------------------

static char g_trail[48];
static int  g_trail_n    = 0;
static int  g_disc_reason = 0;

static void on_event(WiFiEvent_t ev, WiFiEventInfo_t info) {
    if (g_trail_n < (int)sizeof(g_trail) - 4) {
        g_trail_n += snprintf(g_trail + g_trail_n, sizeof(g_trail) - g_trail_n,
                              "%s%d", g_trail_n ? "," : "", (int)ev);
    }
    if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        g_disc_reason = info.wifi_sta_disconnected.reason;
    }
}

// ---------------------------------------------------------------------------

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);

    auto &d = M5Cardputer.Display;
    d.setRotation(1);
    d.fillScreen(TFT_BLACK);
    d.setFont(&fonts::efontJA_16);
    d.setTextColor(TFT_WHITE, TFT_BLACK);

    at(0, "probe start");

    // --- config.txt を読むためだけに SD を上げる ---
    bool sd_ok = false;
    if (M5.hasSD()) {
        SPI.begin(M5.getPin(m5::pin_name_t::sd_spi_sclk),
                  M5.getPin(m5::pin_name_t::sd_spi_miso),
                  M5.getPin(m5::pin_name_t::sd_spi_mosi),
                  M5.getPin(m5::pin_name_t::sd_spi_ss));
        sd_ok = SD.begin(M5.getPin(m5::pin_name_t::sd_spi_ss), SPI,
                         25000000, SD_MOUNT, 5, false);
    }

    opur_config_clear(&g_cfg);
    const int keys = opur_config_load(&g_cfg, SD_MOUNT "/config.txt");
    at(1, "SD=%d cfg=%d ssid%dB pass%dB", sd_ok, keys,
        (int)strlen(g_cfg.wifi_ssid), (int)strlen(g_cfg.wifi_pass));

    // --- SD を切り離す。SPI を掴んだままの影響を外す ---
    SD.end();
    SPI.end();
    /* SD 切り離しは行 1 に含めたので出さない */

    if (!opur_config_has_wifi(&g_cfg)) {
        at(2, "設定なし。終了");
        return;
    }

    // --- WiFi ---
    // イベント登録を mode() より先にする。STA_START(2) も取りこぼさないため。
    WiFi.onEvent(on_event);

    const bool mode_ok = WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    // 自動再接続を切る。入れたままだと失敗のたびに勝手に再試行が始まり、
    // 「最初の試行がどう終わったか」が見えなくなる。
    WiFi.setAutoReconnect(false);

    /* mode は begin と一緒に行 5 へ */

    // --- 国設定。AP が ch12/13 に居ると、国コードによっては接続時の探索から
    //     外れる。scan では見えるのに繋がらない、の典型的な原因のひとつ。
    {
        wifi_country_t c;
        if (esp_wifi_get_country(&c) == ESP_OK) {
            at(2, "国 %c%c ch%d-%d pol%d",
                c.cc[0], c.cc[1], c.schan, c.schan + c.nchan - 1, (int)c.policy);
        }
    }

    // --- 目的の AP がどの ch に居るか。ここが 12/13 なら国設定を疑う ---
    {
        const int n = WiFi.scanNetworks(false, true);
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (WiFi.SSID(i) != g_cfg.wifi_ssid) continue;
            at(3, "AP ch%d %ddBm enc%d", WiFi.channel(i), WiFi.RSSI(i),
                (int)WiFi.encryptionType(i));
            found = 1;
            break;
        }
        if (!found) at(3, "scan %d件 AP未発見", n);
        WiFi.scanDelete();
    }

    at(4, "内部%uK 塊%uK DMA%uK mode=%d",
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024), mode_ok);

    // 接続は loop() 側の巡回に任せる。ここでは繋ぎにいかない。
}

// --- 接続パターンの巡回 -----------------------------------------------------
//
// AP は WPA2/WPA3 混在（enc=7）。ESP32 がこのモードで握れないことがあるので、
// PMF（保護管理フレーム）と認証方式の下限を変えた 4 通りを順に試す。
// Arduino の WiFi.begin() では PMF を指定できないため、
// esp_wifi_set_config() を直接叩く。
//
//   v0 既定   PMF capable のみ・下限 WPA2 … Arduino の WiFi.begin() と同じ
//   v1 PMF無  PMF を capable からも外す … AP に「素の WPA2」を使わせる狙い
//   v2 WPA3   PMF required・下限 WPA3   … 逆に WPA3(SAE) を明示的に選ばせる
//   v3 下限無 下限 OPEN                 … しきい値で弾かれていないかの確認

static const char *kVariantName[4] = { "既定", "PMF無", "WPA3", "下限無" };

static void try_variant(int v) {
    wifi_config_t c = {};

    strncpy((char *)c.sta.ssid,     g_cfg.wifi_ssid, sizeof(c.sta.ssid));
    strncpy((char *)c.sta.password, g_cfg.wifi_pass, sizeof(c.sta.password));

    c.sta.scan_method    = WIFI_ALL_CHANNEL_SCAN;
    c.sta.sort_method    = WIFI_CONNECT_AP_BY_SIGNAL;
    c.sta.threshold.rssi = -127;

    switch (v) {
    case 0:
        c.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        c.sta.pmf_cfg.capable = true;  c.sta.pmf_cfg.required = false;
        break;
    case 1:
        c.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        c.sta.pmf_cfg.capable = false; c.sta.pmf_cfg.required = false;
        break;
    case 2:
        c.sta.threshold.authmode = WIFI_AUTH_WPA3_PSK;
        c.sta.pmf_cfg.capable = true;  c.sta.pmf_cfg.required = true;
        break;
    default:
        c.sta.threshold.authmode = WIFI_AUTH_OPEN;
        c.sta.pmf_cfg.capable = true;  c.sta.pmf_cfg.required = false;
        break;
    }

    esp_wifi_disconnect();
    delay(300);

    // 前の試行の足跡を消す。どのパターンで何が起きたかを混ぜないため。
    g_trail[0]    = '\0';
    g_trail_n     = 0;
    g_disc_reason = 0;

    const esp_err_t e1 = esp_wifi_set_config(WIFI_IF_STA, &c);
    const esp_err_t e2 = esp_wifi_connect();

    at(5, "v%d %s set=%s conn=%s", v, kVariantName[v],
       esp_err_to_name(e1), esp_err_to_name(e2));
}

void loop() {
    static uint32_t t0        = millis();
    static int      last_st   = -1;
    static int      last_n    = -1;
    static uint32_t last_show = 0;
    static uint32_t last_try  = 0;
    static int      variant   = 0;

    delay(200);

    // 20 秒ずつパターンを巡回する。1 パターンで握れなければ次へ。
    // 4 パターン一巡で 80 秒。繋がったらそこで止まる。
    if (last_try == 0 || (millis() - last_try) >= 20000) {
        try_variant(variant);
        variant  = (variant + 1) % 4;
        last_try = millis();
    }

    const int st  = (int)WiFi.status();
    const int sec = (int)((millis() - t0) / 1000);

    // 変化したときだけ出す。500ms ごとに出すと 4 秒で画面が流れてしまい、
    // 肝心の「イベントが来た瞬間」を読み取れない。
    const bool changed = (st != last_st) || (g_trail_n != last_n);
    const bool tick    = (millis() - last_show) >= 5000;   // 生存確認

    if (changed || tick) {
        tail("%3ds st=%d ev:%s r=%d", sec, st,
            g_trail[0] ? g_trail : "-", g_disc_reason);
        last_st   = st;
        last_n    = g_trail_n;
        last_show = millis();
    }

    if (st == WL_CONNECTED) {
        const IPAddress ip = WiFi.localIP();
        // 直前に試したパターンが正解。variant は既に次へ進んでいるので戻す。
        const int hit = (variant + 3) % 4;
        at(0, "接続成功 v%d %s", hit, kVariantName[hit]);
        tail("OK %u.%u.%u.%u (%ds)", ip[0], ip[1], ip[2], ip[3], sec);
        while (true) delay(1000);          // 繋がったらここで止める
    }
}
