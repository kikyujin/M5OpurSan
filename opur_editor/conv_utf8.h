// conv_utf8.h — 文字種変換（PC ビルド用の C11 / UTF-8 実装）
//
// candidate_bar には CandConv 経由で注入する。M5 ビルドでは
// fep/convert.cpp（UTF-16）を extern "C" ラップしたものに差し替える想定なので、
// candidate_bar 側はこのヘッダに直接依存しない。
//
// セマンティクスは fep/convert.cpp と揃えてある:
//   - カタカナ : ひらがな U+3041〜U+3096 を +0x60 する。それ以外は素通し
//   - 全角英字 : かな→ローマ字の逆変換をかけてから ASCII を全角化
//   - 半角英字 : かな→ローマ字の逆変換をかけてから全角英数を半角化
//
// 逆変換テーブルは fep/roma_table.cpp の canonical=true エントリと同一。

#ifndef OPUR_CONV_UTF8_H_INCLUDED
#define OPUR_CONV_UTF8_H_INCLUDED

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// いずれも dst を NUL 終端する。入りきらないぶんは文字境界で打ち切る。
// dst_size が 0 のときは何もしない。

void conv_utf8_to_katakana(const char *src, char *dst, size_t dst_size);
void conv_utf8_to_fullwidth(const char *src, char *dst, size_t dst_size);
void conv_utf8_to_halfwidth(const char *src, char *dst, size_t dst_size);

// かな列 → ローマ字列（促音・撥音・拗音を解決する）。
// 全角英字 / 半角英字はこれを通してから幅を変える。
// 上 3 つの内部で使うが、テストから直接叩けるように公開しておく。
void conv_utf8_kana_to_roma(const char *src, char *dst, size_t dst_size);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPUR_CONV_UTF8_H_INCLUDED
