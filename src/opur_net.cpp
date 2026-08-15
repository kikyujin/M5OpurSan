// opur_net.cpp — EQIDEN セマフォ送信の実装
//
// 手順は 1 回の呼び出しにつき最大 2 往復:
//   1. GET  受け口が空か見る。非空なら相手がまだ引き取っていないので何もしない
//   2. PUT  空いていたら、キューの先頭 1 件を置く
//   3. 置けたら /opur/sent/ へ move する
//
// 1 回に 1 件しか送らない。まとめて送ろうとすると「途中で切れたときどこまで
// 送れたか」を持つ必要が出るが、move が成否そのものなので 1 件ずつなら要らない。
// 残りは次の保存のときに送られる。
//
// HTTPS の証明書は検証しない（setInsecure）。CA バンドルを積むと Flash を
// 数十 KB 使ううえ、Let's Encrypt の中間証明書が変わるたびに焼き直しになる。
// 送っているのは自分で書いた本文であり、盗聴されて困る秘密ではないので、
// 経路の暗号化だけ取って検証は省く。必要になったら CA 埋め込みに替えられる。

#include "opur_net.h"

#ifdef ESP_PLATFORM

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "editor.h"      // OPUR_BUF_MAX — SD 上のファイルの上限を決めている
#include "m5curses.h"    // M5C_SD_MOUNT
#include "opur_log.h"

#define NET_DIR      M5C_SD_MOUNT "/opur"
#define NET_SENT_DIR M5C_SD_MOUNT "/opur/sent"

// 1 ファイルの最大バイト数。本文は UTF-16 で OPUR_BUF_MAX 文字まで、
// UTF-8 に直すと 1 文字最大 3 バイト（BMP のみなので 4 バイトは出ない）。
// main.cpp の save_to_sd() が使う u8[] と同じ大きさ。
#define NET_FILE_MAX (OPUR_BUF_MAX * 3 + 1)

// パス組み立て用。"/sdcard/opur/sent/OPUR_0001.txt" で 32 文字。
#define NET_PATH_MAX 96

static char s_endpoint[OPUR_NET_URL_MAX];

// 送信するファイルの中身。1.5KB あるので loop タスクの 8KB スタックには置かない。
static char s_body[NET_FILE_MAX];

// ---------------------------------------------------------------------------

void opur_net_init(const char *endpoint_url) {
    if (!endpoint_url) {
        s_endpoint[0] = '\0';
        return;
    }
    strncpy(s_endpoint, endpoint_url, sizeof(s_endpoint) - 1);
    s_endpoint[sizeof(s_endpoint) - 1] = '\0';
}

// /opur/ の中で名前が最小の OPUR_*.txt を探す。見つかれば 1。
//
// ファイル名は OPUR_%04d.txt と 0 埋めされているので、名前順 = 番号順。
// つまり「名前が最小 = 一番古い未送信」で、書いた順に送られる。
// sent/ はディレクトリなので sscanf に引っかからず、自然に除外される。
static int find_head(char *name, size_t cap) {
    DIR *d = opendir(NET_DIR);
    struct dirent *e;
    int found = 0;

    if (!d) return 0;

    name[0] = '\0';

    while ((e = readdir(d)) != NULL) {
        int n = 0;
        if (sscanf(e->d_name, "OPUR_%d.txt", &n) != 1) continue;
        if (!found || strcmp(e->d_name, name) < 0) {
            strncpy(name, e->d_name, cap - 1);
            name[cap - 1] = '\0';
            found = 1;
        }
    }

    closedir(d);
    return found;
}

// ファイル全体を s_body に読む。読めたバイト数、失敗なら -1。
// 上限を超えるファイルは -1（切り詰めて送ると本文が欠けるため）。
static int read_file(const char *path) {
    FILE  *fp = fopen(path, "rb");
    size_t n;

    if (!fp) return -1;

    n = fread(s_body, 1, sizeof(s_body) - 1, fp);

    // まだ続きがある = 上限超え。
    if (n == sizeof(s_body) - 1 && fgetc(fp) != EOF) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    s_body[n] = '\0';
    return (int)n;
}

// 受け口が空か見る。1=空いている, 0=塞がっている, -1=エラー。
//
// 改行だけが返ることがあるので、空白を除いてから判定する。
// 「空」の意味は「引き取り済みで何も置かれていない」であり、
// 改行 1 文字が置かれている状態は実際には空と同じ。
static int semaphore_is_free(WiFiClientSecure &sec) {
    HTTPClient http;
    int   code;
    int   free_slot;

    if (!http.begin(sec, s_endpoint)) return -1;

    code = http.GET();
    if (code != HTTP_CODE_OK) {
        opur_log_add("送信 GET %d", code);
        http.end();
        return -1;
    }

    {
        String body = http.getString();
        body.trim();
        free_slot = (body.length() == 0) ? 1 : 0;
    }

    http.end();
    return free_slot;
}

// 本文を置く。1=成功, -1=失敗。
static int put_body(WiFiClientSecure &sec, int len) {
    HTTPClient http;
    int code;

    if (!http.begin(sec, s_endpoint)) return -1;

    http.addHeader("Content-Type", "text/plain; charset=utf-8");
    code = http.PUT((uint8_t *)s_body, (size_t)len);
    http.end();

    if (code != HTTP_CODE_OK) {
        opur_log_add("送信 PUT %d", code);
        return -1;
    }
    return 1;
}

int opur_net_try_send(void) {
    char name[NET_PATH_MAX];
    char src[NET_PATH_MAX];
    char dst[NET_PATH_MAX];
    int  len;
    int  free_slot;

    if (s_endpoint[0] == '\0')             return 0;   // 送信先が設定されていない
    if (WiFi.status() != WL_CONNECTED)     return 0;   // 電波の無い場所。異常ではない
    if (!find_head(name, sizeof(name)))    return 0;   // キューが空

    snprintf(src, sizeof(src), "%s/%s", NET_DIR,      name);
    snprintf(dst, sizeof(dst), "%s/%s", NET_SENT_DIR, name);

    len = read_file(src);
    if (len < 0) {
        opur_log_add("送信 読めず %s", name);
        return -1;
    }
    if (len == 0) {
        // 空ファイルは置いても意味が無いので、送ったことにして退ける。
        // 残しておくと以後ずっとキューの先頭を塞いでしまう。
        mkdir(NET_SENT_DIR, 0777);
        rename(src, dst);
        opur_log_add("送信 空 %s 退避", name);
        return 0;
    }

    {
        WiFiClientSecure sec;
        sec.setInsecure();               // 証明書検証はしない（冒頭の注記）

        free_slot = semaphore_is_free(sec);
        if (free_slot < 0)  return -1;
        if (free_slot == 0) {
            opur_log_add("送信 待ち %s", name);
            return 0;                    // 相手がまだ引き取っていない
        }

        if (put_body(sec, len) < 0) return -1;
    }

    // 送れたので退ける。move できないと次回また同じものを送ってしまうので、
    // 失敗はエラーとして返す（実害は二重送信で、本文が消えるわけではない）。
    mkdir(NET_SENT_DIR, 0777);
    if (rename(src, dst) != 0) {
        opur_log_add("送信済だが移動失敗 %s", name);
        return -1;
    }

    opur_log_add("送信 OK %s %dB", name, len);
    return 1;
}

#else  // !ESP_PLATFORM

// PC ビルド用の空実装。PC 側は SD も WiFi も持たないので、
// 呼ばれても「何もしなかった」を返すだけでよい。
void opur_net_init(const char *endpoint_url) { (void)endpoint_url; }
int  opur_net_try_send(void)                 { return 0; }

#endif  // ESP_PLATFORM
