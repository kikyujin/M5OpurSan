// opur_net.cpp — EQIDEN セマフォ送信の実装
//
// 019 では「保存したら /opur/ の先頭 1 件を自動で送り、送れたら sent/ へ移す」
// という複合処理だったが、020 で **保存と送信を切り離した**。
// ここに残るのは HTTPS の 2 動作だけで、何を送るか・送ってよいかの判断は
// すべて呼び出し側（メニュー操作）にある。
//
// HTTPS の証明書は検証しない（setInsecure）。CA バンドルを積むと Flash を
// 数十 KB 使ううえ、Let's Encrypt の中間証明書が変わるたびに焼き直しになる。
// 送っているのは自分で書いた本文であり、盗聴されて困る秘密ではないので、
// 経路の暗号化だけ取って検証は省く。必要になったら CA 埋め込みに替えられる。
//
// **メモリの前提**: mbedTLS のハンドシェイクは入出力バッファ 16KB x 2 を
// 含めて 45〜50KB 要る。この機体（ESP32-S3FN8）は PSRAM を積んでおらず、
// WiFi 接続後の内部ヒープは素のままだと 44KB しか残らない。
// 描画 Canvas を 1bpp にして 28KB 空けることで成立している
// （m5curses.cpp の canvas_alloc を参照。8bpp に戻すと HTTPS が壊れる）。

#include "opur_net.h"

#ifdef ESP_PLATFORM

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include <string.h>

#include "opur_log.h"

static char s_endpoint[OPUR_NET_URL_MAX];

// ---------------------------------------------------------------------------

void opur_net_init(const char *endpoint_url) {
    if (!endpoint_url) {
        s_endpoint[0] = '\0';
        return;
    }
    strncpy(s_endpoint, endpoint_url, sizeof(s_endpoint) - 1);
    s_endpoint[sizeof(s_endpoint) - 1] = '\0';
}

// 送れる状態か。0 なら呼び出し側は何もしない。
static int ready(void) {
    if (s_endpoint[0] == '\0')         return 0;   // 送信先が設定されていない
    if (WiFi.status() != WL_CONNECTED) return 0;   // 電波の無い場所。異常ではない
    return 1;
}

// ---------------------------------------------------------------------------
// 切り分け用のログ
// ---------------------------------------------------------------------------
//
// 接続の失敗（HTTPClient の -1）は「繋がらなかった」としか言わないので、
// それだけでは DNS・ヒープ・TLS のどれが原因か分からない。実機にシリアルを
// 常時繋げない以上、失敗した瞬間の材料は自分で残すしかない。
//
// 2026-08-15 の実機切り分けはこれで解けた。出るのは失敗したときだけ。

// 空きヒープ。内部 RAM と PSRAM を分けて出す。
// mbedTLS は連続した大きい領域を要求するので、総量だけでなく塊も見る。
//
// 「外」は**この機体では常に 0K**。無印 / v1.1 / ADV はどれも PSRAM を持たない
// ESP32-S3FN8 なので、0K であることは機種の区別にならない（CLAUDE.md 参照）。
// 出しているのは、PSRAM のある機体に載せ替えたときに気づけるようにするため。
static void log_heap(const char *tag) {
    opur_log_add("%s 内%uK 塊%uK 外%uK", tag,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
                            / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

// "https://host/path" → "host"。ポート付きや不正な URL は 0。
static int url_host(const char *url, char *out, size_t cap) {
    const char *p = strstr(url, "://");
    const char *e;
    size_t      n;

    p = p ? p + 3 : url;
    e = strchr(p, '/');
    n = e ? (size_t)(e - p) : strlen(p);

    if (n == 0 || n >= cap) return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

// 失敗したときだけ呼ぶ。DNS が引けるかと、mbedTLS が何を言っているか。
static void log_why(WiFiClientSecure &sec) {
    char host[80];
    char err[80];
    int  e;

    // DNS。ここで落ちていれば TLS 以前の問題（名前が引けていない）。
    if (url_host(s_endpoint, host, sizeof(host))) {
        IPAddress ip;
        if (WiFi.hostByName(host, ip)) {
            opur_log_add("DNS %s", ip.toString().c_str());

            // 暗号化なしの素の TCP で 443 を叩いてみる。
            // HTTPClient が返す -1 は「ソケットの失敗」までしか言わないので、
            // 経路の問題（届かない）と TLS の問題（届くが握れない）を
            // ここで分ける。繋がったらすぐ切る。
            {
                WiFiClient plain;
                uint32_t   t0 = millis();
                int        ok;

                plain.setTimeout(5000);
                ok = plain.connect(ip, 443);
                opur_log_add("TCP443 %s %ums", ok ? "OK" : "NG",
                             (unsigned)(millis() - t0));
                plain.stop();
            }
        } else {
            opur_log_add("DNS 失敗 %s", host);
        }
    }

    // mbedTLS の直近のエラー。0 なら TLS まで到達していない。
    err[0] = '\0';
    e = sec.lastError(err, sizeof(err));
    opur_log_add("TLS %d %s", e, err);

    log_heap("失敗時");
}

// ---------------------------------------------------------------------------

int opur_net_check(void) {
    WiFiClientSecure sec;
    HTTPClient       http;
    int              code;
    int              busy;

    if (!ready()) return 0;

    sec.setInsecure();               // 証明書検証はしない（冒頭の注記）

    log_heap("確認前");

    if (!http.begin(sec, s_endpoint)) {
        opur_log_add("確認 begin 失敗");   // URL の形が壊れている
        return -1;
    }

    code = http.GET();
    if (code != HTTP_CODE_OK) {
        opur_log_add("確認 GET %d", code);
        log_why(sec);
        http.end();
        return -1;
    }

    // 改行だけが返ることがあるので、空白を除いてから判定する。
    // 「空」の意味は「引き取り済みで何も置かれていない」であり、
    // 改行 1 文字が置かれている状態は実際には空と同じ。
    {
        String body = http.getString();
        body.trim();
        busy = (body.length() > 0);
    }

    http.end();
    return busy ? 2 : 1;
}

int opur_net_put(const char *text, int len) {
    WiFiClientSecure sec;
    HTTPClient       http;
    int              code;

    if (!ready())            return 0;
    if (!text || len <= 0)   return 0;

    sec.setInsecure();

    log_heap("送信前");

    if (!http.begin(sec, s_endpoint)) {
        opur_log_add("送信 begin 失敗");
        return -1;
    }

    http.addHeader("Content-Type", "text/plain; charset=utf-8");
    code = http.PUT((uint8_t *)text, (size_t)len);

    if (code != HTTP_CODE_OK) {
        opur_log_add("送信 PUT %d", code);
        log_why(sec);
        http.end();
        return -1;
    }

    http.end();
    opur_log_add("送信 OK %dB", len);
    return 1;
}

#else  // !ESP_PLATFORM

// PC ビルド用の空実装。PC 側は WiFi を持たないので、
// 呼ばれても「送る条件が揃っていない」を返すだけでよい。
void opur_net_init(const char *endpoint_url) { (void)endpoint_url; }
int  opur_net_check(void)                    { return 0; }

int opur_net_put(const char *text, int len) {
    (void)text;
    (void)len;
    return 0;
}

#endif  // ESP_PLATFORM
