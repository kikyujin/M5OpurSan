// editor.h — OpurSan エディタ: バッファ・カーソル API
//
// ここで宣言する API はすべて環境非依存（editor.c で実装）。
// curses / M5Stack / FEP のいずれにも依存しない。
//
// 将来 C++ 側（fep/*.cpp）とリンクするため extern "C" で囲む。

#ifndef OPUR_EDITOR_H_INCLUDED
#define OPUR_EDITOR_H_INCLUDED

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- 定数 ------------------------------------------------------------------

#define OPUR_BUF_MAX 512     // バッファ最大文字数（UTF-16 単位、BMP のみ）
#define OPUR_COLS    30      // 本文領域の幅（半角換算）
#define OPUR_ROWS    7       // 本文領域の行数
#define OPUR_LF      0x000A  // 改行（buf に 1 文字として格納）

// 論理行の最大数。全文字が LF のとき 512 行 + 末尾の空行 1 で 513。
#define OPUR_MAX_LINES (OPUR_BUF_MAX + 1)

// ---- エディタ状態 ------------------------------------------------------------

typedef struct {
    uint16_t buf[OPUR_BUF_MAX];
    int len;         // 現在の文字数（0〜512）
    int cursor;      // カーソル位置（0〜len）
    int goal_col;    // 上下移動時の目標カラム（-1 = 未設定）
    int scroll_top;  // 表示先頭行番号（0 始まり）
} OpurEditor;

// ---- 折り返し結果 ------------------------------------------------------------

typedef struct {
    int start;   // 行頭の buf インデックス
    int end;     // 行末の次の buf インデックス（LF 終端行は LF を含む）
    int width;   // 表示幅（半角換算。LF は 0 幅なので含まない）
    int has_lf;  // この行が LF で終端しているか（1/0）
} OpurLine;

typedef struct {
    OpurLine line[OPUR_MAX_LINES];
    int count;   // 常に 1 以上（空バッファでも空行 1 行）
} OpurLayout;

// ---- API --------------------------------------------------------------------

// 空バッファで初期化する。
void opur_init(OpurEditor* ed);

// 文字の表示幅。ASCII 印字可能 = 1、LF = 0、それ以外 = 2（全角）。
int opur_char_width(uint16_t ch);

// buf[0] から走査して折り返しを求める。行頭テーブルは保持しない。
void opur_layout(const OpurEditor* ed, OpurLayout* out);

// cursor がどの論理行にいるか。
//
// 折り返し境界（前行の end == 次行の start）は次行の col 0 に属するものとする
// （last-line affinity）。詳細は editor.c の注釈を参照。
int opur_cursor_line(const OpurLayout* lay, int cursor);

// line 行内での cursor のカラム位置（半角換算、0 始まり）。
int opur_cursor_col(const OpurEditor* ed, const OpurLayout* lay,
                    int line, int cursor);

// line 行で goal カラムに最も近い文字の左端の buf インデックスを返す。
// goal が行幅を超える場合は行末に clamp する（3 パターン: LF 終端 / 折り返し /
// 最終行末）。全角の途中には止まらない。
int opur_index_at_col(const OpurEditor* ed, const OpurLayout* lay,
                      int line, int goal);

// ---- 編集操作（goal_col をリセットする） ---------------------------------------

void opur_insert(OpurEditor* ed, uint16_t ch);  // 満杯・制御文字は無視
void opur_backspace(OpurEditor* ed);

// ---- カーソル移動 -------------------------------------------------------------

void opur_left(OpurEditor* ed);   // goal_col をリセットする
void opur_right(OpurEditor* ed);  // goal_col をリセットする
void opur_up(OpurEditor* ed);     // goal_col を保持する
void opur_down(OpurEditor* ed);   // goal_col を保持する

// カーソルが表示範囲に入るよう scroll_top を調整する。
void opur_update_scroll(OpurEditor* ed);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPUR_EDITOR_H_INCLUDED
