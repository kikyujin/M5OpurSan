// fep.h — FEP 共通宣言
//
// master_making/fep.cpp の中で定義されていた型・クラス・列挙を、
// 分割コンパイルできるようにそのまま切り出したもの。
// 定義内容（メンバ構成・シグネチャ・セマンティクス）は原典から変更していない。
// 追加したのは CFep の read-only getter 2つ（GetBuffLen / IsSelectMode）のみ。

#ifndef FEP_H_INCLUDED
#define FEP_H_INCLUDED

#include <stdint.h>
#include <assert.h>

// ---- 基本型 ---------------------------------------------------------------

typedef uint16_t UTF16;
typedef int      VKEY;
typedef int      BOOL;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define ASSERT(x) assert(x)

// ---- VKEY -----------------------------------------------------------------
// 印字可能キーは ASCII コードそのものを VKEY として使う（0x20〜0x7E）。
// 制御キーは 0x100 以降に割り当てて印字可能文字と衝突させない。

enum {
  VK_INVALID = -1,
  VK_NONE    = 0,

  VK_ESC     = 0x100,
  VK_ENTER,
  VK_BS,
  VK_LEFT,
  VK_RIGHT,
  VK_UP,
  VK_DOWN,
  VK_HENKAN,   // 変換キー（スペース）
};

// ---- UTF16Array（原典 fep.cpp より） ---------------------------------------
// ここであつかう UTF16 配列は 0 終端されてなく、長さを別途管理する。
class UTF16Array {
  UTF16* ach;   // バッファは指定
  int nSize;    // バッファの最大長
public:
  UTF16Array(UTF16* buff, int size) : ach(buff), nSize(size) {}
  UTF16* GetBuffer() { return ach; }
  UTF16& operator [] (int index) { ASSERT(index >= 0 && index < nSize); return ach[index]; }
  int GetSize() { return nSize; }
  void SetSize(int size) { ASSERT(size >= 0); nSize = size; }  // バッファの最大長を設定する
  static int Copy(UTF16Array& to, UTF16Array& from, int nLen);
};

// ---- システム関数（fep_util.cpp で実装） -----------------------------------

UTF16 CONVERT_UTF16(VKEY key);
int   CONVERT_NUMBER(VKEY key);
BOOL  ISPRINTABLE(VKEY key);
BOOL  ISVALIDKEY(VKEY key);
BOOL  ISNUMBER(VKEY key);
BOOL  ISCAPITAL(UTF16 ch);
BOOL  IS_ROMATOP(UTF16 ch);
BOOL  IS_FULLWIDTH_KANA(UTF16 ch);

// ---- 文字種変換関数（convert.cpp で実装） ----------------------------------

int ConvertToFullWidthKatakana(UTF16Array& to, UTF16Array& from, int len);
int ConvertToFullWidthAlphabet(UTF16Array& to, UTF16Array& from, int len);
int ConvertToHalfWidthAlphabet(UTF16Array& to, UTF16Array& from, int len);

// ---- ローマ字テーブル（roma_table.cpp で実装） ------------------------------

// src[0..len) の先頭を最長一致でローマ字→かな変換する。
// 変換できたら消費した入力文字数（>=1）を返し、out に *outlen 文字のかなを書く。
// 変換できない／続きの入力待ち（"ky" など）の場合は 0 を返す（out は不定）。
// out は最低 4 文字ぶんの領域が必要。
// 不変条件: 戻り値 nUsed > 0 のとき必ず *outlen <= nUsed
//           （ConvertRome の in-place 変換が壊れないための保証）
int LookupRoma(const UTF16* src, int len, UTF16* out, int* outlen);

// かな列 → ローマ字列への逆変換。促音・撥音・拗音を解決する。
// 出力は ASCII 相当のコードポイントだが、かな以外の文字はそのまま 1:1 で
// 通すので UTF16 で受け取る。戻り値は出力した文字数（最大 maxout）。
int KanaStringToRoma(const UTF16* src, int len, UTF16* out, int maxout);

// ---- FEP 本体（原典 fep.cpp より） ------------------------------------------

// 定数定義
#define FEP_MAXBUFF 32  // FEP対応は32文字まで

typedef enum {
  FEP_ERROR,        // エラー
  FEP_INSERTED,     // バッファ書いた
  FEP_THRU,         // 上流判断。
  FEP_UPDATE_DISP,  // 内部ステート変更、画面表示更新せよ
} FEPRET;

class CFep {
private:
  // バッファ
  UTF16 _buff[FEP_MAXBUFF];
  UTF16Array uaBuff;
  int nBuffLen;

  // 選択モード
  BOOL bSelectMode;   // 変換候補選択モード
  int nSelectIndex;   // 変換候補の選択インデックス

  // functions
  void StartSelectMode();
  FEPRET SelectMode(VKEY key, UTF16Array& to_edit);
  FEPRET InputMode(VKEY key, UTF16Array& to_edit);
  BOOL CheckValidItem(int nIndex);

  CFep(const CFep&) = delete;
  CFep& operator=(const CFep&) = delete;

public:
  CFep() : uaBuff(_buff, FEP_MAXBUFF) { ClearMode(); }

  int GetSelectIndex() { return nSelectIndex; }  // 選択インデックス取得
  int GetItem(int nIndex, UTF16Array& item);   // 変換候補取得
  void ClearMode();

  FEPRET KeyPress(VKEY key, UTF16Array& to_edit);  // 編集処理

  // --- 表示用 read-only getter（テストハーネスが内部状態を可視化するために追加）---
  int  GetBuffLen()   const { return nBuffLen; }
  BOOL IsSelectMode() const { return bSelectMode; }
};

#endif // FEP_H_INCLUDED
