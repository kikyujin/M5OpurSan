// candidate_bar.h — 変換候補バー（C11、curses 非依存）
//
// ひらがなの読み（UTF-8）を受け取り、辞書を引いて候補リストを組み立て、
// 30 半角ぶんのビューポートに収まるレンダリング結果を返す。
// 実際の画面描画は呼び出し側（view_curses.c / env_m5.c）の責務。
//
// 文字列はすべて UTF-8。エディタ（UTF-16）との変換は Phase D で
// 呼び出し側に 1 箇所だけ置く（utf8_utf16.h）。
//
// 候補リストの構成（順序）:
//   0        : ひらがな（入力そのまま）
//   1〜N     : 辞書候補（LRU 順。単漢字は辞書候補の末尾にまとめる）
//   N+1      : カタカナ
//   N+2      : 全角英字
//   N+3      : 半角英字
//
// cand_bar_start() 直後の選択は 1 番（辞書の第 1 候補）。
// 変換したいのはたいてい漢字なので、0 番から始めると必ず 1 回余計に
// キーを押すことになる。辞書候補が 1 件も無いときだけ 0 番になる。

#ifndef OPUR_CANDIDATE_BAR_H_INCLUDED
#define OPUR_CANDIDATE_BAR_H_INCLUDED

#include <stddef.h>

#include "opur_dict.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- 定数 ------------------------------------------------------------------

#define CAND_MAX          128  // 候補の最大数
#define CAND_READING_MAX  100  // 読みの最大バイト数（FEP バッファ 32 文字 = 96 バイト）
// 候補 1 件の最大バイト数。最悪ケースは「全角英字」で、かな 32 文字が
// ローマ字 96 文字に展開され、全角化で 3 倍の 288 バイトになる。
#define CAND_TEXT_MAX     320
#define CAND_LRU_MAX      256  // LRU エントリ数の上限
#define CAND_LRU_TEXT_MAX  64  // LRU に覚える表記の最大バイト数

#define CAND_VIEW_WIDTH    30  // ビューポート幅（半角換算。240px / 8px）
#define CAND_RENDER_MAX   512  // レンダリング結果のバイト数上限

// ---- キー ------------------------------------------------------------------
// 印字可能キーは ASCII をそのまま渡す（スペース = ' '、数字 = '0'〜'9'）。
// 制御キーは 0x100 以降（fep.h の VKEY と同じ流儀）。

enum {
    CAND_KEY_LEFT = 0x100,
    CAND_KEY_RIGHT,
    CAND_KEY_ENTER,
    CAND_KEY_ESC
};

// ---- キー処理の結果 ---------------------------------------------------------

typedef enum {
    CAND_IGNORED = 0,  // 何も起きなかった
    CAND_UPDATED,      // 選択またはスクロールが変わった → 再描画せよ
    CAND_COMMITTED,    // 確定した → cand_bar_committed() を読め
    CAND_CANCELLED     // ESC で取り消した
} CandResult;

// ---- 文字種変換の注入 -------------------------------------------------------
// PC ビルドは conv_utf8.c、M5 ビルドは fep/convert.cpp の extern "C" ラッパを渡す。

typedef struct {
    void (*to_katakana)(const char *src_utf8, char *dst_utf8, size_t dst_size);
    void (*to_fullwidth)(const char *src_utf8, char *dst_utf8, size_t dst_size);
    void (*to_halfwidth)(const char *src_utf8, char *dst_utf8, size_t dst_size);
} CandConv;

// ---- 内部状態 ---------------------------------------------------------------

typedef struct {
    char text[CAND_TEXT_MAX];
    int  is_tankan;   // 表記が 1 コードポイントか
} CandItem;

typedef struct {
    char reading[CAND_READING_MAX];
    char surface[CAND_LRU_TEXT_MAX];
} CandLruEntry;

// ※ sizeof(CandBar) は約 84KB ある。スタックではなく静的領域か
//    ヒープに置くこと。
typedef struct {
    OpurDict *dict;    // NULL 可（辞書なしでも文字種候補だけは出る）
    CandConv  conv;    // メンバが NULL ならその候補は出さない

    char reading[CAND_READING_MAX];   // 変換対象の読み
    CandItem item[CAND_MAX];
    int count;         // 候補数
    int dict_count;    // うち辞書候補の数（item[1] 〜 item[dict_count]）
    int sel;           // 選択中の候補インデックス
    int win_start;     // ビューポート左端の候補インデックス

    char committed[CAND_TEXT_MAX];    // 直近に確定した文字列

    CandLruEntry lru[CAND_LRU_MAX];
    int lru_count;     // 先頭ほど新しい
} CandBar;

// ---- レンダリング結果 -------------------------------------------------------

typedef struct {
    char text[CAND_RENDER_MAX];  // 描画すべき 1 行（UTF-8、◀▶ を含む）
    int  width;                  // text の表示幅（半角換算）
    int  first;                  // 画面内の先頭候補インデックス
    int  last;                   // 画面内の末尾候補インデックス（含む）
    int  has_left;               // 左端外に候補がある（◀ を出した）
    int  has_right;              // 右端外に候補がある（▶ を出した）
    int  sel_col;                // 選択候補の開始カラム（半角換算、0 始まり）
    int  sel_width;              // 選択候補の表示幅（番号を含む）
} CandRender;

// ---- API --------------------------------------------------------------------

// bar を初期化する。LRU も空になる。conv は中身をコピーするので寿命は問わない。
void cand_bar_init(CandBar *bar, OpurDict *dict, const CandConv *conv);

// reading（ひらがな UTF-8）に対する候補リストを組み立てる。
// 候補数を返す。reading が空なら -1（状態は変えない）。
int cand_bar_start(CandBar *bar, const char *reading_utf8);

// 候補リストを空にする（LRU は保持する）。
void cand_bar_clear(CandBar *bar);

int         cand_bar_count(const CandBar *bar);
int         cand_bar_selected(const CandBar *bar);
const char *cand_bar_text(const CandBar *bar, int index);   // 範囲外は ""
int         cand_bar_is_tankan(const CandBar *bar, int index);

// キー 1 つを処理する。
CandResult cand_bar_key(CandBar *bar, int key);

// 直近に確定した文字列。まだ確定していなければ ""。
const char *cand_bar_committed(const CandBar *bar);

// 現在の選択・スクロール位置から 1 行ぶんの描画内容を作る。
void cand_bar_render(const CandBar *bar, CandRender *out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPUR_CANDIDATE_BAR_H_INCLUDED
