// main.cpp — M5Cardputer 実機版の統合（Phase E-1 / E-2）
//
// opur_editor/main.cpp（PC 版）と同じ構造。違いは:
//   1. view_curses.c ではなく view_m5.c がリンクされる
//   2. 辞書は SD カードから開く（DICT_PATH）
//   3. Ctrl+Q が無い。実機ではキーボードから制御コードが取れないため（007 調査）
//   4. 書きかけを NVS に自動退避し、起動時に復元する（E-2）
//   5. ESC メニューから SD へ保存できる（E-2）
//
// FEP は C++17 のままなのでこのファイルも C++。それ以外（editor / candidate_bar /
// conv_utf8 / utf8_utf16 / opur_dict / view_m5）は C11 で、
// ヘッダが extern "C" で囲ってあるのでそのまま呼べる。

#include "candidate_bar.h"
#include "conv_utf8.h"
#include "editor.h"
#include "m5curses.h"
#include "opur_config.h"
#include "opur_log.h"
#include "opur_net.h"
#include "opur_wifi.h"
#include "utf8_utf16.h"
#include "view.h"

#include "fep.h"

#include <dirent.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef M5OPUR
  #define DICT_PATH M5C_SD_MOUNT "/dict/system.dic"
#else
  #define DICT_PATH "../dict_tools/output/system.dic"
#endif

#define OPUR_DIR      M5C_SD_MOUNT "/opur"
#define OPUR_SENT_DIR M5C_SD_MOUNT "/opur/sent"
#define CONFIG_PATH   M5C_SD_MOUNT "/config.txt"

// 無操作がこれだけ続いたら NVS に退避する。
//
// 3 秒だと長時間の執筆で数百回書くことになり、20KB しかない NVS
// パーティションの GC が頻発する。10 秒でも「電源を切っても書きかけが残る」
// という目的は変わらない（失うのは最後の 10 秒ぶんまで）。
#define IDLE_SAVE_MS 10000

// NVS
#define NVS_NS      "opur"
#define NVS_K_BUF   "buf"
#define NVS_K_LEN   "buf_len"
#define NVS_K_CUR   "cursor"

// CandBar は約 84KB。スタックにも loop() のローカルにも置けないので、
// 起動時に 1 度だけ確保される静的領域に置く。
// （malloc でも同じだが、静的にしておくと pio run のサイズ表に出るので
//   実機の RAM 残量が一目で分かる。確保失敗の分岐も要らない。）
static CandBar    g_bar;
static OpurEditor g_ed;
static OpurDict   g_dict;
static CFep       g_fep;

// SD の /config.txt の内容。WiFi も将来の送信処理もここを見る。
static OpurConfig g_cfg;

// ---------------------------------------------------------------------------
// パンくず（勝手に再起動したとき、どこまで進んでいたかを次の起動で知る）
// ---------------------------------------------------------------------------
//
// 実機にシリアルを繋げないのでパニック時のバックトレースが読めない。
// RTC メモリはリセット（電源断以外）をまたいで内容が残るので、
// 通過点の番号をここに置いておき、次の起動でログに出す。
//
// RTC_NOINIT_ATTR は起動時にゼロ初期化「されない」領域。電源投入直後は
// 不定値なので、マジックナンバーで有効性を判定する。

#define CRUMB_MAGIC 0x09E20001u

RTC_NOINIT_ATTR static uint32_t g_crumb_magic;
RTC_NOINIT_ATTR static uint32_t g_crumb;

// 補助値。通過点だけでは足りないとき、そのときの数値を一緒に持ち帰る
// （バッファ長・カーソル位置・API の戻り値など）。
RTC_NOINIT_ATTR static int32_t g_crumb_a;
RTC_NOINIT_ATTR static int32_t g_crumb_b;

static inline void crumb2(uint32_t v, int32_t a, int32_t b) {
    g_crumb_magic = CRUMB_MAGIC;
    g_crumb       = v;
    g_crumb_a     = a;
    g_crumb_b     = b;
}

static inline void crumb(uint32_t v) { crumb2(v, 0, 0); }

// 通過点。10 番台 = setup、20 番台 = loop。
#define CRUMB_SETUP_BEGIN   10
#define CRUMB_VIEW_READY    11
#define CRUMB_DICT_DONE     12
#define CRUMB_WIFI_DONE     13
#define CRUMB_SETUP_END     14
#define CRUMB_DRAW          20
#define CRUMB_GETCH         21
#define CRUMB_NVS_SAVE      22
#define CRUMB_NVS_DONE      23
#define CRUMB_KEY           24

static const char *crumb_text(uint32_t c) {
    switch (c) {
    case CRUMB_SETUP_BEGIN: return "setup開始";
    case CRUMB_VIEW_READY:  return "view_init後";
    case CRUMB_DICT_DONE:   return "辞書open後";
    case CRUMB_WIFI_DONE:   return "wifi後";
    case CRUMB_SETUP_END:   return "setup完了";
    case CRUMB_DRAW:        return "描画中";
    case CRUMB_GETCH:       return "キー待ち";
    case CRUMB_NVS_SAVE:    return "NVS退避中";
    case CRUMB_NVS_DONE:    return "NVS退避後";
    case CRUMB_KEY:         return "キー処理中";
    case 220:               return "NVS: 入口";
    case 221:               return "NVS: open後";
    case 222:               return "NVS: blob後";
    case 223:               return "NVS: len後";
    case 224:               return "NVS: cur後";
    case 225:               return "NVS: commit後";
    case 226:               return "NVS: close後";
    default:                return "?";
    }
}

// リセット理由。esp_reset_reason_t の生値。
static const char *reset_text(int r) {
    switch (r) {
    case 1:  return "電源投入";
    case 3:  return "ソフトリセット";
    case 4:  return "パニック(異常終了)";
    case 5:  return "割込WDT";
    case 6:  return "タスクWDT";
    case 7:  return "その他WDT";
    case 9:  return "電圧低下";
    default: return "";
    }
}

static const CandConv g_conv = {
    conv_utf8_to_katakana,
    conv_utf8_to_fullwidth,
    conv_utf8_to_halfwidth,
};

enum Mode { MODE_INPUT, MODE_SELECT, MODE_MENU, MODE_LOG };

static Mode g_mode      = MODE_INPUT;
static bool g_have_dict = false;
static bool g_dirty     = false;   // 前回の退避から本文が変わったか
static int  g_log_top   = 0;       // ログ表示の先頭行

// NVS 退避が失敗した。以後は試さない。
// 壊れた NVS に書き続けるとパニックを繰り返すため、1 度で見切る。
static bool g_nvs_dead  = false;

// ---------------------------------------------------------------------------
// キーマッピング
// ---------------------------------------------------------------------------

// m5curses の getch() の戻り値 → VKEY。
// PC 版の curses_to_vkey() と同じ。m5curses が ncurses と同じキーコードを
// 返すようにしてあるので、この関数は両方で使い回せる。
static VKEY curses_to_vkey(int ch) {
    switch (ch) {
    case KEY_LEFT:      return VK_LEFT;
    case KEY_RIGHT:     return VK_RIGHT;
    case KEY_UP:        return VK_UP;
    case KEY_DOWN:      return VK_DOWN;

    case '\n':
    case '\r':
    case KEY_ENTER:     return VK_ENTER;

    case KEY_ESC:       return VK_ESC;

    case 8:
    case 127:
    case KEY_BACKSPACE:
    case KEY_DC:        return VK_BS;

    case ' ':           return VK_HENKAN;   // 変換キー

    default:
        if (ch >= 0x21 && ch <= 0x7E) return (VKEY)ch;
        return VK_INVALID;
    }
}

// VKEY → candidate_bar のキー。対応がなければ 0（無視される）。
static int vkey_to_candkey(VKEY key) {
    switch (key) {
    case VK_LEFT:   return CAND_KEY_LEFT;
    case VK_RIGHT:  return CAND_KEY_RIGHT;
    case VK_HENKAN: return ' ';             // スペースは次候補
    case VK_ENTER:  return CAND_KEY_ENTER;
    case VK_ESC:    return CAND_KEY_ESC;
    // 変換中の BS は「候補選択をやめて入力に戻る」= キャンセル扱い。
    case VK_BS:     return CAND_KEY_ESC;
    default:
        if (key >= '0' && key <= '9') return (int)key;
        return 0;
    }
}

// ---------------------------------------------------------------------------
// 本文への挿入
// ---------------------------------------------------------------------------

static void insert_utf16(const UTF16* src, int len) {
    for (int i = 0; i < len; i++) {
        opur_insert(&g_ed, (uint16_t)src[i]);
    }
}

// 確定文字列（UTF-8）を本文へ。UTF-8 → UTF-16 の変換はここ 1 箇所だけ。
static void insert_utf8(const char* utf8) {
    static uint16_t u16[CAND_TEXT_MAX];   // 640B。loop のスタックには置かない
    int n = utf8_to_utf16(utf8, u16, CAND_TEXT_MAX);
    for (int i = 0; i < n; i++) {
        opur_insert(&g_ed, u16[i]);
    }
}

// FEP の未確定バッファ（かな + ローマ字途中）を取り出す。文字数を返す。
static int fep_pending(UTF16* out, int maxlen) {
    if (g_fep.GetBuffLen() <= 0) return 0;
    UTF16Array arr(out, maxlen);
    return g_fep.GetItem(0, arr);   // 候補 0 = 生バッファのコピー
}

// ---------------------------------------------------------------------------
// NVS への自動退避
// ---------------------------------------------------------------------------
//
// 電源を切っても書きかけが消えないようにする。SD ではなく内蔵 Flash の NVS を
// 使うのは、SD が挿さっていなくても効いてほしいのと、書き込みが数 ms で済むため。
//
// バッファは UTF-16 のまま blob で置く。UTF-8 に直すと復元時に長さの検算が
// 要るうえ、OPUR_BUF_MAX が文字数で決まっているので素直に対応しない。

// 退避する。失敗しても編集は続けたいので戻り値は返さない。
// パンくずを細かく打ってある。ここでパニックしているのが分かっているが、
// どの呼び出しかまでは絞れていないため（実機にシリアルを繋げないので
// バックトレースが読めない）。原因が判明したら 220 番台は外してよい。
static void nvs_save(void) {
    nvs_handle_t h;
    esp_err_t    e;

    crumb2(220, g_ed.len, g_ed.cursor);

    if (g_nvs_dead || !m5c_nvs_ready()) return;

    e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    crumb2(221, (int32_t)e, g_ed.len);
    if (e != ESP_OK) return;

    e = nvs_set_blob(h, NVS_K_BUF, g_ed.buf,
                     (size_t)g_ed.len * sizeof(g_ed.buf[0]));
    crumb2(222, (int32_t)e, g_ed.len);
    if (e != ESP_OK) goto fail;

    e = nvs_set_u16(h, NVS_K_LEN, (uint16_t)g_ed.len);
    crumb2(223, (int32_t)e, 0);
    if (e != ESP_OK) goto fail;

    e = nvs_set_u16(h, NVS_K_CUR, (uint16_t)g_ed.cursor);
    crumb2(224, (int32_t)e, 0);
    if (e != ESP_OK) goto fail;

    e = nvs_commit(h);
    crumb2(225, (int32_t)e, 0);
    if (e != ESP_OK) goto fail;

    nvs_close(h);
    crumb2(226, 0, 0);
    return;

fail:
    // 一度でも失敗したら以後あきらめる。編集そのものは続けられる。
    nvs_close(h);
    g_nvs_dead = true;
    opur_log_add("NVS退避を停止 err=%d", (int)e);
}

// 退避内容を消す。保存し終えたときと「新規」のとき。
static void nvs_clear(void) {
    nvs_handle_t h;

    if (!m5c_nvs_ready()) return;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_erase_key(h, NVS_K_BUF);
    nvs_erase_key(h, NVS_K_LEN);
    nvs_erase_key(h, NVS_K_CUR);
    nvs_commit(h);
    nvs_close(h);
}

// 起動時の復元。復元したら true。
static bool nvs_restore(void) {
    nvs_handle_t h;
    uint16_t len = 0, cur = 0;
    size_t   blob_size = sizeof(g_ed.buf);
    bool     ok = false;

    if (!m5c_nvs_ready()) return false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    if (nvs_get_u16(h, NVS_K_LEN, &len) == ESP_OK && len > 0 &&
        len <= OPUR_BUF_MAX &&
        nvs_get_blob(h, NVS_K_BUF, g_ed.buf, &blob_size) == ESP_OK &&
        blob_size == (size_t)len * sizeof(g_ed.buf[0])) {

        g_ed.len = (int)len;

        // カーソルは壊れていても致命的ではないので範囲に丸めるだけ
        if (nvs_get_u16(h, NVS_K_CUR, &cur) != ESP_OK) cur = len;
        g_ed.cursor = (cur <= len) ? (int)cur : (int)len;

        g_ed.goal_col = -1;
        opur_update_scroll(&g_ed);
        ok = true;
    }

    nvs_close(h);
    return ok;
}

// ---------------------------------------------------------------------------
// SD への保存
// ---------------------------------------------------------------------------

// dir の中の OPUR_nnnn.txt から最大の nnnn を拾う。無ければ 0。
// 開けないディレクトリは 0 を返す（まだ作られていない場合など）。
static int scan_max_index(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *e;
    int max_n = 0;

    if (!d) return 0;

    while ((e = readdir(d)) != NULL) {
        int n = 0;
        if (sscanf(e->d_name, "OPUR_%d.txt", &n) == 1 && n > max_n) max_n = n;
    }
    closedir(d);
    return max_n;
}

// 本文を /sdcard/opur/OPUR_nnnn.txt に書き出す。
// 成功したら採番した番号、失敗したら -1、本文が空なら 0。
static int save_to_sd(void) {
    static char u8[OPUR_BUF_MAX * 3 + 1];   // UTF-16 1 文字は UTF-8 で最大 3 バイト
    char path[64];
    int  n;
    FILE *fp;
    size_t bytes, written;

    if (g_ed.len <= 0) return 0;            // 空ファイルは作らない
    if (!m5c_sd_ready()) return -1;

    // /opur/ がまだ無いこともある。mkdir は既にあれば失敗するだけなので放置。
    // sent/ も一緒に作る。送信側で作ってもよいが、番号の採番でここが
    // scan_max_index(OPUR_SENT_DIR) を読むので、先に在るほうが素直。
    mkdir(OPUR_DIR, 0777);
    mkdir(OPUR_SENT_DIR, 0777);

    // sent/ に移されたものと番号がぶつからないよう両方見る
    n = scan_max_index(OPUR_DIR);
    {
        int ns = scan_max_index(OPUR_SENT_DIR);
        if (ns > n) n = ns;
    }
    n += 1;

    snprintf(path, sizeof(path), "%s/OPUR_%04d.txt", OPUR_DIR, n);

    bytes = (size_t)utf16_to_utf8(g_ed.buf, g_ed.len, u8, sizeof(u8));

    fp = fopen(path, "wb");
    if (!fp) return -1;
    written = fwrite(u8, 1, bytes, fp);
    fclose(fp);

    return (written == bytes) ? n : -1;
}

// 本文と退避内容を捨てて新規状態にする。
static void start_new(void) {
    opur_init(&g_ed);
    g_fep.ClearMode();
    cand_bar_clear(&g_bar);
    nvs_clear();
    g_dirty = false;
}

// 数行のメッセージを出してキー待ちする。
static void notice(const char *l1, const char *l2) {
    clear();
    mvaddstr(2, 0, l1);
    if (l2) mvaddstr(3, 0, l2);
    mvaddstr(5, 0, "キーを押すと戻ります");
    refresh();

    timeout(-1);            // ここは待ち切る
    getch();
    timeout(IDLE_SAVE_MS);
}

// 辞書が開けなかったときに理由を出す。文字種変換だけなら辞書なしでも動くので、
// 止めずにそのまま続ける。
static void warn_no_dict(void) {
    clear();
    mvaddstr(0, 0, "辞書が開けません");
    mvaddstr(2, 0, m5c_sd_ready() ? "SD: OK" : "SD: マウント失敗");
    mvaddstr(3, 0, DICT_PATH);
    mvaddstr(5, 0, "キーを押すと続行");
    mvaddstr(6, 0, "(文字種変換のみ動きます)");
    refresh();
    getch();
}

// ---------------------------------------------------------------------------
// 起動時の WiFi 接続と NTP 同期
// ---------------------------------------------------------------------------
//
// 結果を下 2 行に出して 2 秒見せる。**元に戻す処理は要らない**。
// この 2 行は view_m5.c の FEP 行（6）と候補バー行（7）で、起動直後は
// どちらも空。loop() の最初の view_draw() が画面をまるごと描き直すため。

#define ROW_STATUS1 (M5C_ROWS - 2)   // = 6。view_m5.c の ROW_FEP
#define ROW_STATUS2 (M5C_ROWS - 1)   // = 7。view_m5.c の ROW_CAND

#define BOOT_STATUS_MS 2000

// 短い知らせ。notice() と違ってキー待ちで止めず、ms 経ったら勝手に戻る。
// 送信結果や「未実装」のように、読めなくても困らないものはこちら。
//
// 元に戻す処理は要らない。呼び出し側が MODE_INPUT に戻れば、
// loop() の次の view_draw() が画面をまるごと描き直す。
static void flash_status(const char *l1, const char *l2, int ms) {
    clear();
    mvaddstr(ROW_STATUS1, 0, l1);
    if (l2) mvaddstr(ROW_STATUS2, 0, l2);
    m5c_separator();
    refresh();

    // キーを押せばすぐ飛ばせる（getch は溜まっていれば待たずに返る）。
    timeout(ms);
    getch();
    timeout(IDLE_SAVE_MS);
}

static void boot_wifi(void) {
    char l1[64], l2[64];
    int  keys;

    opur_config_clear(&g_cfg);
    keys = opur_config_load(&g_cfg, CONFIG_PATH);

    if (keys < 0) opur_log_add("cfg: 読めません");
    else          opur_log_add("cfg: %d 項目", keys);

    // 送信先は WiFi とは別に決まる。WiFi の設定が無くて下で早期に返っても
    // 困らないよう、ここで先に渡しておく。
    opur_net_init(opur_config_endpoint(&g_cfg));
    opur_log_add("送信先: %s",
                 opur_config_endpoint(&g_cfg)[0] ? "あり" : "なし");

    // config.txt が無い / SD が無い / WiFi の設定が書かれていない。
    // どれも異常ではない（電波の無い場所で使うことは普通にある）ので、
    // 何も出さずに通常起動する。エディタ側は WiFi を一切見ていない。
    if (keys < 0 || !opur_config_has_wifi(&g_cfg)) {
        opur_log_add("wifi: 設定なし。skip");
        return;
    }

    opur_log_add("wifi: %s へ接続", g_cfg.wifi_ssid);

    // 接続は最大 3 秒ブロックする。その間ずっと黒画面だと固まったように
    // 見えるので、先に一言出しておく。
    clear();
    mvaddstr(ROW_STATUS1, 0, "WiFi 接続中...");
    refresh();

    if (!opur_wifi_connect(&g_cfg)) {
        // 切り分けの詳細（スキャン・理由コード）は opur_wifi 側がログに残す。
        // ここは 2 行に収まるぶんだけ出す。
        const int r = opur_wifi_last_reason();

        snprintf(l1, sizeof(l1), "WiFi NG st=%d %ums",
                 opur_wifi_last_status(), (unsigned)opur_wifi_elapsed_ms());
        snprintf(l2, sizeof(l2), "理由%d %s", r, opur_wifi_reason_text(r));

        opur_log_add("wifi NG st=%d 計%ums",
                     opur_wifi_last_status(),
                     (unsigned)opur_wifi_elapsed_ms());
        opur_log_add("heap %u>%uK",
                     (unsigned)(opur_wifi_heap_before() / 1024),
                     (unsigned)(opur_wifi_heap_after()  / 1024));
    } else {
        snprintf(l1, sizeof(l1), "WiFi OK %s", opur_wifi_ip());
        opur_log_add("wifi OK %s", opur_wifi_ip());
        opur_log_add("接続 %ums", (unsigned)opur_wifi_elapsed_ms());
        opur_log_add("heap %u>%uK",
                     (unsigned)(opur_wifi_heap_before() / 1024),
                     (unsigned)(opur_wifi_heap_after()  / 1024));

        if (opur_wifi_ntp_sync()) {
            time_t    now = time(NULL);
            struct tm t;
            char      stamp[24];

            localtime_r(&now, &t);
            strftime(stamp, sizeof(stamp), "%m-%d %H:%M", &t);
            // heap は WiFi.begin() の前>後（KB）。WiFi スタックの実コストを
            // 実機で見るための数字なので、画面から読めるようにしてある。
            snprintf(l2, sizeof(l2), "%s heap %u>%uK", stamp,
                     (unsigned)(opur_wifi_heap_before() / 1024),
                     (unsigned)(opur_wifi_heap_after()  / 1024));
        } else {
            snprintf(l2, sizeof(l2), "NTP NG heap %u>%uK",
                     (unsigned)(opur_wifi_heap_before() / 1024),
                     (unsigned)(opur_wifi_heap_after()  / 1024));
        }
    }

    clear();
    mvaddstr(ROW_STATUS1, 0, l1);
    mvaddstr(ROW_STATUS2, 0, l2);
    m5c_separator();
    refresh();

    // delay() を使わないのは、main.cpp に Arduino のヘッダを持ち込まないため。
    // ついでにキーを押せばすぐ飛ばせる（getch は溜まっていれば待たずに返す）。
    timeout(BOOT_STATUS_MS);
    getch();
}

// ---------------------------------------------------------------------------
// ログ画面
// ---------------------------------------------------------------------------
//
// 起動時に溜めたログを後から読む。実機にシリアルを常時繋げないのと、
// ESP32-S3 の USB CDC はリセットで再列挙してホスト側が起動直後を
// 取りこぼすため、本体に溜めて画面で読めるようにしてある。

#define LOG_VIEW_ROWS (M5C_ROWS - 1)   // = 7。最下行は操作の案内に使う

// 表示位置を範囲内に丸める。行数が 1 画面に収まるときは常に 0。
static void log_clamp(void) {
    const int max_top = opur_log_count() - LOG_VIEW_ROWS;

    if (g_log_top > max_top) g_log_top = max_top;
    if (g_log_top < 0)       g_log_top = 0;
}

static void draw_log(void) {
    const int count = opur_log_count();
    char info[40];
    int  r;

    clear();

    for (r = 0; r < LOG_VIEW_ROWS; r++) {
        const int i = g_log_top + r;
        if (i >= count) break;
        mvaddstr(r, 0, opur_log_line(i));
    }

    // 何行目を見ているか。溜まった量が 1 画面を超えたときに要る。
    snprintf(info, sizeof(info), "ESC:戻る %d-%d/%d",
             count ? g_log_top + 1 : 0,
             (g_log_top + LOG_VIEW_ROWS < count) ? g_log_top + LOG_VIEW_ROWS
                                                 : count,
             count);
    mvaddstr(M5C_ROWS - 1, 0, info);

    refresh();
}

static void log_key(int ch) {
    switch (ch) {
    case KEY_UP:   g_log_top -= 1; log_clamp(); break;
    case KEY_DOWN: g_log_top += 1; log_clamp(); break;

    // ログは読むだけなので、抜け方は 1 つで十分。メニューには戻さず
    // そのまま編集に帰る（メニューを経由したい場面が無い）。
    case KEY_ESC:  g_mode = MODE_INPUT;         break;

    default:                                    break;
    }
}

// ---------------------------------------------------------------------------
// ESC メニュー
// ---------------------------------------------------------------------------

// 送信結果・未実装の知らせを出しておく時間（ms）。
// 読み落としても実害が無いので短くしてある。送信結果は次の保存でまた出るし、
// 詳しい理由は「4 ﾛｸﾞ」に残っている。
#define FLASH_MS 1000

// 保存できたあと、キューの先頭 1 件を送ってみる。
// 送信は編集と独立していて、失敗しても本文は SD に残っているので、
// 結果は一言出すだけで止めない。
static void send_after_save(void) {
    switch (opur_net_try_send()) {
    case 1:  flash_status("sent!",    NULL, FLASH_MS); break;
    case -1: flash_status("send err", "ﾛｸﾞ(ESC-4)に理由", FLASH_MS); break;
    default: break;   // 0 = 送る条件が揃っていない。通常運用なので黙る
    }
}

// メニュー表示中のキー処理。
static void menu_key(int ch) {
    switch (ch) {
    case '1': {
        int n = save_to_sd();
        if (n > 0) {
            char msg[48];
            snprintf(msg, sizeof(msg), "OPUR_%04d.txt", n);
            start_new();                     // 保存できたら新規状態へ
            g_mode = MODE_INPUT;
            notice("保存しました", msg);
            // 送信は保存を見せたあと。順番を逆にすると、通信待ちの数秒が
            // 「保存できたのか分からないまま固まっている」ように見える。
            send_after_save();
        } else if (n == 0) {
            // 空ファイルは作らない。メニューは開いたままにして意図を伝える
            notice("本文が空です", "保存しませんでした");
        } else {
            notice("保存できません", m5c_sd_ready() ? OPUR_DIR : "SD が読めません");
        }
        break;
    }

    case '2':
        start_new();
        g_mode = MODE_INPUT;
        break;

    case '3':
        // ロードは 020 で実装する。項目だけ先に出してあるので、
        // 押しても何も起きないと壊れて見える。一言返して戻る。
        g_mode = MODE_INPUT;
        flash_status("ﾛｰﾄﾞは未実装です", NULL, FLASH_MS);
        break;

    case '4':
        // 開いた直後は最新が見えていてほしいので末尾に寄せる
        g_log_top = opur_log_count();
        log_clamp();
        g_mode = MODE_LOG;
        break;

    case KEY_ESC:
        g_mode = MODE_INPUT;
        break;

    default:
        break;                               // それ以外は無視
    }
}

// ---------------------------------------------------------------------------

void setup() {
    // view_init() より前に読む。以降 crumb() で上書きされてしまうため。
    const int      reset_reason = (int)esp_reset_reason();
    const uint32_t prev_crumb   =
        (g_crumb_magic == CRUMB_MAGIC) ? g_crumb : 0;
    const int32_t  prev_a       = g_crumb_a;
    const int32_t  prev_b       = g_crumb_b;

    crumb(CRUMB_SETUP_BEGIN);

    opur_init(&g_ed);

    view_init();                 // 中で initscr() → SD.begin() → NVS 初期化まで済む
    crumb(CRUMB_VIEW_READY);

    opur_log_clear();

    // 前回が異常終了なら、どこまで進んでいたかを最初に出す。
    // 正常な電源投入・ソフトリセットのときは黙っている。
    if (reset_reason != 1 && reset_reason != 3) {
        opur_log_add("前回 %s", reset_text(reset_reason));
        opur_log_add("落ちた場所 %u %s", prev_crumb, crumb_text(prev_crumb));
        opur_log_add("  a=%ld b=%ld", (long)prev_a, (long)prev_b);

        // NVS の書き込み中に落ちていた場合、ページが半端な状態で残って
        // 次の書き込みでも同じ場所でパニックする（クラッシュの自己再生産）。
        // 抜け出すには消して作り直すしかない。書きかけは失われるが、
        // 起動のたびに落ちる状態よりはましと判断する。
        if (prev_crumb >= 220 && prev_crumb <= 226) {
            opur_log_add("NVS を作り直します");
            opur_log_add("NVS 再構築 %s", m5c_nvs_reset() ? "OK" : "失敗");
        }
    }
    opur_log_add("SD: %s",  m5c_sd_ready()  ? "OK" : "マウント失敗");
    opur_log_add("NVS: %s", m5c_nvs_ready() ? "OK" : "NG");

    g_have_dict = (opur_dict_open(&g_dict, DICT_PATH) == 0);
    cand_bar_init(&g_bar, g_have_dict ? &g_dict : NULL, &g_conv);

    opur_log_add("辞書: %s", g_have_dict ? "OK" : "開けません");
    crumb(CRUMB_DICT_DONE);

    if (!g_have_dict) warn_no_dict();

    // WiFi は編集機能とは独立している。繋がらなくてもここから先は同じ。
    boot_wifi();
    crumb(CRUMB_WIFI_DONE);

    // 電源を切る前の書きかけを戻す。復元直後は退避済みなので dirty にしない。
    nvs_restore();
    g_dirty = false;

    timeout(IDLE_SAVE_MS);
    crumb(CRUMB_SETUP_END);
}

static void handle_key(int ch) {
    // --- ログ表示中 ---
    if (g_mode == MODE_LOG) {
        log_key(ch);
        return;
    }

    // --- メニュー表示中 ---
    if (g_mode == MODE_MENU) {
        menu_key(ch);
        return;
    }

    VKEY key = curses_to_vkey(ch);
    if (key == VK_INVALID) return;

    // --- 候補選択中 ---
    if (g_mode == MODE_SELECT) {
        int ckey = vkey_to_candkey(key);
        if (ckey == 0) return;

        switch (cand_bar_key(&g_bar, ckey)) {
        case CAND_COMMITTED:
            insert_utf8(cand_bar_committed(&g_bar));
            g_fep.ClearMode();
            g_mode = MODE_INPUT;
            break;
        case CAND_CANCELLED:
            // 読みは FEP バッファに残っているので入力の続きから再開できる
            g_mode = MODE_INPUT;
            break;
        default:
            break;
        }
        return;
    }

    // --- 入力中: スペースは横取りして変換を開始する ---
    if (key == VK_HENKAN && g_fep.GetBuffLen() > 0) {
        static UTF16 buf[FEP_MAXBUFF];
        char reading[CAND_READING_MAX];
        int n = fep_pending(buf, FEP_MAXBUFF);

        utf16_to_utf8(buf, n, reading, sizeof(reading));
        if (cand_bar_start(&g_bar, reading) > 0) g_mode = MODE_SELECT;
        return;
    }

    // --- 入力中: FEP に渡す ---
    {
        static UTF16 edit[FEP_MAXBUFF];
        UTF16Array to_edit(edit, FEP_MAXBUFF);

        switch (g_fep.KeyPress(key, to_edit)) {
        case FEP_INSERTED:
            // 変換前 Enter。ひらがなをそのまま本文へ
            insert_utf16(edit, to_edit.GetSize());
            break;

        case FEP_UPDATE_DISP:
            break;   // FEP 行が変わっただけ。次のループ先頭で描き直す

        case FEP_THRU:
            // FEP が関与しないキー。エディタ側で処理する
            switch (key) {
            case VK_LEFT:   opur_left(&g_ed);              break;
            case VK_RIGHT:  opur_right(&g_ed);             break;
            case VK_UP:     opur_up(&g_ed);                break;
            case VK_DOWN:   opur_down(&g_ed);              break;
            case VK_ENTER:  opur_insert(&g_ed, OPUR_LF);   break;
            case VK_BS:     opur_backspace(&g_ed);         break;
            case VK_HENKAN: opur_insert(&g_ed, ' ');       break;
            // FEP バッファが空のときの ESC はここに落ちてくる（fep.cpp の
            // InputMode を参照）。「変換中でないエディタモード」の判定を
            // FEP の状態に任せられるので、こちらで持たなくて済む。
            case VK_ESC:    g_mode = MODE_MENU;            break;
            default:                                       break;
            }
            break;

        case FEP_ERROR:
        default:
            break;
        }
    }
}

void loop() {
    static UTF16 pending[FEP_MAXBUFF];
    int pending_len = fep_pending(pending, FEP_MAXBUFF);

    // ログは本文を全部隠すので view_draw() には通さない。
    // 通すと view.h に「ログ」という編集と無関係な状態を足すことになる。
    crumb(CRUMB_DRAW);

    if (g_mode == MODE_LOG) {
        draw_log();
    } else {
        view_draw(&g_ed, pending, pending_len,
                  (g_mode == MODE_SELECT) ? &g_bar : NULL,
                  (g_mode == MODE_MENU) ? 1 : 0);
    }

    crumb(CRUMB_GETCH);
    int ch = getch();

    // 無操作が続いた。変わっていれば退避する（数 ms なので気づかれない）
    if (ch == ERR) {
        if (g_dirty) {
            crumb(CRUMB_NVS_SAVE);
            nvs_save();
            crumb(CRUMB_NVS_DONE);
            g_dirty = false;
        }
        return;
    }

    // 本文かカーソルが実際に動いたときだけ dirty にする。
    // 「キーが来たら dirty」にすると、候補を眺めただけ・メニューを開いただけでも
    // 退避が走ってしまう。前後を見比べるほうが確実で行数も少ない。
    {
        const int before_len = g_ed.len;
        const int before_cur = g_ed.cursor;

        crumb(CRUMB_KEY);
        handle_key(ch);

        if (g_ed.len != before_len || g_ed.cursor != before_cur) g_dirty = true;
    }
}
