// main.cpp — M5Cardputer 実機版の統合（Phase E-1 / E-2）
//
// opur_editor/main.cpp（PC 版）と同じ構造。違いは:
//   1. view_curses.c ではなく view_m5.c がリンクされる
//   2. 辞書は SD カードから開く（DICT_PATH）
//   3. Ctrl+Q が無い。実機ではキーボードから制御コードが取れないため（007 調査）
//   4. ESC メニューから SD へ保存できる（E-2）
//
// 書きかけの NVS 自動退避は 021 で撤去した。保存前に電源を切れば消える
// （ワープロと同じ）。理由は nvs_save() があった場所の注記に残してある。
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
#include "opur_sleep.h"
#include "opur_wifi.h"
#include "utf8_utf16.h"
#include "view.h"

#include "fep.h"

#include <dirent.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef M5OPUR
  #define DICT_PATH M5C_SD_MOUNT "/dict/system.dic"
#else
  #define DICT_PATH "../dict/output/system.dic"
#endif

#define OPUR_DIR      M5C_SD_MOUNT "/opur"
#define CONFIG_PATH   M5C_SD_MOUNT "/config.txt"

// ディープスリープに落ちる直前の書きかけ。中身は保存したメモと同じ UTF-8 の
// 生テキストで、ヘッダは持たない（ファイル名だけが判別子）。
//
// 起動時にあれば読んで消す。**フラグは持たない**——ファイルの有無がそのまま
// 状態なので、二重に読む心配も、NVS の書き込み（021 のパニック源）も要らない。
#define AUTOSAVE_PATH M5C_SD_MOUNT "/AUTOSAVE.txt"

// 開いている（= 次の保存で上書きする）ファイル名。空なら新規。
// "OPUR_0016.txt" で 13 文字。9999 を超えて 5 桁になっても収まる。
static char s_current_file[16];

// いま本文にあるのはメモではなく /config.txt。保存すると設定として書き戻す。
//
// **保存・新規・読込のどれでも抜ける。** 設定を開いたままメモを書き続けて
// うっかり config.txt を潰す、という事故が起きないようにするため。
static bool s_config_mode = false;

// ロードのファイル一覧。64 件あれば十分（256 バイト）。
#define LOAD_MAX_FILES 64

// 入力待ちの上限（ms）。過ぎたら getch() が ERR を返し、loop() が
// 何もせず描き直す。**それだけのために置いてある**——右下の時計は
// view_draw() の中で描かれるので、描き直さないと止まって見える。
//
// 021 で NVS 退避をやめたとき待ち切る（-1）ようにしたら、キーを打たない間
// 時計が固まった。HH:MM 表示なので、分の変わり目は最大これだけ遅れる。
#define IDLE_REDRAW_MS 30000

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
#define CRUMB_KEY           24   /* 22/23 は NVS 退避用だった。021 で撤去 */
// スリープ経路。ここで落ちると WDT リセットになり、パニック経路を通らないので
// **コアダンプが残らない**（2026-08-16 に実際に踏んだ）。落ちた場所を知る手段が
// パンくずしか無いため、突入前・復帰直後・ディープ突入前の 3 点に置いてある。
#define CRUMB_SLEEP_ENTER   25
#define CRUMB_SLEEP_WOKE    26   /* a = 起きた理由（OPUR_WAKE_*） */
#define CRUMB_SLEEP_DEEP    27
// 無操作で getch() が ERR を返したあとの処理。これが無いと、21「キー待ち」が
// getch() 本体と ERR 枝の両方を指してしまい、場所が絞れない
// （2026-08-16 の割込 WDT がまさにこれで絞れなかった）。
#define CRUMB_IDLE          28

static const char *crumb_text(uint32_t c) {
    switch (c) {
    case CRUMB_SETUP_BEGIN: return "setup開始";
    case CRUMB_VIEW_READY:  return "view_init後";
    case CRUMB_DICT_DONE:   return "辞書open後";
    case CRUMB_WIFI_DONE:   return "wifi後";
    case CRUMB_SETUP_END:   return "setup完了";
    case CRUMB_DRAW:        return "描画中";
    case CRUMB_GETCH:       return "キー待ち";
    case CRUMB_KEY:         return "キー処理中";
    case CRUMB_SLEEP_ENTER: return "スリープ突入";
    case CRUMB_SLEEP_WOKE:  return "スリープ復帰";
    case CRUMB_SLEEP_DEEP:  return "ディープ突入";
    case CRUMB_IDLE:        return "無操作処理";
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
    case 8:  return "ディープスリープ復帰";
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
// 保存していない変更があるか。読込のときに「保存しますか?」と聞くためだけに
// 使う（021 で NVS 退避を撤去したので、これ以外の用途は無い）。
static bool g_dirty     = false;
static int  g_log_top   = 0;       // ログ表示の先頭行

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
// 書きかけの扱い（021 で NVS 退避を撤去した）
// ---------------------------------------------------------------------------
//
// 以前は無操作 10 秒ごとに本文を内蔵 Flash の NVS へ退避し、起動時に戻していた。
// これをやめて、起動は常に空バッファから始める。保存前に電源を切れば消える。
//
// 撤去した理由は、NVS の書き込み自体がパニックの発生源になっていたため。
// nvs_open() が内部ミューテックスを取る時点で assert に当たっており
// （xQueueSemaphoreTake の uxItemSize == 0）、データの問題ではないので
// パーティションを消しても直らなかった。復旧のために置いていた
// m5c_nvs_reset() が nvs_flash_deinit() を呼ぶこと自体を疑っているが、
// 確証は無い。詳細は claude-store/opur/2026-08-15_nvs_panic_investigation.md。
//
// 守れる範囲が「最後の 10 秒を除く書きかけ」しかないのに対して、
// 失敗すると起動のたびに落ちる。割に合わないと判断した。
// 保存したものは SD にあり、読込で取り戻せる。

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

// 本文を SD に書き出す。
//
// s_current_file が空なら新しい番号を採番し、非空ならそのファイルを上書きする。
// 採番は「いまある最大 + 1」で、欠番は埋めない（0001,0003,0007 の次は 0008）。
// 埋めてしまうと、消したファイルの番号が別の文書に再利用されて紛らわしい。
//
// 成功したら 1、失敗したら -1、本文が空なら 0。
// 保存したファイル名は s_current_file に入る（続けて保存すれば上書きになる）。
// 本文を指定パスへ UTF-8 で書く。成功したら 1、失敗したら -1。
//
// パスの決め方（採番・/opur/・config.txt）は呼び出し側の関心事なので分けてある。
// AUTOSAVE も同じ形式で書きたいだけなので、ここだけを共有する。
static int write_text_to(const char *path) {
    static char u8[OPUR_BUF_MAX * 3 + 1];   // UTF-16 1 文字は UTF-8 で最大 3 バイト
    FILE *fp;
    size_t bytes, written;

    bytes = (size_t)utf16_to_utf8(g_ed.buf, g_ed.len, u8, sizeof(u8));

    fp = fopen(path, "wb");
    if (!fp) return -1;
    written = fwrite(u8, 1, bytes, fp);
    fclose(fp);

    return (written == bytes) ? 1 : -1;
}

static int save_to_sd(void) {
    char path[80];

    if (g_ed.len <= 0) return 0;            // 空ファイルは作らない
    if (!m5c_sd_ready()) return -1;

    if (s_config_mode) {
        // 設定は採番も /opur/ も関係ない。決まった 1 箇所に書き戻す。
        snprintf(path, sizeof(path), "%s", CONFIG_PATH);
    } else {
        // /opur/ がまだ無いこともある。mkdir は既にあれば失敗するだけなので放置。
        mkdir(OPUR_DIR, 0777);

        if (s_current_file[0] == '\0') {
            const int n = scan_max_index(OPUR_DIR) + 1;
            snprintf(s_current_file, sizeof(s_current_file), "OPUR_%04d.txt", n);
        }
        snprintf(path, sizeof(path), "%s/%s", OPUR_DIR, s_current_file);
    }

    return write_text_to(path);
}

// SD のファイルを本文に読み込む。成功したら 1、失敗したら -1。
// path は SD 上のフルパス（メモも /config.txt も同じ経路で読む）。
//
// SD 上は UTF-8、エディタバッファは UTF-16。変換は utf8_to_utf16() が
// 既にあるのでそれを使う（本文への挿入 insert_utf8 と同じ経路）。
static int load_from_sd(const char *path) {
    static char u8[OPUR_BUF_MAX * 3 + 1];
    FILE  *fp;
    size_t nread;
    int    n;

    if (!m5c_sd_ready()) return -1;

    fp = fopen(path, "rb");
    if (!fp) return -1;

    nread = fread(u8, 1, sizeof(u8) - 1, fp);
    fclose(fp);
    u8[nread] = '\0';

    // 変換先はバッファそのもの。入り切らないぶんは捨てられる
    // （utf8_to_utf16 は dst_max で止まり、常に 0 以上を返す）。
    // 空ファイルなら 0 文字。それも正常な結果として扱う。
    n = utf8_to_utf16(u8, g_ed.buf, OPUR_BUF_MAX);

    g_ed.len      = n;
    g_ed.cursor   = 0;
    g_ed.goal_col = -1;
    g_ed.scroll_top = 0;
    opur_update_scroll(&g_ed);

    return 1;
}

// 本文を捨てて新規状態にする。
// 開いていたファイルも忘れる（次の保存は新しい番号になる）。
static void start_new(void) {
    opur_init(&g_ed);
    g_fep.ClearMode();
    cand_bar_clear(&g_bar);
    g_dirty = false;
    s_current_file[0] = '\0';
    s_config_mode     = false;   // 設定を開いていたなら、ここで抜ける
}

// 選択肢を出してキーを待ち、押されたキーを返す。
// notice() と違って呼び出し側が分岐する（「保存する？ 1:はい 2:いいえ」など）。
static int ask(const char *l1, const char *l2) {
    int ch;

    clear();
    mvaddstr(2, 0, l1);
    if (l2) mvaddstr(3, 0, l2);
    refresh();

    // ここは待ち切る。時間切れで ERR を返すと「いいえ」と区別が付かず、
    // 席を外しただけで書きかけを捨てる判断になってしまう。
    timeout(-1);
    ch = getch();
    timeout(IDLE_REDRAW_MS);
    return ch;
}

// 数行のメッセージを出してキー待ちする。
static void notice(const char *l1, const char *l2) {
    clear();
    mvaddstr(2, 0, l1);
    if (l2) mvaddstr(3, 0, l2);
    mvaddstr(5, 0, "キーを押すと戻ります");
    refresh();

    timeout(-1);            // 読むまで消さない
    getch();
    timeout(IDLE_REDRAW_MS);
}

// 本文を捨てる操作（読込・新規）の前に、書きかけの始末をつける。
// 続けてよければ 1、やめるなら 0。
//
// **読込と新規で同じ問い方をする。** 片方だけ黙って捨てると、
// 「メニューから抜けたつもりが消えていた」が起きる。
static int confirm_discard(const char *giveup_msg) {
    int ch;

    if (!g_dirty) return 1;

    ch = ask("保存しますか?", "1:はい 2:いいえ ESC:中止");

    if (ch == '1') {
        if (save_to_sd() < 0) {
            notice("保存できません", giveup_msg);
            return 0;
        }
        g_dirty = false;
        return 1;
    }

    // '2' だけが「捨ててよい」。ESC も誤爆も、捨てない側に倒す。
    return (ch == '2') ? 1 : 0;
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

// 起動時に間に合わなかった WiFi を、あとから拾い直すための状態。
//
//   s_wifi_logged  「繋がった」をログに書いたか（二重に書かないため）
//   s_ntp_tries    遅延 NTP の試行回数。届かない環境で毎回待たされないよう打ち切る
static bool s_wifi_logged = false;
static int  s_ntp_tries   = 0;

#define NTP_LATE_TRIES      3
#define NTP_LATE_TIMEOUT_MS 2000

// 知らせを出しておく時間（ms）。読み落としても実害が無いものに使う。
// 詳しい理由は隠しの「5 ログ」に残っている。
#define FLASH_MS 1000

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
    timeout(IDLE_REDRAW_MS);
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
        const int  r    = opur_wifi_last_reason();
        const bool wait = opur_wifi_last_pending();

        // **「NG」と言い切らない。** WiFi.begin() は非同期なので、
        // 待ち時間内に間に合わなかっただけで、このあと繋がることがある
        // （実際そうなり、送信は通るのにログだけ NG のままになっていた）。
        // 後始末は wifi_catch_up() がやる。
        snprintf(l1, sizeof(l1), "WiFi %s st=%d %ums",
                 wait ? "待ち" : "NG",
                 opur_wifi_last_status(), (unsigned)opur_wifi_elapsed_ms());
        snprintf(l2, sizeof(l2), "理由%d %s", r, opur_wifi_reason_text(r));

        opur_log_add("wifi %s st=%d 計%ums",
                     wait ? "時間切れ(継続中)" : "NG",
                     opur_wifi_last_status(),
                     (unsigned)opur_wifi_elapsed_ms());
        opur_log_add("heap %u>%uK",
                     (unsigned)(opur_wifi_heap_before() / 1024),
                     (unsigned)(opur_wifi_heap_after()  / 1024));
    } else {
        s_wifi_logged = true;
        snprintf(l1, sizeof(l1), "WiFi OK %s", opur_wifi_ip());
        opur_log_add("wifi OK %s", opur_wifi_ip());
        opur_log_add("接続 %ums", (unsigned)opur_wifi_elapsed_ms());
        opur_log_add("heap %u>%uK",
                     (unsigned)(opur_wifi_heap_before() / 1024),
                     (unsigned)(opur_wifi_heap_after()  / 1024));

        if (opur_wifi_ntp_sync(OPUR_NTP_TIMEOUT_MS)) {
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

// 起動時に間に合わなかった WiFi を拾い直す。無操作の再描画から呼ぶ。
//
// 送信は毎回 opur_net_check() が WiFi.status() を見るので、繋がりさえすれば
// 動く。残るのは次の 2 つで、どちらも起動時に 1 度きりの判定に引きずられる:
//   - ログが「NG」のままになり、実態と食い違う
//   - NTP が一度も走らず、右下の時計がいつまでも出ない
//
// ブロックするのは NTP の待ちだけ。起動時の 8 秒は「まだ何も届いていない」
// 前提の値なので、ここでは短く聞いて画面を止めない。
static void wifi_catch_up(void) {
    if (!opur_config_has_wifi(&g_cfg))        return;   // そもそも使わない
    if (s_wifi_logged && opur_wifi_ntp_synced()) return; // やることが無い
    if (!opur_wifi_is_connected())            return;   // まだ。次の周回で見る

    if (!s_wifi_logged) {
        s_wifi_logged = true;
        opur_log_add("wifi 遅れてOK %s", opur_wifi_ip());
    }

    if (!opur_wifi_ntp_synced() && s_ntp_tries < NTP_LATE_TRIES) {
        s_ntp_tries++;
        opur_wifi_ntp_sync(NTP_LATE_TIMEOUT_MS);   // 結果は opur_wifi 側がログに残す
    }
}

// ---------------------------------------------------------------------------
// AUTOSAVE
// ---------------------------------------------------------------------------
//
// ディープスリープは RAM を捨てるので、その前に本文を SD へ逃がす。
// 手動保存（ESC→1 の OPUR_%04d.txt）とは完全に独立していて、
// 番号も s_current_file も動かさない。

// 本文を AUTOSAVE.txt へ書く。空なら何もしない（空ファイルを作らない）。
static void autosave_write(void) {
    if (g_ed.len <= 0)   return;
    if (!m5c_sd_ready()) return;

    if (write_text_to(AUTOSAVE_PATH) == 1) opur_log_add("AUTOSAVE %d字", g_ed.len);
    else                                   opur_log_add("AUTOSAVE 書けません");
}

// 起動時。AUTOSAVE.txt があれば本文に戻して消す。
//
// **読めたときだけ消す。** 読めないファイルを消すと書きかけが完全に消える。
// 残しておけば次の起動でもう一度試せるし、ログに理由が残る。
static void autosave_restore(void) {
    struct stat st;

    if (!m5c_sd_ready())                  return;
    if (stat(AUTOSAVE_PATH, &st) != 0)    return;   // 無い = 通常の白紙起動

    if (load_from_sd(AUTOSAVE_PATH) != 1) {
        opur_log_add("AUTOSAVE 読めません");
        return;
    }

    remove(AUTOSAVE_PATH);
    opur_log_add("AUTOSAVE 復元 %d字", g_ed.len);

    // 手動保存はされていないので未保存扱い。ファイル名も持たせない
    // （次の保存で新しい番号が採られる）。
    g_dirty = true;
}

// ---------------------------------------------------------------------------
// スリープ
// ---------------------------------------------------------------------------
//
//   [Active] --1分無操作--> [Light Sleep] --10分--> [AUTOSAVE → Deep Sleep]
//
// 判定は loop() の「無操作で ERR が返った」枝に置いてある。getch() の中で
// 寝たほうが反応は良いが、m5curses が電源管理を持つことになる。あそこは
// PC 版と対になる curses 互換の薄層なので、役割を増やしたくない。
//
// **ライトスリープにタイマーを付けている。** 指示 2-1 には「タイマー不要」と
// あるが、キーでしか起きないと 2-2 の「10 分で Deep Sleep」に永久に届かない。
// キー以外で起きる理由が無い以上、10 分ぶんの累積カウントは
// 「10 分のタイマーを 1 本張る」のと同じことになる。
//
// キーウェイクが使えない機体（無印 / v1.1）では**一切寝ない**。
// 理由は opur_sleep.h の注記を参照。

// **ディープスリープは止めてある。** 実装は入っていて（AUTOSAVE →
// ext1 キーウェイク → 2 分タイマー）ビルドも通るが、実機で一度も
// 通しで確認できていない。落ちると USB CDC が切れて焼けなくなるので、
// 検証の段取りが取れるまでは入らないようにしておく。
//
// 1 に戻せばそのまま有効になる。そのときは
//   - 焼く前に本体スイッチ OFF（CLAUDE.md 参照）
//   - 10 分 → AUTOSAVE → ディープ → 2 分で自動復帰、を通しで確認する
// ライトスリープ（1 分で画面 OFF・キーで復帰）は止めていない。
#define DEEP_SLEEP_ENABLED 0

#define IDLE_LIGHT_MS   60000UL   // 無操作 1 分でライトスリープ
#define LIGHT_TOTAL_MS 600000UL   // ライトスリープ 10 分でディープへ
#define DEEP_TIMER_MS  120000UL   // ディープからの復帰タイマー 2 分

// 最後にキーが来た時刻（ms）。
//
// millis() ではなく esp_timer_get_time() を使うのは、このファイルに Arduino の
// ヘッダを持ち込まないため（flash_status が delay() を避けているのと同じ理由）。
static uint32_t s_last_key_ms = 0;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// 画面と WiFi を落としてライトスリープに入り、起きたら戻す。
//
// バッファは RAM 上にそのまま残る（ライトスリープは RAM を保持する）ので、
// 復帰後は loop() の次の view_draw() が続きを描くだけでいい。
//
// WiFi は落とす。このビルドには CONFIG_PM_ENABLE が入っておらず自動ライト
// スリープが使えないため、手で寝ている間は WiFi のタスクも止まってビーコンを
// 取りこぼす。繋いだままにすると AP から切られる。再接続は実測 100ms。
static void light_sleep_cycle(void) {
    const bool had_wifi = opur_wifi_is_connected() != 0;

    // 電力実験の材料。**1 周につきここ 1 行だけ**にしてある（ログは 32 行の
    // リングなので、入眠と起床で 2 行使うと履歴が半分しか残らない）。
    // 起床は「次の『寝る』が来ていること」で分かるので、別に出さなくていい。
    // 寝たということは 100% 未満だったはず。100% が出ていたら
    // on_wall_power() の判定が抜けている。
    opur_log_add("寝る 電池%d%% %dmV",
                 m5c_battery_level(), m5c_battery_mv());

    m5c_display_off();
    if (had_wifi) opur_wifi_disconnect();

    // キーが来れば戻ってくる。次の段（ディープ）へ進むためのタイマーは、
    // ディープを止めているあいだは張らない —— 張っても 10 分後にただ起きて
    // Active に戻るだけで、電気を使うだけになる。
    crumb(CRUMB_SLEEP_ENTER);
    const int wake = opur_sleep_light(DEEP_SLEEP_ENABLED ? LIGHT_TOTAL_MS : 0);
    crumb2(CRUMB_SLEEP_WOKE, wake, 0);

    if (DEEP_SLEEP_ENABLED && wake == OPUR_WAKE_TIMER) {
        // 画面も WiFi も落ちたまま、そのままディープへ落ちる。**戻らない。**
        // ここで点け直すと、誰も見ていない画面を数百 ms 光らせるだけになる。
        autosave_write();
        opur_log_add("deep sleep へ 電池%d%% %dmV",
                     m5c_battery_level(), m5c_battery_mv());
        crumb(CRUMB_SLEEP_DEEP);
        opur_sleep_deep(DEEP_TIMER_MS);
    }

    // 先に画面を点ける。Canvas は触っていないので直前の絵がそのまま出る。
    // WiFi の再接続は最大 3 秒ブロックするので、そのあとに回す。
    m5c_display_on();
    if (had_wifi) opur_wifi_connect(&g_cfg);

    s_last_key_ms = now_ms();
}

// 給電されているか。
//
// Cardputer では充電中かどうかを直接取れない（M5Unified の isCharging() は
// この機種を扱わない）ので、目印は残量 100% ひとつだけ。
//
// **これが当てになるのは本体スイッチ OFF のとき。** その場合 USB が唯一の
// 電源なので必ず 100% を指す。スイッチ ON だとバッテリーの実残量が出るため、
// 充電中でも 100% に届かない（マスターの実機観察）。
//
// **焼くときにスイッチ OFF で繋ぐのはこのため。** 判定はこの 1 本しか無く、
// USB が挿さっているかは見ていない（見られるものが無い。m5curses.h の注記を
// 参照）。ここで寝られると USB CDC が切れて焼けなくなる
// （2026-08-16 にそれで実機を半分壊した）。
//
// 残る穴は「スイッチ ON ＋ 充電器だけ ＋ 満充電でない」。ホストが居ないので
// 焼く事故は起きず、寝ても充電は続くので実害は小さい。
#define BATTERY_POWERED_PCT 100

static bool on_wall_power(void) {
    return m5c_battery_level() >= BATTERY_POWERED_PCT;
}

// 無操作が続いていたら寝る。loop() の ERR 枝から毎回呼ばれる。
static void idle_check(void) {
    // ライトスリープを止めているあいだは、寝る手前で引き返す。
    //
    // **opur_sleep_light() 側の停止だけでは足りない。** あちらが即座に返しても、
    // light_sleep_cycle() の画面 OFF / WiFi 切断はこちらの担当なので、
    // 無操作のあいだ「消して点けて、切って繋いで」を延々と繰り返してしまう。
    // 理由と復活の手順は opur_sleep.h の opur_sleep_light_enabled() の注記。
    if (!opur_sleep_light_enabled()) return;

    if (!opur_sleep_key_wake_supported()) return;   // 起きられないので寝ない

    // 給電中は寝ない。省電力の意味が無いうえ、ディープスリープに落ちると
    // USB CDC が切れて焼けなくなる。
    //
    // m5c_battery_level() は 30 秒キャッシュだが、判定するのは無操作 1 分の
    // タイムアウト後なので遅れは問題にならない。
    if (on_wall_power()) return;

    if ((now_ms() - s_last_key_ms) < IDLE_LIGHT_MS) return;

    light_sleep_cycle();
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

// /opur/ の OPUR_*.txt の番号を集めて昇順に並べる。件数を返す。
// 欠番があっても詰めて並ぶだけなので、飛び番の一覧をそのまま辿れる。
static int scan_file_list(int *nums, int cap) {
    DIR *d = opendir(OPUR_DIR);
    struct dirent *e;
    int count = 0;
    int i;

    if (!d) return 0;

    while ((e = readdir(d)) != NULL && count < cap) {
        int n = 0;
        if (sscanf(e->d_name, "OPUR_%d.txt", &n) != 1) continue;
        nums[count++] = n;
    }
    closedir(d);

    // readdir の順は保証されないので自分で並べる。
    // 高々 LOAD_MAX_FILES 件なので挿入ソートで足りる。
    for (i = 1; i < count; i++) {
        const int v = nums[i];
        int j = i - 1;
        while (j >= 0 && nums[j] > v) { nums[j + 1] = nums[j]; j--; }
        nums[j + 1] = v;
    }
    return count;
}

// ロード。ファイルを選ばせ、選ばれたものを本文に読み込む。
//
// 選択中のファイルの中身をそのまま本文に読み込んで view_draw() に描かせる
// （＝プレビュー）。専用の描画を書かずに済み、実際に読み込んだ結果が
// そのまま見えるので「開いてみたら違った」が起きない。
// 中止したときのために、入る前の本文を退避しておく。
static void do_load(void) {
    // どちらも loop タスクの 8KB スタックには置けないので静的に。
    static uint16_t saved_buf[OPUR_BUF_MAX];
    static int      nums[LOAD_MAX_FILES];

    char fname[16];
    char fpath[80];
    int  saved_len, saved_cursor;
    int  count, sel = 0;

    // 書きかけがあるなら先に始末をつける。黙って捨てない。
    if (!confirm_discard("読込をやめます")) return;

    count = scan_file_list(nums, LOAD_MAX_FILES);
    if (count == 0) {
        flash_status("ファイルがありません", NULL, FLASH_MS);
        return;
    }

    saved_len    = g_ed.len;
    saved_cursor = g_ed.cursor;
    memcpy(saved_buf, g_ed.buf, (size_t)saved_len * sizeof(g_ed.buf[0]));

    for (;;) {
        char l1[48];
        int  ch;

        snprintf(fname, sizeof(fname), "OPUR_%04d.txt", nums[sel]);
        snprintf(fpath, sizeof(fpath), "%s/%s", OPUR_DIR, fname);

        // 読めないファイルも一覧には出す。中身を空にして選択は続けられる。
        if (load_from_sd(fpath) < 0) opur_init(&g_ed);

        // 本文は view に描かせ、下 2 行だけ上書きする。
        // ここだけのためにビューへ新しいモードを足すほどではない。
        view_draw(&g_ed, NULL, 0, NULL, 0);

        // 記号は < > を使う。◀ ▶ は JIS X 0208 に無く efont では豆腐になる
        // （candidate_bar / view_m5.c の draw_cand と同じ理由）。
        snprintf(l1, sizeof(l1), "< %s > %d/%d", fname, sel + 1, count);
        mvaddstr(ROW_STATUS1, 0, l1);
        mvaddstr(ROW_STATUS2, 0, "Enter:読込 ESC:戻る");
        m5c_separator();
        refresh();

        // 選んでいる間は待ち切る。時間切れで戻ってきても、この周回は
        // プレビューを SD から読み直すだけで何も進まない。
        timeout(-1);
        ch = getch();
        timeout(IDLE_REDRAW_MS);

        if (ch == KEY_LEFT)  { sel = (sel + count - 1) % count; continue; }
        if (ch == KEY_RIGHT) { sel = (sel + 1) % count;         continue; }

        if (ch == '\r' || ch == '\n' || ch == KEY_ENTER) {
            // 本文はプレビューで既に g_ed に入っている。
            strncpy(s_current_file, fname, sizeof(s_current_file) - 1);
            s_current_file[sizeof(s_current_file) - 1] = '\0';

            g_fep.ClearMode();
            cand_bar_clear(&g_bar);

            // 読み込んだ直後は SD の中身と一致している。
            g_dirty      = false;
            s_config_mode = false;      // 設定を開いていたなら、ここで抜ける
            return;
        }

        if (ch == KEY_ESC) {
            memcpy(g_ed.buf, saved_buf, (size_t)saved_len * sizeof(g_ed.buf[0]));
            g_ed.len        = saved_len;
            g_ed.cursor     = saved_cursor;
            g_ed.goal_col   = -1;
            g_ed.scroll_top = 0;
            opur_update_scroll(&g_ed);
            return;
        }
    }
}

// 送信。**いま編集中のバッファ**をそのまま送る。ファイルは読まない。
// 画面に見えているものが送られる、という一対一の対応にしてある。
static void do_send(void) {
    static char u8[OPUR_BUF_MAX * 3 + 1];
    int bytes;
    int r;

    if (g_ed.len <= 0) {
        flash_status("本文が空です", NULL, FLASH_MS);
        return;
    }

    // 通信は数秒ブロックする。何も出さないと固まったように見える。
    clear();
    mvaddstr(ROW_STATUS1, 0, "送信中...");
    m5c_separator();
    refresh();

    r = opur_net_check();
    if (r == 0) { flash_status("WiFi/URL 未設定", NULL, FLASH_MS);            return; }
    if (r < 0)  { flash_status("通信エラー", "ログ(ESC-5)に理由", FLASH_MS); return; }

    // 相手がまだ引き取っていない。上書きしてよいかは本人にしか決められない。
    if (r == 2 && ask("前の文書が残っています", "1:上書き 2:やめる") != '1') return;

    bytes = utf16_to_utf8(g_ed.buf, g_ed.len, u8, sizeof(u8));

    clear();
    mvaddstr(ROW_STATUS1, 0, "送信中...");
    m5c_separator();
    refresh();

    if (opur_net_put(u8, bytes) == 1) {
        flash_status("sent!", NULL, FLASH_MS);
    } else {
        flash_status("send err", "ログ(ESC-5)に理由", FLASH_MS);
    }
}

// 設定（/config.txt）を本文に読み込む。メニューの隠し '0'。
//
// **PC が手元に無いところで WiFi を切り替えるための逃げ道。**
// opur_config.c はキーの先頭が '#' の行を読み飛ばすので、
//
//   WIFI_SSID=home
//   #WIFI_SSID=tether
//
// と両方書いておけば、'#' を 1 文字動かすだけで切り替わる。
// 同じキーが 2 回生きていたら**後に読んだほう**が勝つ。
//
// 値を打ち直さずに済むのが肝。FEP は 32 文字までしか持てないので、
// 83 文字の ENDPOINT_URL などは実機では入力し切れない。
static void do_config(void) {
    if (!confirm_discard("設定をやめます")) return;

    s_current_file[0] = '\0';
    s_config_mode     = true;

    if (load_from_sd(CONFIG_PATH) < 0) {
        // 無ければ空から作らせる。SD が無いときもここに来るが、
        // そのときは保存で「保存できません」に落ちるので実害は無い。
        opur_init(&g_ed);
        notice("config.txt がありません", "新しく作ります");
    }

    g_fep.ClearMode();
    cand_bar_clear(&g_bar);
    g_dirty = false;
}

// 保存して結果を知らせる。save_to_sd() の戻り値をそのまま返す。
// メニューの '1' と Fn+S の両方から呼ぶので、モードには触らない
// （メニューを閉じるかどうかは呼び出し側の都合）。
//
// 保存しても本文は残す。開いているファイルを編集し続けられないと
// 「上書き保存」に意味が無い。新規に戻したいときはメニューの '2'。
static int do_save(void) {
    int r;

    // 変更が無く、既に開いているファイルがあるなら書かない。
    // 中身が同じものを書き直しても得るものが無いし、何も変えていないのに
    // 「保存しました」と出るのは嘘に近い。
    //
    // s_current_file が空のとき（＝まだ 1 度も保存していない）は素通しする。
    // 起動直後は本文も空なので、下の save_to_sd() が 0 を返して
    // 「本文が空です」になる。
    if (!g_dirty && (s_config_mode || s_current_file[0] != '\0')) {
        notice("変更ありません", s_config_mode ? "config.txt" : s_current_file);
        if (s_config_mode) start_new();     // 開きっぱなしにしない
        return 1;
    }

    r = save_to_sd();

    switch (r) {
    case 1:
        g_dirty = false;
        if (s_config_mode) {
            // 設定は起動時にしか読まない。張り直しはせず、そう伝えるだけ。
            notice("config.txt を保存", "電源を入れ直すと有効");
            start_new();                    // 設定モードから抜ける
        } else {
            notice("保存しました", s_current_file);
        }
        break;
    case 0:
        // 空ファイルは作らない
        notice("本文が空です", "保存しませんでした");
        break;
    default:
        notice("保存できません",
               !m5c_sd_ready()  ? "SD が読めません"
               : s_config_mode  ? CONFIG_PATH
                                : OPUR_DIR);
        break;
    }
    return r;
}

// メニュー表示中のキー処理。
static void menu_key(int ch) {
    switch (ch) {
    case '1':
        // 失敗したときはメニューを開いたままにして、やり直せることを伝える
        if (do_save() == 1) g_mode = MODE_INPUT;
        break;

    case '2':
        // 読込と同じく、書きかけがあるなら聞く。
        // 中止ならメニューに留まる（やり直せることを伝えるため）。
        if (!confirm_discard("新規をやめます")) break;
        start_new();
        g_mode = MODE_INPUT;
        break;

    case '3':
        g_mode = MODE_INPUT;
        do_load();
        break;

    case '4':
        g_mode = MODE_INPUT;
        do_send();
        break;

    case '0':
        // 隠し。メニューには出していない（常用の導線ではないため）。
        g_mode = MODE_INPUT;
        do_config();
        break;

    case '5':
        // 隠し。メニューには出していない（開発用）。
        // 開いた直後は最新が見えていてほしいので末尾に寄せる。
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

    view_init();                 // 中で initscr() → SD.begin() まで済む
    crumb(CRUMB_VIEW_READY);

    opur_log_clear();

    // ディープスリープからタイマーで起きただけなら、誰も待っていない。
    // AUTOSAVE も読まず WiFi も繋がず、そのまま次の 2 分へ落とす。
    // これが無いと、2 分ごとに起きては 11 分（1 分 + 10 分）起きたままになり、
    // 寝かせた意味がほとんど無くなる。
    //
    // ページャー受信を入れるときは、この分岐が
    // 「GET する → 何も無ければ寝る」の置き場になる。
    //
    // **view_init() より後に置いてある。** opur_sleep_deep() は
    // M5.getBoard() を見てキーウェイクを張るかどうかを決めるので、
    // M5 の初期化前に呼ぶと「タイマーでしか起きない」状態で寝てしまう。
    // そうなるとキーを押しても戻ってこない機械になる。
    if (opur_sleep_wake_cause() == OPUR_WAKE_TIMER) {
        opur_log_add("タイマー復帰 電池%d%% → 再スリープ", m5c_battery_level());
        opur_sleep_deep(DEEP_TIMER_MS);   // 戻らない
    }

    // 前回が異常終了なら、どこまで進んでいたかを最初に出す。
    // 黙っているのは 1 電源投入 / 3 ソフトリセット / 8 ディープスリープ復帰。
    //
    // **8 を外さないと、寝て起きるたびに誤爆する。** 意図した復帰なのに
    // 「落ちた場所」が出てしまい、しかも reset_text() に枝が無かった頃は
    // 「前回 」と空欄になって、何が起きたのか分からないログになっていた。
    if (reset_reason != 1 && reset_reason != 3 && reset_reason != 8) {
        opur_log_add("前回 %s", reset_text(reset_reason));
        opur_log_add("落ちた場所 %u %s", prev_crumb, crumb_text(prev_crumb));
        opur_log_add("  a=%ld b=%ld", (long)prev_a, (long)prev_b);
    }
    opur_log_add("SD: %s",  m5c_sd_ready()  ? "OK" : "マウント失敗");

    // 電池。Cardputer は PMIC が無く GPIO10 の分圧を ADC1 で読んでいる。
    // ボード判定に失敗すると % が -2 になるので、ここで出るかどうかが
    // ステータス行の表示が出ない理由の切り分けになる。
    // mV も出すのは、% が M5Unified のざっくり換算
    //（3300mV=0% / 4100mV=100%）で、充電中は張り付いて見えるため。
    // 100% はスリープ抑制の判定でもある（on_wall_power() を参照）。
    opur_log_add("電池 %d%% %dmV",
                 m5c_battery_level(), m5c_battery_mv());

    // PSRAM。**この機体では常に 0K** で、それが正常。無印 / v1.1 / ADV は
    // どれも PSRAM を持たない ESP32-S3FN8（CLAUDE.md 参照。memory_type と
    // BOARD_HAS_PSRAM を足しても 0K のままだった）。
    //
    // 0K でも HTTPS は張れている。TLS に要る 45〜50KB は、描画 Canvas を
    // 1bpp にして内部 RAM を 28KB 空けることで作っている（接続後 77K）。
    // この行が意味を持つのは、PSRAM のある機体に載せ替えたときだけ。
    opur_log_add("PSRAM %uK",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    g_have_dict = (opur_dict_open(&g_dict, DICT_PATH) == 0);
    cand_bar_init(&g_bar, g_have_dict ? &g_dict : NULL, &g_conv);

    opur_log_add("辞書: %s", g_have_dict ? "OK" : "開けません");
    crumb(CRUMB_DICT_DONE);

    if (!g_have_dict) warn_no_dict();

    // WiFi は編集機能とは独立している。繋がらなくてもここから先は同じ。
    boot_wifi();
    crumb(CRUMB_WIFI_DONE);

    // 起動は空バッファから。021 で NVS 自動退避をやめて以来、
    // 電源を切れば書きかけは消える（ワープロと同じ）。
    g_dirty = false;

    // 例外はディープスリープ直前の AUTOSAVE だけ。あれば続きから始める。
    autosave_restore();

    s_last_key_ms = now_ms();

    timeout(IDLE_REDRAW_MS);
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

    // --- Fn+S: 保存のショートカット ---
    // ESC → 1 と同じ。ただし**変換の途中では効かせない**。
    //
    // 未確定のローマ字（FEP バッファ）や候補選択中の読みは本文に入っていない
    // ので、そのまま保存すると画面に見えているものと SD の中身がずれる。
    //
    // メニュー経由でも同じことは起きない。FEP バッファが空でないときの ESC は
    // FEP が食って変換を取り消すので、そもそもメニューに入れないため
    // （FEP_THRU の VK_ESC の注記を参照）。ここもそれに揃える。
    if (ch == KEY_SAVE) {
        if (g_mode == MODE_INPUT && g_fep.GetBuffLen() <= 0) do_save();
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

// loop タスクのスタックの残りを見張る。
//
// ↑↓ で OpurLayout（8.2KB）が 2 枚積まれて 16KB のスタックを溢れ、下に隣接する
// ヒープに居た NVS のオブジェクトを壊していた（2026-08-17）。壊れても即死せず、
// **あとで WiFi ドライバが NVS を読んだときに落ちる**ので、スタック側からは
// 何も見えない。直したあとも余裕がどれだけあるかを実機で見えるようにしておく。
//
// **減ったときだけ出す。** 毎周回出すとログの 32 行がこれで埋まる。
// uxTaskGetStackHighWaterMark() は ESP-IDF ではバイトを返す。
static void watch_stack(void) {
    static unsigned s_logged = 0xFFFFFFFFu;
    const unsigned left = (unsigned)uxTaskGetStackHighWaterMark(NULL);

    if (left + 256 > s_logged) return;   // 256B 以上減ったときだけ
    s_logged = left;
    opur_log_add("stack 残り%uB", left);
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

    // 無操作のまま IDLE_REDRAW_MS 過ぎた。次の周回の view_draw() で
    // 時計が描き直される。起動時に間に合わなかった WiFi もここで拾う。
    if (ch == ERR) {
        crumb(CRUMB_IDLE);
        wifi_catch_up();
        idle_check();          // 中で 25/26/27 を打つ
        return;
    }

    s_last_key_ms = now_ms();

    // 本文が実際に増減したときだけ dirty にする。
    // 「キーが来たら dirty」にすると、候補を眺めただけでも未保存扱いになり、
    // 読込のたびに「保存しますか?」が出る。
    //
    // 本文を変えるのは opur_insert() と opur_backspace() だけで、
    // どちらも必ず len が動く。カーソル移動は見なくてよい。
    //
    // ただしメニューから来たときは触らない。新規・読込・保存はどれも
    // len を大きく動かすが、それは「編集した」ではないし、
    // 各処理が自分で g_dirty を決めている（ここで上書きすると台無しになる）。
    {
        const int  before_len  = g_ed.len;
        const Mode before_mode = g_mode;

        crumb(CRUMB_KEY);
        handle_key(ch);
        watch_stack();      // いちばん深く潜った直後に測る

        if (before_mode != MODE_MENU && g_ed.len != before_len) g_dirty = true;
    }
}
