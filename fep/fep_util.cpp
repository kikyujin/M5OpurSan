// fep_util.cpp — 原典 fep.cpp でダミー宣言されていたシステム関数の実体

#include "fep.h"

// VKEY → UTF-16 コードポイント
// 印字可能キーは ASCII コードをそのまま VKEY にしているので素通し。
UTF16 CONVERT_UTF16(VKEY key) {
  if (key >= 0x20 && key <= 0x7E) {
    return (UTF16)key;
  }
  return 0;
}

// 数字キー → 数値
int CONVERT_NUMBER(VKEY key) {
  if (ISNUMBER(key)) {
    return key - '0';
  }
  return -1;
}

// 印字可能文字か（制御キーは 0x100 以降なので自動的に弾かれる）
BOOL ISPRINTABLE(VKEY key) {
  return (key >= 0x20 && key <= 0x7E) ? TRUE : FALSE;
}

// 有効なキー入力か
BOOL ISVALIDKEY(VKEY key) {
  if (key == VK_NONE || key == VK_INVALID) {
    return FALSE;
  }
  return TRUE;
}

// 数字キーか
BOOL ISNUMBER(VKEY key) {
  return (key >= '0' && key <= '9') ? TRUE : FALSE;
}

// 大文字か（先頭大文字ならローマ字変換をバイパスする判定に使う）
BOOL ISCAPITAL(UTF16 ch) {
  return (ch >= 'A' && ch <= 'Z') ? TRUE : FALSE;
}

// ローマ字の先頭文字になりうるか
BOOL IS_ROMATOP(UTF16 ch) {
  return (ch >= 'a' && ch <= 'z') ? TRUE : FALSE;
}
