// main.c — curses 初期化・キーループ・終了
//
// キー入力を editor API に振り分けるだけ。描画は view_curses.c に任せる。

#include "editor.h"

#include <ncurses.h>

// view_curses.c が提供する（この規模なので view.h は起こさない）
void view_init(void);
void view_end(void);
int  view_fits(void);
void view_too_small(void);
void view_draw(const OpurEditor* ed);

#define KEY_CTRL_Q 17

// テスト用の仮マップ。FEP がない状態で全角のカーソル制御・折り返しを確認する。
#define TEST_CH_HIRA  0x3042  // あ
#define TEST_CH_KANJI 0x6F22  // 漢
#define TEST_CH_ZEN_A 0xFF21  // Ａ（全角 A）

int main(void) {
    OpurEditor ed;

    opur_init(&ed);
    view_init();

    for (;;) {
        int ch;

        if (view_fits()) {
            view_draw(&ed);
        } else {
            view_too_small();
        }

        ch = getch();

        if (ch == KEY_CTRL_Q) break;

        switch (ch) {
        case KEY_LEFT:   opur_left(&ed);      break;
        case KEY_RIGHT:  opur_right(&ed);     break;
        case KEY_UP:     opur_up(&ed);        break;
        case KEY_DOWN:   opur_down(&ed);      break;

        case '\n':
        case '\r':
        case KEY_ENTER:  opur_insert(&ed, OPUR_LF); break;

        case 8:
        case 127:
        case KEY_BACKSPACE:
        case KEY_DC:     opur_backspace(&ed); break;

        case KEY_F(1):   opur_insert(&ed, TEST_CH_HIRA);  break;
        case KEY_F(2):   opur_insert(&ed, TEST_CH_KANJI); break;
        case KEY_F(3):   opur_insert(&ed, TEST_CH_ZEN_A); break;

        case KEY_RESIZE: break;   // 次のループ先頭で描き直す

        default:
            if (ch >= 0x20 && ch <= 0x7E) {
                opur_insert(&ed, (uint16_t)ch);
            }
            break;
        }
    }

    view_end();
    return 0;
}
