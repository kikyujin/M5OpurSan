// view_curses.c — curses 描画
//
// 将来 view_m5.c と差し替え可能な境界。editor.c 側は curses を一切知らない。
// ここから editor の内部状態を書き換えないこと（読むだけ）。

// ncurses の wide-char API（mvaddwstr / cchar_t）を使うには、ncurses.h より前に
// この定義が必要。CFLAGS ではなくここで定義しておく（editor.c は素の C11 のまま）。
#ifndef _XOPEN_SOURCE_EXTENDED
#define _XOPEN_SOURCE_EXTENDED 1
#endif

#include "editor.h"

#include <locale.h>
#include <ncurses.h>
#include <stdio.h>
#include <wchar.h>

// 本文領域は論理 30 半角幅だが、画面上は 1 桁多く取る。
// 行末（col == 30）にカーソルが来るケースがあり、その 1 桁を描く場所が要るため。
#define VIEW_COLS (OPUR_COLS + 1)

#define BOX_Y 2   // 枠の上辺
#define BOX_X 2   // 枠の左辺

#define NEED_H (BOX_Y + OPUR_ROWS + 4)   // 枠上 + 本文7 + ステータス1 + 枠下 + 余白
#define NEED_W (BOX_X + VIEW_COLS + 3)

// ---------------------------------------------------------------------------

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

// 端末が枠を描くには小さすぎるとき 0 を返す。
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
    // 枠内の行構成: 本文 7 行 / 仕切り 1 行 / ステータス 1 行
    int bottom = BOX_Y + OPUR_ROWS + 3;

    mvaddstr(0, 0, "OpurSan editor proto   "
                   "F1:a  F2:kanji  F3:zenA   Ctrl+Q:quit");

    mvhline(BOX_Y, BOX_X + 1, ACS_HLINE, VIEW_COLS);
    mvhline(bottom, BOX_X + 1, ACS_HLINE, VIEW_COLS);
    mvvline(BOX_Y + 1, BOX_X, ACS_VLINE, OPUR_ROWS + 2);
    mvvline(BOX_Y + 1, BOX_X + VIEW_COLS + 1, ACS_VLINE, OPUR_ROWS + 2);

    mvaddch(BOX_Y, BOX_X, ACS_ULCORNER);
    mvaddch(BOX_Y, BOX_X + VIEW_COLS + 1, ACS_URCORNER);
    mvaddch(bottom, BOX_X, ACS_LLCORNER);
    mvaddch(bottom, BOX_X + VIEW_COLS + 1, ACS_LRCORNER);

    // 本文とステータスバーの仕切り
    mvhline(BOX_Y + OPUR_ROWS + 1, BOX_X + 1, ACS_HLINE, VIEW_COLS);
}

void view_draw(const OpurEditor* ed) {
    OpurLayout lay;
    wchar_t wcs[OPUR_COLS + 2];
    char status[64];
    int cur_line, cur_col;
    int r;

    opur_layout(ed, &lay);
    cur_line = opur_cursor_line(&lay, ed->cursor);
    cur_col  = opur_cursor_col(ed, &lay, cur_line, ed->cursor);

    erase();
    draw_frame();

    // --- 本文 7 行 ---
    for (r = 0; r < OPUR_ROWS; r++) {
        int li = ed->scroll_top + r;
        const OpurLine* ln;
        int content_end, n;

        if (li >= lay.count) break;

        ln = &lay.line[li];
        content_end = ln->has_lf ? (ln->end - 1) : ln->end;
        n = utf16_to_wcs(&ed->buf[ln->start], content_end - ln->start,
                         wcs, OPUR_COLS + 2);
        if (n > 0) {
            mvaddwstr(BOX_Y + 1 + r, BOX_X + 1, wcs);
        }
    }

    // --- ステータスバー ---
    snprintf(status, sizeof(status), "L:%d C:%d  %d/%d",
             cur_line + 1, cur_col + 1, ed->len, OPUR_BUF_MAX);
    mvaddstr(BOX_Y + OPUR_ROWS + 2, BOX_X + 1, status);

    // --- カーソル ---
    {
        int r_cur = cur_line - ed->scroll_top;
        if (r_cur < 0)          r_cur = 0;
        if (r_cur >= OPUR_ROWS) r_cur = OPUR_ROWS - 1;
        move(BOX_Y + 1 + r_cur, BOX_X + 1 + cur_col);
    }

    refresh();
}
