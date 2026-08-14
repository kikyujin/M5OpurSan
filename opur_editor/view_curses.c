// view_curses.c — curses 描画
//
// 将来 view_m5.c と差し替え可能な境界。editor.c 側は curses を一切知らない。
// ここから editor / candidate_bar の内部状態を書き換えないこと（読むだけ）。
//
// 枠内 8 行の構成（実機と 1:1）:
//   本文 OPUR_ROWS(6) 行 / FEP 入力行 1 行 / 候補バー 1 行
// ステータスバーは枠外（下）に出す。

// ncurses の wide-char API（mvaddwstr / cchar_t）を使うには、ncurses.h より前に
// この定義が必要。CFLAGS ではなくここで定義しておく（editor.c は素の C11 のまま）。
#ifndef _XOPEN_SOURCE_EXTENDED
#define _XOPEN_SOURCE_EXTENDED 1
#endif

#include "view.h"

#include "utf8_utf16.h"

#include <locale.h>
#include <ncurses.h>
#include <stdio.h>
#include <wchar.h>

// 本文領域は論理 30 半角幅だが、画面上は 1 桁多く取る。
// 行末（col == 30）にカーソルが来るケースがあり、その 1 桁を描く場所が要るため。
#define VIEW_COLS (OPUR_COLS + 1)

#define BOX_Y 2   // 枠の上辺
#define BOX_X 2   // 枠の左辺

// 行の割り当て
#define ROW_TEXT0  (BOX_Y + 1)                  // 本文の先頭行
#define ROW_SEP    (ROW_TEXT0 + OPUR_ROWS)      // 本文と FEP の仕切り
#define ROW_FEP    (ROW_SEP + 1)                // FEP 入力行
#define ROW_CAND   (ROW_FEP + 1)                // 候補バー
#define ROW_BOTTOM (ROW_CAND + 1)               // 枠の下辺
#define ROW_STATUS (ROW_BOTTOM + 1)             // ステータス（枠外）

#define NEED_H (ROW_STATUS + 1)
#define NEED_W (BOX_X + VIEW_COLS + 3)

#define WBUF_MAX 256

// ---------------------------------------------------------------------------
// 幅の計算
// ---------------------------------------------------------------------------

// candidate_bar.c の cp_width と同じ規則にそろえる。
// ◀ / ▶ は仕様上「半角 1 文字ぶん」として数える（M5 の efont では 1 桁）。
// ターミナルはこれを 2 桁で描くので枠の右端が 1〜2 桁ぶんずれることがあるが、
// 実機の見え方を基準にしたいのでこちらに合わせる。
static int cp_cols(uint32_t cp) {
    if (cp >= 0x20u && cp <= 0x7Eu) return 1;
    if (cp == 0x25C0u || cp == 0x25B6u) return 1;   // ◀ ▶
    return 2;
}

// UTF-16（BMP のみ）→ wchar_t 列。curses の wide API 用。
static int utf16_to_wcs(const uint16_t* src, int len, wchar_t* out, int maxout) {
    int n = 0;
    int i;
    for (i = 0; i < len && n < maxout - 1; i++) {
        out[n++] = (wchar_t)src[i];
    }
    out[n] = L'\0';
    return n;
}

// ---------------------------------------------------------------------------

void view_init(void) {
    setlocale(LC_ALL, "");
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);
    set_escdelay(25);
}

void view_end(void) {
    endwin();
}

int view_fits(void) {
    int h, w;
    getmaxyx(stdscr, h, w);
    return (h >= NEED_H && w >= NEED_W);
}

void view_too_small(void) {
    char msg[128];
    erase();
    snprintf(msg, sizeof(msg),
             "terminal too small: need %dx%d  (Ctrl+Q to quit)", NEED_W, NEED_H);
    mvaddstr(0, 0, msg);
    refresh();
}

// ---------------------------------------------------------------------------

static void draw_frame(void) {
    // NEED_W（36 桁）に収まる長さにしておく
    mvaddstr(0, 0, "OpurSan  FEP integration demo");
    mvaddstr(1, 0, "Space:conv Enter:ok Esc:x ^Q:quit");

    mvhline(BOX_Y,      BOX_X + 1, ACS_HLINE, VIEW_COLS);
    mvhline(ROW_SEP,    BOX_X + 1, ACS_HLINE, VIEW_COLS);
    mvhline(ROW_BOTTOM, BOX_X + 1, ACS_HLINE, VIEW_COLS);

    mvvline(BOX_Y + 1, BOX_X,             ACS_VLINE, ROW_BOTTOM - BOX_Y - 1);
    mvvline(BOX_Y + 1, BOX_X + VIEW_COLS + 1, ACS_VLINE, ROW_BOTTOM - BOX_Y - 1);

    mvaddch(BOX_Y,      BOX_X,                 ACS_ULCORNER);
    mvaddch(BOX_Y,      BOX_X + VIEW_COLS + 1, ACS_URCORNER);
    mvaddch(ROW_BOTTOM, BOX_X,                 ACS_LLCORNER);
    mvaddch(ROW_BOTTOM, BOX_X + VIEW_COLS + 1, ACS_LRCORNER);

    // 本文と FEP の仕切りは縦線とつなぐ
    mvaddch(ROW_SEP, BOX_X,                 ACS_LTEE);
    mvaddch(ROW_SEP, BOX_X + VIEW_COLS + 1, ACS_RTEE);
}

// --- 本文 -------------------------------------------------------------------

static void draw_text(const OpurEditor* ed, const OpurLayout* lay) {
    wchar_t wcs[OPUR_COLS + 2];
    int r;

    for (r = 0; r < OPUR_ROWS; r++) {
        int li = ed->scroll_top + r;
        const OpurLine* ln;
        int content_end, n;

        if (li >= lay->count) break;

        ln = &lay->line[li];
        content_end = ln->has_lf ? (ln->end - 1) : ln->end;
        n = utf16_to_wcs(&ed->buf[ln->start], content_end - ln->start,
                         wcs, OPUR_COLS + 2);
        if (n > 0) {
            mvaddwstr(ROW_TEXT0 + r, BOX_X + 1, wcs);
        }
    }
}

// --- FEP 入力行 --------------------------------------------------------------

// 未確定バッファ（かな + ローマ字途中の混在）をそのまま出す。
// 幅を超えるときは末尾側を残す（打っている先が見えるほうが自然）。
static void draw_fep(const uint16_t* buf, int len) {
    wchar_t wcs[WBUF_MAX];
    int start = 0;
    int cols = 0;
    int i, n;

    if (len <= 0) return;

    for (i = len - 1; i >= 0; i--) {
        int w = cp_cols(buf[i]);
        if (cols + w > OPUR_COLS) break;
        cols += w;
        start = i;
    }

    n = utf16_to_wcs(&buf[start], len - start, wcs, WBUF_MAX);
    if (n <= 0) return;

    // 未確定であることが分かるように下線を引く
    attron(A_UNDERLINE);
    mvaddwstr(ROW_FEP, BOX_X + 1, wcs);
    attroff(A_UNDERLINE);
}

// --- 候補バー ----------------------------------------------------------------

// レンダリング結果を「選択前 / 選択中 / 選択後」の 3 区間に割り、
// 選択中だけ反転して描く。桁送りは curses に任せる（全角の実幅は端末が決める）。
static void draw_cand(const CandBar* bar) {
    CandRender r;
    wchar_t seg[3][WBUF_MAX];
    int seg_n[3] = { 0, 0, 0 };
    const char* p;
    int col = 0;

    cand_bar_render(bar, &r);
    if (r.text[0] == '\0') return;

    for (p = r.text; *p; ) {
        uint32_t cp;
        int used = u8_decode(p, &cp);
        int which;
        if (used == 0) break;
        p += used;

        if (col < r.sel_col)                    which = 0;
        else if (col < r.sel_col + r.sel_width) which = 1;
        else                                    which = 2;

        if (seg_n[which] < WBUF_MAX - 1) {
            seg[which][seg_n[which]++] = (wchar_t)cp;
        }
        col += cp_cols(cp);
    }

    seg[0][seg_n[0]] = L'\0';
    seg[1][seg_n[1]] = L'\0';
    seg[2][seg_n[2]] = L'\0';

    move(ROW_CAND, BOX_X + 1);
    if (seg_n[0]) addwstr(seg[0]);
    if (seg_n[1]) {
        attron(A_REVERSE);
        addwstr(seg[1]);
        attroff(A_REVERSE);
    }
    if (seg_n[2]) addwstr(seg[2]);
}

// ---------------------------------------------------------------------------

void view_draw(const OpurEditor* ed,
               const uint16_t* fep_buf, int fep_len,
               const CandBar* bar) {
    OpurLayout lay;
    char status[80];
    int cur_line, cur_col;

    opur_layout(ed, &lay);
    cur_line = opur_cursor_line(&lay, ed->cursor);
    cur_col  = opur_cursor_col(ed, &lay, cur_line, ed->cursor);

    erase();

    // 中身を先に描き、あとから枠を重ねる。
    // ◀▶ の実幅が端末では 2 桁になるので、はみ出したぶんは枠の縦線で潰れる。
    draw_text(ed, &lay);
    draw_fep(fep_buf, fep_len);
    if (bar) draw_cand(bar);

    draw_frame();

    // --- ステータス（枠外）---
    if (bar) {
        snprintf(status, sizeof(status), "L:%d C:%d  %d/%d   [convert %d/%d]",
                 cur_line + 1, cur_col + 1, ed->len, OPUR_BUF_MAX,
                 cand_bar_selected(bar) + 1, cand_bar_count(bar));
    } else {
        snprintf(status, sizeof(status), "L:%d C:%d  %d/%d",
                 cur_line + 1, cur_col + 1, ed->len, OPUR_BUF_MAX);
    }
    mvaddstr(ROW_STATUS, BOX_X, status);

    // --- カーソルは常に本文側に置く（FEP 入力中も見えること）---
    {
        int r_cur = cur_line - ed->scroll_top;
        if (r_cur < 0)          r_cur = 0;
        if (r_cur >= OPUR_ROWS) r_cur = OPUR_ROWS - 1;
        move(ROW_TEXT0 + r_cur, BOX_X + 1 + cur_col);
    }

    refresh();
}
