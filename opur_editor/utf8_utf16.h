// utf8_utf16.h — UTF-8 ⇔ UTF-16 変換と UTF-8 コードポイント入出力
//
// 辞書 / candidate_bar は UTF-8、エディタ（editor.h）と FEP（fep.h）は UTF-16。
// その境界で使う。Phase D の結合では main 側の 1 箇所だけがこれを呼ぶ想定。
//
// BMP のみ対応（サロゲートペア非対応）。U+FFFF を超えるコードポイントは
// 変換時に U+FFFD（REPLACEMENT CHARACTER）に置き換える。
// 不正なバイト列も U+FFFD にして 1 バイト進める（脱落せず必ず前進する）。

#ifndef OPUR_UTF8_UTF16_H_INCLUDED
#define OPUR_UTF8_UTF16_H_INCLUDED

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U8_REPLACEMENT 0xFFFDu  // 不正な入力の置き換え先

// s の先頭 1 文字を復号する。消費したバイト数（1〜4）を返す。
// 文字列終端では 0 を返し *cp は 0 になる。
// 不正なバイト列のときは 1 を返し *cp = U8_REPLACEMENT。
int u8_decode(const char *s, uint32_t *cp);

// cp を UTF-8 で out に書く。書いたバイト数（1〜3）を返す。NUL は付けない。
// out は最低 3 バイト必要。BMP 外は U+FFFD として書く。
int u8_encode(uint32_t cp, char *out);

// UTF-8 文字列 → UTF-16 配列。書き込んだ要素数を返す（NUL 終端はしない）。
// dst_max を超えるぶんは切り捨てる。
int utf8_to_utf16(const char *src, uint16_t *dst, int dst_max);

// UTF-16 配列（len 要素）→ UTF-8 文字列。NUL 終端し、NUL を除く長さを返す。
// dst_size は NUL を含むバイト数。途中で入りきらなくなったら文字境界で打ち切る。
int utf16_to_utf8(const uint16_t *src, int len, char *dst, size_t dst_size);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPUR_UTF8_UTF16_H_INCLUDED
