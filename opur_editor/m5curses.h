// m5curses.h — curses 互換の薄層（M5Cardputer / M5Cardputer ADV 用）
//
// view_curses.c が PC で ncurses を叩くのと同じ形で、実機側では
// この層を叩く。将来の view_m5.c は C11 のままでいられるように
// extern "C" で囲ってある（実装 m5curses.cpp は C++）。
//
// 画面は 240x135 / efontJA_16 → 半角 8x16px、全角 16x16px。
// したがって 30 半角 x 8 行。座標は文字セル単位で、
//   x = 半角文字幅（8px）単位、y = 行（16px）単位。
// 全角文字は 2 セルぶんの幅を占める。
//
// Phase E-0 では表示のみ。getch() は未実装（常に ERR を返す）。

#ifndef M5CURSES_H_INCLUDED
#define M5CURSES_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

// 文字セルの寸法（px）
#define M5C_CELL_W 8
#define M5C_CELL_H 16

// 画面サイズ（文字セル単位）。実機 240x135 に対応する。
#define M5C_COLS 30
#define M5C_ROWS 8

// 属性。ncurses の同名定数と同じ用途で使う。
//   A_REVERSE   候補バーの選択中の候補（view_curses.c の draw_cand 相当）
//   A_UNDERLINE FEP の未確定文字列（view_curses.c の draw_fep 相当）
#define A_NORMAL    0x0000
#define A_REVERSE   0x0001
#define A_UNDERLINE 0x0002

// curses と同じく「入力なし」を表す値
#define ERR (-1)

// M5.begin() → Display 初期化 → 描画用 Canvas 確保 → efontJA_16 設定。
// 失敗しても戻り値では通知しない（curses の initscr と同じ流儀）。
void initscr(void);

// curses では端末を元に戻す処理。実機では戻す先が無いので何もしない。
void endwin(void);

// Canvas を背景色で塗りつぶす。画面にはまだ出ない。
void clear(void);

// Canvas を画面に転送する。ここで初めて表示が変わる。
void refresh(void);

// UTF-8 文字列を (y, x) に描く。x は半角セル単位。
// 属性（A_REVERSE）は描画時点の状態が適用される。
void mvaddstr(int y, int x, const char *s);

// ASCII 1 文字版。
void mvaddch(int y, int x, char c);

// 属性のオン・オフ。
//   A_REVERSE   前景色と背景色を入れ替える
//   A_UNDERLINE 描いた文字列の下端に 1px の水平線を引く
// 複数同時に立てられる（ncurses と同じくビットフラグ）。
void attron(int attrs);
void attroff(int attrs);

// Phase E-0 では未実装。少し待って ERR を返すだけ。
// 実装は Phase E-1（M5Cardputer.Keyboard を curses のキーコードへ変換する）。
int getch(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // M5CURSES_H_INCLUDED
