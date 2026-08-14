// view.h — 描画層の I/F
//
// 実装は view_curses.c（PC）。将来 view_m5.c と差し替える境界。
// main.cpp（C++）から呼ぶので extern "C" で囲む。
//
// 画面は枠内 8 行。実機（M5 Cardputer: 240x135 / efont 16px = 30 半角 x 8 行）と
// 1:1 に対応する:
//   本文 OPUR_ROWS(6) 行 / FEP 入力行 1 行 / 候補バー 1 行
// ステータスバーは枠外に出す（PC デモ専用で実機には無い）。

#ifndef OPUR_VIEW_H_INCLUDED
#define OPUR_VIEW_H_INCLUDED

#include <stdint.h>

#include "candidate_bar.h"
#include "editor.h"

#ifdef __cplusplus
extern "C" {
#endif

void view_init(void);
void view_end(void);

// 端末が枠を描くには小さすぎるとき 0 を返す。
int  view_fits(void);
void view_too_small(void);

// 1 回の全画面再描画。
//   fep_buf / fep_len : 未確定の FEP バッファ（かな + ローマ字途中の混在）。
//                       fep_len が 0 なら FEP 入力行は空になる。
//   bar               : 候補選択中なら CandBar、そうでなければ NULL。
// 最後にカーソルを本文側の位置へ置くので、FEP 入力中も本文カーソルが見える。
void view_draw(const OpurEditor* ed,
               const uint16_t* fep_buf, int fep_len,
               const CandBar* bar);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPUR_VIEW_H_INCLUDED
