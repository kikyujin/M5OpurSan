// hello_m5.cpp — Phase E-1a: getch() の疎通確認
//
// 実機の全キーが m5curses 経由で正しいコードになって出てくるかを目で見るための
// テストプログラム。Phase E-0 のフォント表示確認は済んでいるので、
// ここは入力に絞る（フォント表示は git 履歴の feaddb5 を参照）。
//
// 操作:
//   印字可能文字   そのままエコー行に積まれる
//   BS             エコー行から 1 文字消す
//   その他         直前キー欄に名前が出る
//   ESC 3 回連打   終了（テスト用の仮仕様）

#include "m5curses.h"

#include <stdio.h>
#include <string.h>

#define ECHO_MAX (M5C_COLS * 2)   // 2 行ぶん

static char g_echo[ECHO_MAX + 1];
static int  g_echo_len = 0;

static char g_last[16] = "-";
static int  g_esc_run  = 0;       // ESC の連打回数

// 特殊キーの表示名。印字可能文字なら NULL を返す。
static const char *key_name(int ch) {
    switch (ch) {
    case KEY_LEFT:      return "[LEFT]";
    case KEY_RIGHT:     return "[RIGHT]";
    case KEY_UP:        return "[UP]";
    case KEY_DOWN:      return "[DOWN]";
    case KEY_BACKSPACE: return "[BS]";
    case KEY_DC:        return "[DEL]";
    case KEY_TAB:       return "[TAB]";
    case KEY_ESC:       return "[ESC]";
    case '\r':          return "[ENTER]";
    case ' ':           return "[SPACE]";
    default:            return NULL;
    }
}

static void echo_push(char c) {
    if (g_echo_len >= ECHO_MAX) {
        // 溢れたら先頭を 1 文字捨てて送る
        memmove(g_echo, g_echo + 1, ECHO_MAX - 1);
        g_echo_len = ECHO_MAX - 1;
    }
    g_echo[g_echo_len++] = c;
    g_echo[g_echo_len]   = '\0';
}

static void echo_pop(void) {
    if (g_echo_len > 0) g_echo[--g_echo_len] = '\0';
}

static void draw(void) {
    char row[M5C_COLS + 1];
    char line[64];

    clear();

    attron(A_REVERSE);
    snprintf(row, sizeof(row), "%-*s", M5C_COLS, " getch test  ESC x3 = end");
    mvaddstr(0, 0, row);
    attroff(A_REVERSE);

    // エコー行（30 桁で折り返して 2 行）
    for (int r = 0; r < 2; r++) {
        const int from = r * M5C_COLS;
        if (from >= g_echo_len) break;
        int n = g_echo_len - from;
        if (n > M5C_COLS) n = M5C_COLS;
        memcpy(row, g_echo + from, n);
        row[n] = '\0';
        mvaddstr(2 + r, 0, row);
    }

    snprintf(line, sizeof(line), "last: %s", g_last);
    mvaddstr(5, 0, line);

    snprintf(line, sizeof(line), "esc : %d/3", g_esc_run);
    mvaddstr(6, 0, line);

    refresh();
}

void setup() {
    initscr();
    g_echo[0] = '\0';
    draw();
}

void loop() {
    const int ch = getch();
    if (ch == ERR) return;

    const char *name = key_name(ch);

    if (ch == KEY_ESC) {
        g_esc_run++;
    } else {
        g_esc_run = 0;
    }

    if (name) {
        snprintf(g_last, sizeof(g_last), "%s", name);
        if (ch == KEY_BACKSPACE) echo_pop();
        if (ch == ' ')           echo_push(' ');
    } else if (ch >= 0x21 && ch <= 0x7E) {
        snprintf(g_last, sizeof(g_last), "'%c' 0x%02X", (char)ch, ch);
        echo_push((char)ch);
    } else {
        // 想定外のコードが来ていないかも見たいので生の値を出す
        snprintf(g_last, sizeof(g_last), "0x%X", ch);
    }

    if (g_esc_run >= 3) {
        clear();
        mvaddstr(0, 0, "getch test 終了");
        mvaddstr(2, 0, "リセットで再開します");
        refresh();
        for (;;) { }   // ここで止める
    }

    draw();
}
