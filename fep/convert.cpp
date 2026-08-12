// convert.cpp — 文字種変換（CFep::GetItem の変換候補用）
//
// 候補2「全角英字」/候補3「半角英字」は、バッファがすでにローマ字変換済みの
// かなになっているため、まず かな→ローマ字 の逆変換をかけてから
// 全角化／半角化する。

#include "fep.h"

namespace {

// 逆変換の一時バッファ。かな1文字が最大3文字程度に展開されるので余裕をとる。
const int kTempMax = FEP_MAXBUFF * 4;

const UTF16 kHiraganaFirst = 0x3041;  // ぁ
const UTF16 kHiraganaLast  = 0x3096;  // ゖ
const UTF16 kKanaOffset    = 0x0060;  // ひらがな → カタカナ

const UTF16 kWideOffset    = 0xFEE0;  // ASCII → 全角英数
const UTF16 kWideSpace     = 0x3000;  // 全角スペース

}  // namespace

// ひらがな → 全角カタカナ（U+3041〜 → U+30A1〜）
int ConvertToFullWidthKatakana(UTF16Array& to, UTF16Array& from, int len) {
  int cap = to.GetSize();
  int n = 0;
  for (int i = 0; i < len && n < cap; i++) {
    UTF16 c = from[i];
    if (c >= kHiraganaFirst && c <= kHiraganaLast) {
      c = (UTF16)(c + kKanaOffset);
    }
    to[n++] = c;
  }
  return n;
}

// 半角英字（かなはローマ字へ逆変換、全角英数は半角へ）
int ConvertToHalfWidthAlphabet(UTF16Array& to, UTF16Array& from, int len) {
  UTF16 tmp[kTempMax];
  int m = KanaStringToRoma(from.GetBuffer(), len, tmp, kTempMax);

  int cap = to.GetSize();
  int n = 0;
  for (int i = 0; i < m && n < cap; i++) {
    UTF16 c = tmp[i];
    if (c == kWideSpace) {
      c = ' ';
    } else if (c >= (UTF16)(0x21 + kWideOffset) && c <= (UTF16)(0x7E + kWideOffset)) {
      c = (UTF16)(c - kWideOffset);
    }
    to[n++] = c;
  }
  return n;
}

// 全角英字（かなはローマ字へ逆変換してから全角化）
int ConvertToFullWidthAlphabet(UTF16Array& to, UTF16Array& from, int len) {
  UTF16 tmp[kTempMax];
  int m = KanaStringToRoma(from.GetBuffer(), len, tmp, kTempMax);

  int cap = to.GetSize();
  int n = 0;
  for (int i = 0; i < m && n < cap; i++) {
    UTF16 c = tmp[i];
    if (c == ' ') {
      c = kWideSpace;
    } else if (c >= 0x21 && c <= 0x7E) {
      c = (UTF16)(c + kWideOffset);
    }
    to[n++] = c;
  }
  return n;
}
