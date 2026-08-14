// main.cpp — M5Cardputer 実機版の統合（Phase E-1）
//
// opur_editor/main.cpp（PC 版）と同じ構造。違いは 3 点だけ:
//   1. view_curses.c ではなく view_m5.c がリンクされる
//   2. 辞書は SD カードから開く（DICT_PATH）
//   3. Ctrl+Q が無い。実機ではキーボードから制御コードが取れないため
//      （007 調査）。終了の代わりは Phase E-2 の ESC メニュー。
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

#include <stdio.h>

#ifdef M5OPUR
  #define DICT_PATH M5C_SD_MOUNT "/dict/system.dic"
#else
  #define DICT_PATH "../dict_tools/output/system.dic"
#endif

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

enum Mode { MODE_INPUT, MODE_SELECT };

static Mode g_mode      = MODE_INPUT;
static bool g_have_dict = false;

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

void setup() {
    opur_init(&g_ed);

    view_init();                 // 中で initscr() → SD.begin() まで済む

    g_have_dict = (opur_dict_open(&g_dict, DICT_PATH) == 0);
    cand_bar_init(&g_bar, g_have_dict ? &g_dict : NULL, &g_conv);

    if (!g_have_dict) warn_no_dict();
}

void loop() {
    static UTF16 pending[FEP_MAXBUFF];
    int pending_len = fep_pending(pending, FEP_MAXBUFF);

    view_draw(&g_ed, pending, pending_len,
              (g_mode == MODE_SELECT) ? &g_bar : NULL);

    int ch = getch();
    if (ch == ERR) return;

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
            default:                                       break;
            }
            break;

        case FEP_ERROR:
        default:
            break;
        }
    }
}
