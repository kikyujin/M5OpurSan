#include "fep.h"

// 型・クラス・列挙の宣言は fep.h に切り出した（原典 master_making/fep.cpp から
// 内容は変更していない）。このファイルには実装のみを残す。

// コピーの実装
int UTF16Array::Copy(UTF16Array& to, UTF16Array& from, int nLen) {
  // コピーする最大長を決定
  int last = (to.nSize < from.nSize) ? to.nSize : from.nSize;
  if (last > nLen) {
    last = nLen;
  }
  for (int n = 0; n < last; n++) {
    to[n] = from[n];
  }
  return last;  // コピーした文字数を返す
}

// １文字分のローマ字変換
static BOOL ConvertRomeToKana(UTF16Array& to, int& dest, UTF16Array& from, int& sour, int len) {
  //
  if (dest >= to.GetSize() || sour >= from.GetSize()) {
    return FALSE;
  }

  // そもそもローマ字の１文字目ではない
  if (!IS_ROMATOP(from[sour])) {
    to[dest] = from[sour];
    sour++;
    dest++;
    return TRUE;
  }
  // ローマ字の可能性あり
  // ローマ字マップを比較しながら to[dest] にローマ字を入れる。
  // 残り文字数に注意（lenをチェック)。
  // 'n' に注意（ん、なのか na などなのか）
  UTF16 kana[4];
  int nKana = 0;
  int nUsed = LookupRoma(&from[sour], len - sour, kana, &nKana);

  if (nUsed == 0) {
    // まだ確定できない（"ky" のように続きの入力待ち）か、変換対象外。
    // 生のまま残しておけば、次の打鍵で ConvertRome がバッファ全体を
    // 再走査するときに改めて解決される。
    to[dest] = from[sour];
    sour++;
    dest++;
    return TRUE;
  }

  // 出力先が足りない
  if (dest + nKana > to.GetSize()) {
    return FALSE;
  }

  // LookupRoma は nKana <= nUsed を保証するので dest <= sour が保たれる。
  // （to と from が同じバッファでも未読領域を壊さない）
  for (int i = 0; i < nKana; i++) {
    to[dest] = kana[i];
    dest++;
  }
  sour += nUsed;
  return TRUE;
}

// ローマ字変換関数
// to と from が同じバッファの場合もある
static int ConvertRome(UTF16Array& to, UTF16Array& from, int len) {
  ASSERT(len > 0);  // len が 0 ではこないはず
  if (ISCAPITAL(from[0])) {
    // 先頭が大文字の場合は何もしない
    return UTF16Array::Copy(to, from, len);  // そのままコピーして返す
  }

  // ここにローマ字変換の処理を実装する
  int sour = 0, dest = 0;
  for (;;) {
    if (!ConvertRomeToKana(to, dest, from, sour, len)) {
      break;  // ローマ字変換が完了したらループを抜ける
    }
    if (sour >= len) {
      break;
    }
  }
  return dest;
}

// モードをクリアする
void CFep::ClearMode() {
  nBuffLen = 0;
  bSelectMode = FALSE;
  nSelectIndex = 0;
}

// 変換候補選択モードに入る
void CFep::StartSelectMode() {
  bSelectMode = TRUE;  // 変換候補選択モードに入る
  nSelectIndex = 0;   // 選択インデックスを初期化
}

// 変換候補の有効性をチェックする
BOOL CFep::CheckValidItem(int nIndex) {
  // 変換候補の有効性をチェックする処理をここに実装する
  // 例: nIndex が有効な範囲内かどうかを確認する
  return (nIndex >= 0 && nIndex < 4);  // 仮に4つの候補があるとする
}

// 変換候補取得の実装
int CFep::GetItem(int nIndex, UTF16Array& save_here) {
  // 変換候補の取得処理をここに実装する
  ASSERT(nBuffLen > 0);
  switch (nIndex) {
  case 0:
    // そのまま
    return UTF16Array::Copy(save_here, uaBuff, nBuffLen);
  case 1:
    // 全角カタカナ
    return ConvertToFullWidthKatakana(save_here, uaBuff, nBuffLen);
  case 2:
    // 全角英字
    return ConvertToFullWidthAlphabet(save_here, uaBuff, nBuffLen);
  case 3:
    // 半角英字
    return ConvertToHalfWidthAlphabet(save_here, uaBuff, nBuffLen);

  default:
    // 本当はここに漢字変換の候補を追加する処理を実装する
    return -1;  // エラーを返す
  }
}

// 変換候補選択モード：
//  画面下部に現在バッファに入ってる文字の変換候補が表示されてる想定。
//  to_edit の　サイズが変更されるので注意

FEPRET CFep::SelectMode(VKEY key, UTF16Array& to_edit) {
  // 変換候補選択モードの処理
  // ここでは仮に選択モードを終了する
  switch (key) {
  case VK_ESC:
    // 選択モードキャンセル
    ClearMode();
    return FEP_UPDATE_DISP;  // 内部ステート変更、画面表示更新

  case VK_ENTER:
    // 現在選択中の候補を選択した
    to_edit.SetSize(GetItem(nSelectIndex, to_edit));
    ClearMode();
    return FEP_INSERTED;

  case VK_RIGHT:
  case VK_HENKAN:
    // 右キー or 変換キーで次の候補に移動
    if (CheckValidItem(nSelectIndex + 1)) {
      nSelectIndex++;
      return FEP_UPDATE_DISP;  // 内部ステート変更、画面表示更新
    }
    return FEP_ERROR;  // 無効な候補番号の場合はエラー

  case VK_LEFT:
    // 左キーで前の候補に移動
    if (CheckValidItem(nSelectIndex - 1)) {
      nSelectIndex--;
      return FEP_UPDATE_DISP;  // 内部ステート変更、画面表示更新
    }
    return FEP_ERROR;  // 無効な候補番号の場合はエラー

  default:
    if (ISNUMBER(key)) {
      // 数字キーで選択インデックスを変更
      int n = CONVERT_NUMBER(key);
      if (CheckValidItem(n)) {
        nSelectIndex = n;  // 有効な候補がある場合のみインデックスを変更
        return FEP_UPDATE_DISP;  // 内部ステート変更、画面表示更新
      }
    }
    return FEP_ERROR;  // 無効な候補番号の場合はエラー
  }
}

// 通常の入力モード:
//  ユーザーが入力した文字がバッファに反映される。
//  入力文字がローマ字変換できるときは自動的に変換される。
//  to_edit の　サイズが変更されるので注意

FEPRET CFep::InputMode(VKEY key, UTF16Array& to_edit) {
  // 通常の入力モードの処理
  switch (key) {
  case VK_ESC:
    // 入力キャンセル
    if (nBuffLen > 0) {
      // 有効長を　0　にして、入力内容をクリアする
      nBuffLen = 0;

      // 画面更新
      return FEP_UPDATE_DISP;
    }
    // バッファが空の場合は何もしない
    return FEP_THRU;

  case VK_BS:
    // バックスペース
    if (nBuffLen > 0) {
      // 最後の文字を削除する。
      nBuffLen--;

      // 画面更新
      return FEP_UPDATE_DISP;
    }
    // バッファが空の場合は何もしない
    return FEP_THRU;

  case VK_HENKAN:
    // 変換キー(多分スペース)
    if (nBuffLen > 0) {
      // バッファに文字がある場合は変換候補選択モードに
      StartSelectMode();

      // 内部ステート変更、画面表示
      return FEP_UPDATE_DISP;
    }
    // バッファが空の場合は何もしない
    return FEP_THRU;

  case VK_ENTER:
    // エンターキー
    if (nBuffLen > 0) {
      // 現在のバッファ内容を確定とする。
      to_edit.SetSize(UTF16Array::Copy(to_edit, uaBuff, nBuffLen));
      ClearMode();
      return FEP_INSERTED;
    }
    // バッファが空の場合は何もしない
    return FEP_THRU;

  default:
    // 文字など
    if (!ISPRINTABLE(key)) {
      // 入力文字が無効な場合、何もしない。
      return FEP_THRU;
    }

    // バッファが一杯のときは警告
    if (nBuffLen >= uaBuff.GetSize()) {
      return FEP_ERROR;
    }

    // バッファに文字を追加する。
    uaBuff[nBuffLen++] = CONVERT_UTF16(key);

    // 自動ローマ字変換
    nBuffLen = ConvertRome(uaBuff, uaBuff, nBuffLen);   // 長さは中で更新される
    return FEP_UPDATE_DISP;
  }
}

// 編集処理（上流からループで呼ばれるステートマシン）
FEPRET CFep::KeyPress(VKEY key, UTF16Array& to_edit) {
  if (!ISVALIDKEY(key)) {
    return FEP_THRU;  // 無効なキーは無視
  }

  // モードに応じて処理を分岐
  if (bSelectMode) {
    // 変換候補選択モードの処理
    return SelectMode(key, to_edit);
  } else  {
    // 通常の入力モードの処理
    return InputMode(key, to_edit);
  }
}
