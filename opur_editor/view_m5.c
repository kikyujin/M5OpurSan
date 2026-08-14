// view_m5.c — M5Cardputer 実機の描画層
//
// view_curses.c と同じ view.h を実装する。main.cpp から見ると両者は差し替え可能。
// ここから editor / candidate_bar の内部状態を書き換えないこと（読むだけ）。
//
// 描画は m5curses の API だけで行い、M5GFX は直接呼ばない。
// エディタ・候補バーが curses / M5GFX のどちらも知らずに済む構造を保つため。
//
// レイアウト（240x135、行高 16px）:
//   行 0-5   本文 6 行（OPUR_ROWS。PC 版と同じ）
//   ---      1px の仕切り線（m5curses が行の隙間として持っている）
//   行 6     FEP 入力行（未確定なので下線）
//   行 7     候補バー（選択中の候補だけ反転。PC 版と同じ）
//
// PC 版にあった枠線とステータスバーは無い。実機は 8 行しか無いので、
// 枠を描くと本文が入らない。本文との区切りは仕切り線が受け持つ。

#include "view.h"

#include "m5curses.h"
#include "utf8_utf16.h"

// 本文 1 行ぶんの UTF-8 バッファ。30 半角 = 最大 30 文字、全角は 3 バイトなので 90。
#define LINE_U8_MAX 128

// ESP32 の Arduino loop タスクのスタックは既定 8KB しかない。
// OpurLayout は約 8KB、候補バーの作業用も合わせて 2KB 近くあるので、
// 大きいものはすべて static に置く（描画は単一スレッドなので再入は無い）。
static OpurLayout  g_lay;
static CandRender  g_render;
static char        g_seg[3][CAND_RENDER_MAX];

// 行の割り当て
#define ROW_TEXT0 0
#define ROW_FEP   (ROW_TEXT0 + OPUR_ROWS)
#define ROW_CAND  (ROW_FEP + 1)

// ---------------------------------------------------------------------------

void view_init(void) {
    initscr();
}

void view_end(void) {
    endwin();
}

// 実機は画面サイズが固定なので、入らないことは無い。
int  view_fits(void)      { return 1; }
void view_too_small(void) { }

// ---------------------------------------------------------------------------
// 本文
// ---------------------------------------------------------------------------

static void draw_text(const OpurEditor* ed, const OpurLayout* lay) {
    char u8[LINE_U8_MAX];
    int r;

    for (r = 0; r < OPUR_ROWS; r++) {
        int li = ed->scroll_top + r;
        const OpurLine* ln;
        int content_end;

        if (li >= lay->count) break;

        ln = &lay->line[li];
        content_end = ln->has_lf ? (ln->end - 1) : ln->end;
        if (content_end <= ln->start) continue;

        utf16_to_utf8(&ed->buf[ln->start], content_end - ln->start, u8, sizeof(u8));
        mvaddstr(ROW_TEXT0 + r, 0, u8);
    }
}

// カーソル。実機にはハードウェアカーソルが無いので、
// カーソル位置の 1 文字を反転して代わりにする（ブロックカーソル）。
static void draw_cursor(const OpurEditor* ed, const OpurLayout* lay,
                        int cur_line, int cur_col) {
    char cell[8] = " ";
    int r = cur_line - ed->scroll_top;

    if (r < 0 || r >= OPUR_ROWS) return;
    if (cur_col < 0 || cur_col >= OPUR_COLS) return;   // 行末 30 桁目は描く場所が無い

    if (cur_line < lay->count) {
        const OpurLine* ln = &lay->line[cur_line];
        int content_end = ln->has_lf ? (ln->end - 1) : ln->end;
        if (ed->cursor < content_end) {
            utf16_to_utf8(&ed->buf[ed->cursor], 1, cell, sizeof(cell));
        }
    }

    attron(A_REVERSE);
    mvaddstr(ROW_TEXT0 + r, cur_col, cell);
    attroff(A_REVERSE);
}

// ---------------------------------------------------------------------------
// FEP 入力行
// ---------------------------------------------------------------------------

// 未確定バッファ（かな + ローマ字途中の混在）をそのまま出す。
// 幅を超えるときは末尾側を残す（打っている先が見えるほうが自然）。
static void draw_fep(const uint16_t* buf, int len) {
    char u8[LINE_U8_MAX];
    int start = 0;
    int cols = 0;
    int i;

    if (len <= 0) return;

    for (i = len - 1; i >= 0; i--) {
        int w = opur_char_width(buf[i]);
        if (cols + w > OPUR_COLS) break;
        cols += w;
        start = i;
    }

    utf16_to_utf8(&buf[start], len - start, u8, sizeof(u8));

    // 未確定であることが分かるように下線を引く
    attron(A_UNDERLINE);
    mvaddstr(ROW_FEP, 0, u8);
    attroff(A_UNDERLINE);
}

// ---------------------------------------------------------------------------
// 候補バー
// ---------------------------------------------------------------------------

// candidate_bar.c の cp_width と同じ規則。
static int cp_cols(uint32_t cp) {
    if (cp >= 0x20u && cp <= 0x7Eu) return 1;
    if (cp == 0x25C0u || cp == 0x25B6u) return 1;   // ◀ ▶
    return 2;
}

// 選択中の候補だけ反転する（PC 版の draw_cand と同じ）。
// バー全体を反転させると本文より目立ちすぎるので、通常表示にしてある。
static void draw_cand(const CandBar* bar) {
    CandRender* const r = &g_render;
    int  seg_n[3] = { 0, 0, 0 };
    int  seg_col[3];
    const char* p;
    int col = 0;
    int i;

    if (!bar) return;

    cand_bar_render(bar, r);

    seg_col[0] = 0;
    seg_col[1] = r->sel_col;
    seg_col[2] = r->sel_col + r->sel_width;

    for (p = r->text; *p; ) {
        uint32_t cp;
        int used = u8_decode(p, &cp);
        int which;
        int n;

        if (used == 0) break;

        if (col < r->sel_col)                     which = 0;
        else if (col < r->sel_col + r->sel_width) which = 1;
        else                                      which = 2;

        // ◀ ▶ は JIS X 0208 に無く efont では豆腐になる。
        // 幅はどちらも半角 1 桁ぶんなので、ASCII に置き換えても桁勘定は狂わない。
        if (cp == 0x25C0u || cp == 0x25B6u) {
            if (seg_n[which] < CAND_RENDER_MAX - 1) {
                g_seg[which][seg_n[which]++] = (cp == 0x25C0u) ? '<' : '>';
            }
        } else {
            for (n = 0; n < used; n++) {
                if (seg_n[which] < CAND_RENDER_MAX - 1) {
                    g_seg[which][seg_n[which]++] = p[n];
                }
            }
        }

        p   += used;
        col += cp_cols(cp);
    }

    g_seg[0][seg_n[0]] = '\0';
    g_seg[1][seg_n[1]] = '\0';
    g_seg[2][seg_n[2]] = '\0';

    for (i = 0; i < 3; i++) {
        if (seg_n[i] == 0) continue;
        if (seg_col[i] >= M5C_COLS) continue;
        if (i == 1) {
            attron(A_REVERSE);                               // 選択中だけ反転
            mvaddstr(ROW_CAND, seg_col[i], g_seg[i]);
            attroff(A_REVERSE);
        } else {
            mvaddstr(ROW_CAND, seg_col[i], g_seg[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// メニュー
// ---------------------------------------------------------------------------

// 下 2 行を上書きする。本文（行 0-5）はそのまま残す。
static void draw_menu(void) {
    mvaddstr(ROW_FEP,  0, "[1.保存  2.新規]");
    mvaddstr(ROW_CAND, 0, "ESC:戻る");
}

// ---------------------------------------------------------------------------

void view_draw(const OpurEditor* ed,
               const uint16_t* fep_buf, int fep_len,
               const CandBar* bar,
               int show_menu) {
    OpurLayout* const lay = &g_lay;
    int cur_line, cur_col;

    opur_layout(ed, lay);
    cur_line = opur_cursor_line(lay, ed->cursor);
    cur_col  = opur_cursor_col(ed, lay, cur_line, ed->cursor);

    clear();

    draw_text(ed, lay);
    draw_cursor(ed, lay, cur_line, cur_col);

    if (show_menu) {
        draw_menu();
    } else {
        draw_fep(fep_buf, fep_len);
        draw_cand(bar);
    }

    m5c_separator();

    refresh();
}
