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
#include "utf8_utf16.h"
#include "view.h"

#include "fep.h"

#include <dirent.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef M5OPUR
  #define DICT_PATH M5C_SD_MOUNT "/dict/system.dic"
#else
  #define DICT_PATH "../dict_tools/output/system.dic"
#endif

#define OPUR_DIR      M5C_SD_MOUNT "/opur"
#define OPUR_SENT_DIR M5C_SD_MOUNT "/opur/sent"

// 無操作がこれだけ続いたら NVS に退避する
#define IDLE_SAVE_MS 3000

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

static const CandConv g_conv = {
    conv_utf8_to_katakana,
    conv_utf8_to_fullwidth,
    conv_utf8_to_halfwidth,
};

enum Mode { MODE_INPUT, MODE_SELECT, MODE_MENU };

static Mode g_mode      = MODE_INPUT;
static bool g_have_dict = false;
static bool g_dirty     = false;   // 前回の退避から本文が変わったか

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
static void nvs_save(void) {
    nvs_handle_t h;

    if (!m5c_nvs_ready()) return;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_blob(h, NVS_K_BUF, g_ed.buf,
                 (size_t)g_ed.len * sizeof(g_ed.buf[0]));
    nvs_set_u16(h, NVS_K_LEN, (uint16_t)g_ed.len);
    nvs_set_u16(h, NVS_K_CUR, (uint16_t)g_ed.cursor);
    nvs_commit(h);
    nvs_close(h);
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
    mkdir(OPUR_DIR, 0777);

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

// ---------------------------------------------------------------------------
// ESC メニュー
// ---------------------------------------------------------------------------

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

    case KEY_ESC:
        g_mode = MODE_INPUT;
        break;

    default:
        break;                               // それ以外は無視
    }
}

// ---------------------------------------------------------------------------

void setup() {
    opur_init(&g_ed);

    view_init();                 // 中で initscr() → SD.begin() → NVS 初期化まで済む

    g_have_dict = (opur_dict_open(&g_dict, DICT_PATH) == 0);
    cand_bar_init(&g_bar, g_have_dict ? &g_dict : NULL, &g_conv);

    if (!g_have_dict) warn_no_dict();

    // 電源を切る前の書きかけを戻す。復元直後は退避済みなので dirty にしない。
    nvs_restore();
    g_dirty = false;

    timeout(IDLE_SAVE_MS);
}

static void handle_key(int ch) {
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

    view_draw(&g_ed, pending, pending_len,
              (g_mode == MODE_SELECT) ? &g_bar : NULL,
              (g_mode == MODE_MENU) ? 1 : 0);

    int ch = getch();

    // 無操作が続いた。変わっていれば退避する（数 ms なので気づかれない）
    if (ch == ERR) {
        if (g_dirty) {
            nvs_save();
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

        handle_key(ch);

        if (g_ed.len != before_len || g_ed.cursor != before_cur) g_dirty = true;
    }
}
