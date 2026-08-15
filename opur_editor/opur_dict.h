// opur_dict.h — system.dic 辞書引き API
//
// test_dict.c に埋まっていた辞書アクセス層を切り出したもの。
// 実装内容（アルゴリズム・セマンティクス）は原典から変更していない。
//
// 辞書は可変長レコードなので、ファイル末尾のオフセットテーブル
// （entry_count × uint32 LE）を介してバイナリサーチする。
// system.dic は fopen + fseek で引き、RAM には全展開しない。
//
// 前回位置再利用（hint_pos）は実装・検証してあるが、既定では使わない。
// 実測でバイナリサーチの 10 倍以上シークするため。
// 呼び出し側は hint_pos に -1 を渡すこと。

#ifndef OPUR_DICT_H_INCLUDED
#define OPUR_DICT_H_INCLUDED

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- 定数 ------------------------------------------------------------------

#define OPUR_DICT_MAGIC       "OPUR"
#define OPUR_DICT_VERSION     1
#define OPUR_DICT_HEADER_SIZE 16
#define OPUR_DICT_FIELD_MAX   256  // reading_len / surface_len は uint8 なので最大 255

// ---- 型 --------------------------------------------------------------------

typedef struct {
    FILE *fp;
    uint32_t entry_count;
    long table_offset;    // オフセットテーブルの先頭位置
    long last_pos;        // 前回検索で見つかった位置（再利用用）
    int last_result;      // 前回検索の結果（0=found, -1=not found）
} OpurDict;

// reading / surface はどちらも UTF-8。0 を返すと走査を続け、非 0 で打ち切る。
typedef int (*DictHitCallback)(const char *reading, const char *surface, void *ctx);

// ---- open / close ----------------------------------------------------------

// 0=OK / -1=I/O or open 失敗 / -2=magic 不一致 / -3=version 不一致 / -4=壊れている
int  opur_dict_open(OpurDict *dict, const char *path);
void opur_dict_close(OpurDict *dict);

// ---- 検索 ------------------------------------------------------------------

// reading と完全一致する読みを持つレコードを順に cb へ渡す。ヒット数を返す。
// hint_pos は -1 を渡すこと（前回位置再利用は既定では使わない経路）。
int  opur_dict_search(OpurDict *dict, const char *reading, long hint_pos,
                      DictHitCallback cb, void *ctx);

// ---- 低レベルアクセス（テスト・ベンチ用） ------------------------------------

// index 番目のレコードの先頭バイト位置。失敗時 -1。
long opur_dict_entry_offset(OpurDict *dict, uint32_t index);

// pos のレコードを読み、次のレコードの位置を返す。失敗時 -1。
// rbuf / sbuf は OPUR_DICT_FIELD_MAX バイト必要。surface が不要なら sbuf に NULL。
long opur_dict_read_record(OpurDict *dict, long pos,
                           char *rbuf, size_t *rlen,
                           char *sbuf, size_t *slen);

// ---- 計測 ------------------------------------------------------------------
// hint 経路とバイナリサーチはコスト特性が全く違うので分けて数える。

typedef struct {
    long searches;       // 検索回数（全経路）
    long seeks;          // 総シーク回数
    long hint_searches;  // hint 経路を通った検索回数
    long hint_seeks;     //  〃 のシーク回数
    long bs_searches;    // バイナリサーチを通った検索回数
    long bs_seeks;       //  〃 のシーク回数
} OpurDictStats;

void opur_dict_get_stats(OpurDictStats *out);
void opur_dict_reset_stats(void);

// ---- UTF-8 ユーティリティ ---------------------------------------------------

// s の文字数（コードポイント数）。
int opur_u8_count(const char *s);

// s の先頭から nchars 文字ぶんのバイト数。
size_t opur_u8_bytes(const char *s, int nchars);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPUR_DICT_H_INCLUDED
