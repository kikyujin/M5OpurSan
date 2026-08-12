// test_fep.cpp — curses テストハーネス
//
//   ./test_fep             curses で対話テスト
//   ./test_fep --selftest  curses を使わず、キー列を流し込んで結果を表示
//
// CFep には触らず、KeyPress() の戻り値（FEPRET）だけで画面を更新する。

#include "fep.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <ncurses.h>

// ---------------------------------------------------------------------------
// UTF-16 ユーティリティ
// ---------------------------------------------------------------------------

// UTF-16（BMPのみ）→ UTF-8。戻り値は書き込んだバイト数（終端NUL含まず）。
static int Utf16ToUtf8(const UTF16* src, int len, char* out, int maxout) {
  int n = 0;
  for (int i = 0; i < len; i++) {
    unsigned int c = src[i];
    if (c < 0x80) {
      if (n + 1 >= maxout) break;
      out[n++] = (char)c;
    } else if (c < 0x800) {
      if (n + 2 >= maxout) break;
      out[n++] = (char)(0xC0 | (c >> 6));
      out[n++] = (char)(0x80 | (c & 0x3F));
    } else {
      if (n + 3 >= maxout) break;
      out[n++] = (char)(0xE0 | (c >> 12));
      out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[n++] = (char)(0x80 | (c & 0x3F));
    }
  }
  out[n] = '\0';
  return n;
}

// UTF-16（BMPのみ）→ wchar_t 列。curses の wide API 用。
static int Utf16ToWcs(const UTF16* src, int len, wchar_t* out, int maxout) {
  int n = 0;
  for (int i = 0; i < len && n < maxout - 1; i++) {
    out[n++] = (wchar_t)src[i];
  }
  out[n] = L'\0';
  return n;
}

// ---------------------------------------------------------------------------
// 確定テキストの蓄積
// ---------------------------------------------------------------------------

static const int kFixedMax = 1024;

struct FixedText {
  UTF16 buf[kFixedMax];
  int   len;
  FixedText() : len(0) {}
  void Append(const UTF16* src, int n) {
    for (int i = 0; i < n && len < kFixedMax; i++) {
      buf[len++] = src[i];
    }
  }
};

// ---------------------------------------------------------------------------
// キー変換
// ---------------------------------------------------------------------------

static const int kQuitCtrlC = 3;   // Ctrl-C
static const int kQuitCtrlQ = 17;  // Ctrl-Q

static VKEY KeyToVKey(int ch) {
  switch (ch) {
  case 27:            return VK_ESC;
  case '\n':
  case '\r':
  case KEY_ENTER:     return VK_ENTER;
  case KEY_BACKSPACE:
  case 127:
  case 8:             return VK_BS;
  case KEY_LEFT:      return VK_LEFT;
  case KEY_RIGHT:     return VK_RIGHT;
  case KEY_UP:        return VK_UP;
  case KEY_DOWN:      return VK_DOWN;
  case ' ':           return VK_HENKAN;   // スペース = 変換キー
  default:
    if (ch >= 0x21 && ch <= 0x7E) {
      return (VKEY)ch;                    // 印字可能文字は ASCII そのまま
    }
    return VK_INVALID;
  }
}

// ---------------------------------------------------------------------------
// 画面描画
// ---------------------------------------------------------------------------

static void PutUtf16(int y, int x, const UTF16* src, int len) {
  wchar_t wcs[kFixedMax + 1];
  Utf16ToWcs(src, len, wcs, kFixedMax + 1);
  mvaddwstr(y, x, wcs);
}

static void PutAscii(int y, int x, const char* s) {
  mvaddstr(y, x, s);
}

static void Redraw(CFep& fep, FixedText& fixed) {
  int h, w;
  getmaxyx(stdscr, h, w);
  (void)w;

  erase();

  PutAscii(0, 0, "FEP test harness  --  Space:henkan  Enter:kakutei  ESC:cancel  "
                 "0-3:select  Ctrl-C/Ctrl-Q:quit");
  mvhline(1, 0, ACS_HLINE, w);

  // --- 上部: 確定済みテキスト ---
  PutAscii(2, 0, "[fixed]");
  PutUtf16(3, 2, fixed.buf, fixed.len);

  // --- 下部: FEP 内部状態 ---
  int base = h - 4;
  mvhline(base, 0, ACS_HLINE, w);

  char line[256];
  snprintf(line, sizeof(line), "[fep] mode=%s  bufflen=%d",
           fep.IsSelectMode() ? "SELECT" : "INPUT", fep.GetBuffLen());
  PutAscii(base + 1, 0, line);

  // 未確定バッファ（候補0 = そのまま）
  PutAscii(base + 2, 0, "pending: ");
  if (fep.GetBuffLen() > 0) {
    UTF16 tmp[256];
    UTF16Array ua(tmp, 256);
    int n = fep.GetItem(0, ua);
    if (n > 0) {
      PutUtf16(base + 2, 9, tmp, n);
    }
  }

  // 候補一覧（選択モードのみ）
  if (fep.IsSelectMode() && fep.GetBuffLen() > 0) {
    int x = 0;
    PutAscii(base + 3, x, "cands:");
    x += 7;
    for (int i = 0; i < 8; i++) {
      UTF16 tmp[256];
      UTF16Array ua(tmp, 256);
      int n = fep.GetItem(i, ua);
      if (n < 0) {
        break;   // 候補打ち止め
      }
      char head[16];
      snprintf(head, sizeof(head), "%d:", i);
      bool cur = (i == fep.GetSelectIndex());
      if (cur) attron(A_REVERSE);
      PutAscii(base + 3, x, head);
      x += 2;
      PutUtf16(base + 3, x, tmp, n);
      char utf8[512];
      Utf16ToUtf8(tmp, n, utf8, sizeof(utf8));
      // 全角は2カラム占有するので UTF-16 の文字数では足りない。
      // ざっくり「ASCII は1、それ以外は2」で桁送りする。
      for (int k = 0; k < n; k++) { x += (tmp[k] < 0x80) ? 1 : 2; }
      if (cur) attroff(A_REVERSE);
      x += 2;
    }
  }

  move(base + 2, 9 + fep.GetBuffLen());
  refresh();
}

// ---------------------------------------------------------------------------
// 対話モード
// ---------------------------------------------------------------------------

static int RunCurses() {
  setlocale(LC_ALL, "");
  initscr();
  raw();
  noecho();
  keypad(stdscr, TRUE);

  // keypad(TRUE) だと単独 ESC の判定にデフォルト1秒待たされる。
  // FEP のキャンセルに1秒は長すぎるので詰める。
  set_escdelay(25);

  CFep fep;
  FixedText fixed;

  Redraw(fep, fixed);

  for (;;) {
    int ch = getch();
    if (ch == kQuitCtrlC || ch == kQuitCtrlQ) {
      break;
    }

    VKEY key = KeyToVKey(ch);

    // to_edit は毎回作り直す。
    // KeyPress が SetSize() で「有効長」を書き込むため、使い回すと
    // バッファ容量の情報が失われる。
    UTF16 editbuf[256];
    UTF16Array to_edit(editbuf, 256);

    FEPRET ret = fep.KeyPress(key, to_edit);

    switch (ret) {
    case FEP_INSERTED:
      fixed.Append(editbuf, to_edit.GetSize());
      Redraw(fep, fixed);
      break;
    case FEP_UPDATE_DISP:
      Redraw(fep, fixed);
      break;
    case FEP_ERROR:
      flash();
      Redraw(fep, fixed);
      break;
    case FEP_THRU:
      // エディタ本体は今回なし。何もしない。
      break;
    }
  }

  endwin();
  return 0;
}

// ---------------------------------------------------------------------------
// セルフテスト（curses を使わない）
// ---------------------------------------------------------------------------

namespace {

struct Harness {
  CFep      fep;
  FixedText fixed;

  // 1キー送る
  FEPRET Send(VKEY key) {
    UTF16 editbuf[256];
    UTF16Array to_edit(editbuf, 256);
    FEPRET ret = fep.KeyPress(key, to_edit);
    if (ret == FEP_INSERTED) {
      fixed.Append(editbuf, to_edit.GetSize());
    }
    return ret;
  }

  // ASCII 文字列を1文字ずつ送る（スペースは変換キー扱い）
  FEPRET SendStr(const char* s) {
    FEPRET ret = FEP_THRU;
    for (int i = 0; s[i]; i++) {
      ret = Send(KeyToVKey((unsigned char)s[i]));
    }
    return ret;
  }

  // 未確定バッファを UTF-8 で取り出す
  void Pending(char* out, int maxout) {
    out[0] = '\0';
    if (fep.GetBuffLen() <= 0) {
      return;
    }
    UTF16 tmp[256];
    UTF16Array ua(tmp, 256);
    int n = fep.GetItem(0, ua);
    if (n > 0) {
      Utf16ToUtf8(tmp, n, out, maxout);
    }
  }

  void FixedUtf8(char* out, int maxout) {
    Utf16ToUtf8(fixed.buf, fixed.len, out, maxout);
  }
};

int g_pass = 0;
int g_fail = 0;

void Check(const char* title, const char* got, const char* want) {
  bool ok = (strcmp(got, want) == 0);
  if (ok) { g_pass++; } else { g_fail++; }
  printf("  [%s] %-34s got=\"%s\"  want=\"%s\"\n",
         ok ? "PASS" : "FAIL", title, got, want);
}

void CheckInt(const char* title, int got, int want) {
  bool ok = (got == want);
  if (ok) { g_pass++; } else { g_fail++; }
  printf("  [%s] %-34s got=%d  want=%d\n",
         ok ? "PASS" : "FAIL", title, got, want);
}

const char* RetName(FEPRET r) {
  switch (r) {
  case FEP_ERROR:       return "FEP_ERROR";
  case FEP_INSERTED:    return "FEP_INSERTED";
  case FEP_THRU:        return "FEP_THRU";
  case FEP_UPDATE_DISP: return "FEP_UPDATE_DISP";
  }
  return "?";
}

}  // namespace

static int RunSelfTest() {
  char buf[512];

  printf("=== FEP selftest ===\n");

  // 1. aiueo -> あいうえお
  { Harness h; h.SendStr("aiueo"); h.Pending(buf, sizeof(buf));
    Check("aiueo", buf, "あいうえお"); }

  // 2. kanji -> かんじ
  { Harness h; h.SendStr("kanji"); h.Pending(buf, sizeof(buf));
    Check("kanji", buf, "かんじ"); }

  // 3. nn -> ん
  { Harness h; h.SendStr("nn"); h.Pending(buf, sizeof(buf));
    Check("nn", buf, "ん"); }

  // 4. kka -> っか
  { Harness h; h.SendStr("kka"); h.Pending(buf, sizeof(buf));
    Check("kka", buf, "っか"); }

  // 5. Hello -> 変換されずそのまま
  { Harness h; h.SendStr("Hello"); h.Pending(buf, sizeof(buf));
    Check("Hello (capital bypass)", buf, "Hello"); }

  // 6. スペース（変換キー）で候補選択モードに入る
  { Harness h; h.SendStr("aiueo");
    FEPRET r = h.Send(VK_HENKAN);
    printf("  [ -- ] henkan ret = %s\n", RetName(r));
    CheckInt("space -> select mode", h.fep.IsSelectMode(), TRUE); }

  // 7. ESC でバッファクリア
  { Harness h; h.SendStr("aiueo");
    h.Send(VK_ESC);
    CheckInt("ESC -> bufflen 0", h.fep.GetBuffLen(), 0);
    h.Pending(buf, sizeof(buf));
    Check("ESC -> pending empty", buf, ""); }

  // 8. Enter で確定 → 上部テキストへ
  { Harness h; h.SendStr("kanji");
    FEPRET r = h.Send(VK_ENTER);
    printf("  [ -- ] enter ret = %s\n", RetName(r));
    h.FixedUtf8(buf, sizeof(buf));
    Check("Enter -> fixed text", buf, "かんじ");
    CheckInt("Enter -> bufflen 0", h.fep.GetBuffLen(), 0); }

  printf("\n=== 変換候補（\"kanji\" 入力時） ===\n");
  { Harness h; h.SendStr("kanji");
    const char* names[4] = {"0: そのまま", "1: 全角カタカナ",
                            "2: 全角英字", "3: 半角英字"};
    for (int i = 0; i < 4; i++) {
      UTF16 tmp[256];
      UTF16Array ua(tmp, 256);
      int n = h.fep.GetItem(i, ua);
      Utf16ToUtf8(tmp, (n > 0 ? n : 0), buf, sizeof(buf));
      printf("  %-18s -> \"%s\"\n", names[i], buf);
    } }

  printf("\n=== 追加ケース ===\n");
  { Harness h; h.SendStr("nihongo");   h.Pending(buf, sizeof(buf)); Check("nihongo",   buf, "にほんご"); }
  { Harness h; h.SendStr("kyakka");    h.Pending(buf, sizeof(buf)); Check("kyakka",    buf, "きゃっか"); }
  { Harness h; h.SendStr("shinonome"); h.Pending(buf, sizeof(buf)); Check("shinonome", buf, "しののめ"); }
  { Harness h; h.SendStr("konnnichiha"); h.Pending(buf, sizeof(buf)); Check("konnnichiha", buf, "こんにちは"); }
  { Harness h; h.SendStr("zassi");     h.Pending(buf, sizeof(buf)); Check("zassi",     buf, "ざっし"); }
  { Harness h; h.SendStr("ky");        h.Pending(buf, sizeof(buf)); Check("ky (pending)", buf, "ky"); }
  { Harness h; h.SendStr("n");         h.Pending(buf, sizeof(buf)); Check("n (pending)",  buf, "n"); }

  printf("\n=== 逆変換（かな→ローマ字）===\n");
  { Harness h; h.SendStr("kyakka");
    UTF16 tmp[256]; UTF16Array ua(tmp, 256);
    int n = h.fep.GetItem(3, ua);
    Utf16ToUtf8(tmp, (n > 0 ? n : 0), buf, sizeof(buf));
    Check("きゃっか -> 半角英字", buf, "kyakka"); }
  { Harness h; h.SendStr("nihongo");
    UTF16 tmp[256]; UTF16Array ua(tmp, 256);
    int n = h.fep.GetItem(1, ua);
    Utf16ToUtf8(tmp, (n > 0 ? n : 0), buf, sizeof(buf));
    Check("にほんご -> 全角カタカナ", buf, "ニホンゴ"); }

  printf("\n---- %d passed, %d failed ----\n", g_pass, g_fail);
  return (g_fail == 0) ? 0 : 1;
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) {
    return RunSelfTest();
  }
  return RunCurses();
}
