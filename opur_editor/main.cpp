// main.cpp — FEP + エディタ + 候補バーの統合（PC curses デモ）
//
// C++ なのは fep/ が C++17 だから。それ以外（editor / candidate_bar / conv_utf8 /
// utf8_utf16 / opur_dict）はすべて C11 で、ヘッダが extern "C" で囲ってあるので
// そのまま呼べる。M5 実機ビルドも Arduino framework で C++ なので、この構成が
// そのまま持っていける。
//
// 状態は 2 つだけ:
//   MODE_INPUT   ローマ字 → かな を CFep のバッファに溜める
//   MODE_SELECT  candidate_bar が候補を持っている
//
// CFep::SelectMode は通さない。スペースは main が横取りして candidate_bar に渡す
// （CFep の候補機能はひらがな/カタカナ/全角英/半角英の 4 つだけで、
//   candidate_bar の候補 0 と N+1〜N+3 に相当する。辞書を持つぶん後者が上位互換）。

#include "candidate_bar.h"
#include "conv_utf8.h"
#include "editor.h"
#include "utf8_utf16.h"
#include "view.h"

#include "fep.h"

#include <ncurses.h>
#include <stdio.h>

#define KEY_CTRL_Q 17
#define DICT_PATH "../dict_tools/output/system.dic"

// CandBar は約 84KB あるのでスタックには置けない。
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

// ---------------------------------------------------------------------------
// キーマッピング
// ---------------------------------------------------------------------------

// curses のキーコード → VKEY。
// 印字可能文字は ASCII をそのまま VKEY として使う流儀（fep.h 参照）。
static VKEY curses_to_vkey(int ch) {
    switch (ch) {
    case KEY_LEFT:      return VK_LEFT;
    case KEY_RIGHT:     return VK_RIGHT;
    case KEY_UP:        return VK_UP;
    case KEY_DOWN:      return VK_DOWN;

    case '\n':
    case '\r':
    case KEY_ENTER:     return VK_ENTER;

    case 27:            return VK_ESC;

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
    uint16_t u16[CAND_TEXT_MAX];
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

int main(void) {
    Mode mode = MODE_INPUT;
    int have_dict;

    opur_init(&g_ed);

    have_dict = (opur_dict_open(&g_dict, DICT_PATH) == 0);
    cand_bar_init(&g_bar, have_dict ? &g_dict : NULL, &g_conv);

    view_init();

    for (;;) {
        UTF16 pending[FEP_MAXBUFF];
        int pending_len = fep_pending(pending, FEP_MAXBUFF);
        int ch;
        VKEY key;

        if (view_fits()) {
            // PC 版にメニューは無い（終了は Ctrl+Q）。実機のみ 0 以外になる。
            view_draw(&g_ed, pending, pending_len,
                      (mode == MODE_SELECT) ? &g_bar : NULL, 0);
        } else {
            view_too_small();
        }

        ch = getch();
        if (ch == KEY_CTRL_Q) break;
        if (ch == KEY_RESIZE) continue;   // 次のループ先頭で描き直す

        key = curses_to_vkey(ch);
        if (key == VK_INVALID) continue;

        // --- 候補選択中 ---
        if (mode == MODE_SELECT) {
            int ckey = vkey_to_candkey(key);
            if (ckey == 0) continue;

            switch (cand_bar_key(&g_bar, ckey)) {
            case CAND_COMMITTED:
                insert_utf8(cand_bar_committed(&g_bar));
                g_fep.ClearMode();
                mode = MODE_INPUT;
                break;
            case CAND_CANCELLED:
                // 読みは FEP バッファに残っているので入力の続きから再開できる
                mode = MODE_INPUT;
                break;
            default:
                break;
            }
            continue;
        }

        // --- 入力中: スペースは横取りして変換を開始する ---
        if (key == VK_HENKAN && g_fep.GetBuffLen() > 0) {
            UTF16 buf[FEP_MAXBUFF];
            char reading[CAND_READING_MAX];
            int n = fep_pending(buf, FEP_MAXBUFF);

            utf16_to_utf8(buf, n, reading, sizeof(reading));
            if (cand_bar_start(&g_bar, reading) > 0) mode = MODE_SELECT;
            continue;
        }

        // --- 入力中: FEP に渡す ---
        {
            UTF16 edit[FEP_MAXBUFF];
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

    view_end();
    if (have_dict) opur_dict_close(&g_dict);

    if (!have_dict) {
        fprintf(stderr, "辞書を開けなかった: %s\n", DICT_PATH);
        fprintf(stderr, "  dict_tools/ で make build-dict を実行して\n");
        return 1;
    }
    return 0;
}
