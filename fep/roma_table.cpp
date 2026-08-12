// roma_table.cpp — ローマ字⇔かな変換テーブルと検索
//
// fep.cpp の ConvertRomeToKana は static のままここへは移せないので、
// このファイルはテーブルと検索関数だけを提供し、fep.cpp から呼んでもらう。

#include "fep.h"

namespace {

struct RomaEntry {
  const char*     roma;
  const char16_t* kana;
  bool            canonical;  // かな→ローマ字の逆変換で使う代表つづり
};

// 表記ゆれは複数エントリで受ける。canonical=true が逆変換時の代表。
// 不変条件: どのエントリも strlen(kana) <= strlen(roma)
//           （ConvertRome の in-place 変換が壊れないため）
const RomaEntry kTable[] = {
  // --- 母音 ---
  {"a", u"あ", true}, {"i", u"い", true}, {"u", u"う", true},
  {"e", u"え", true}, {"o", u"お", true},

  // --- か行 ---
  {"ka", u"か", true}, {"ki", u"き", true}, {"ku", u"く", true},
  {"ke", u"け", true}, {"ko", u"こ", true},
  {"ga", u"が", true}, {"gi", u"ぎ", true}, {"gu", u"ぐ", true},
  {"ge", u"げ", true}, {"go", u"ご", true},

  // --- さ行 ---
  {"sa", u"さ", true},  {"si", u"し", false}, {"shi", u"し", true},
  {"su", u"す", true},  {"se", u"せ", true},  {"so", u"そ", true},
  {"za", u"ざ", true},  {"zi", u"じ", false}, {"ji", u"じ", true},
  {"zu", u"ず", true},  {"ze", u"ぜ", true},  {"zo", u"ぞ", true},

  // --- た行 ---
  {"ta", u"た", true},  {"ti", u"ち", false}, {"chi", u"ち", true},
  {"tu", u"つ", false}, {"tsu", u"つ", true},
  {"te", u"て", true},  {"to", u"と", true},
  {"da", u"だ", true},  {"di", u"ぢ", true},  {"du", u"づ", true},
  {"de", u"で", true},  {"do", u"ど", true},

  // --- な行 ---
  {"na", u"な", true}, {"ni", u"に", true}, {"nu", u"ぬ", true},
  {"ne", u"ね", true}, {"no", u"の", true},

  // --- は行 ---
  {"ha", u"は", true}, {"hi", u"ひ", true},
  {"hu", u"ふ", false}, {"fu", u"ふ", true},
  {"he", u"へ", true}, {"ho", u"ほ", true},
  {"ba", u"ば", true}, {"bi", u"び", true}, {"bu", u"ぶ", true},
  {"be", u"べ", true}, {"bo", u"ぼ", true},
  {"pa", u"ぱ", true}, {"pi", u"ぴ", true}, {"pu", u"ぷ", true},
  {"pe", u"ぺ", true}, {"po", u"ぽ", true},

  // --- ま行 ---
  {"ma", u"ま", true}, {"mi", u"み", true}, {"mu", u"む", true},
  {"me", u"め", true}, {"mo", u"も", true},

  // --- や行 ---
  {"ya", u"や", true}, {"yu", u"ゆ", true}, {"yo", u"よ", true},

  // --- ら行 ---
  {"ra", u"ら", true}, {"ri", u"り", true}, {"ru", u"る", true},
  {"re", u"れ", true}, {"ro", u"ろ", true},

  // --- わ行 ---
  {"wa", u"わ", true}, {"wo", u"を", true},

  // --- 拗音 ---
  {"kya", u"きゃ", true}, {"kyu", u"きゅ", true}, {"kyo", u"きょ", true},
  {"gya", u"ぎゃ", true}, {"gyu", u"ぎゅ", true}, {"gyo", u"ぎょ", true},
  {"sha", u"しゃ", true}, {"shu", u"しゅ", true}, {"sho", u"しょ", true},
  {"sya", u"しゃ", false},{"syu", u"しゅ", false},{"syo", u"しょ", false},
  {"ja",  u"じゃ", true}, {"ju",  u"じゅ", true}, {"jo",  u"じょ", true},
  {"jya", u"じゃ", false},{"jyu", u"じゅ", false},{"jyo", u"じょ", false},
  {"zya", u"じゃ", false},{"zyu", u"じゅ", false},{"zyo", u"じょ", false},
  {"cha", u"ちゃ", true}, {"chu", u"ちゅ", true}, {"cho", u"ちょ", true},
  {"tya", u"ちゃ", false},{"tyu", u"ちゅ", false},{"tyo", u"ちょ", false},
  {"cya", u"ちゃ", false},{"cyu", u"ちゅ", false},{"cyo", u"ちょ", false},
  {"dya", u"ぢゃ", true}, {"dyu", u"ぢゅ", true}, {"dyo", u"ぢょ", true},
  {"nya", u"にゃ", true}, {"nyu", u"にゅ", true}, {"nyo", u"にょ", true},
  {"hya", u"ひゃ", true}, {"hyu", u"ひゅ", true}, {"hyo", u"ひょ", true},
  {"bya", u"びゃ", true}, {"byu", u"びゅ", true}, {"byo", u"びょ", true},
  {"pya", u"ぴゃ", true}, {"pyu", u"ぴゅ", true}, {"pyo", u"ぴょ", true},
  {"mya", u"みゃ", true}, {"myu", u"みゅ", true}, {"myo", u"みょ", true},
  {"rya", u"りゃ", true}, {"ryu", u"りゅ", true}, {"ryo", u"りょ", true},

  // --- 外来音 ---
  {"fa", u"ふぁ", true}, {"fi", u"ふぃ", true},
  {"fe", u"ふぇ", true}, {"fo", u"ふぉ", true},
  {"va", u"ゔぁ", true}, {"vi", u"ゔぃ", true}, {"vu", u"ゔ", true},
  {"ve", u"ゔぇ", true}, {"vo", u"ゔぉ", true},
  {"she", u"しぇ", true}, {"je", u"じぇ", true}, {"che", u"ちぇ", true},
  {"tsa", u"つぁ", true}, {"tsi", u"つぃ", true},
  {"tse", u"つぇ", true}, {"tso", u"つぉ", true},
  {"wi", u"うぃ", true}, {"we", u"うぇ", true}, {"ye", u"いぇ", true},

  // --- 小書き（l/x） ---
  {"la", u"ぁ", false}, {"li", u"ぃ", false}, {"lu", u"ぅ", false},
  {"le", u"ぇ", false}, {"lo", u"ぉ", false},
  {"xa", u"ぁ", true},  {"xi", u"ぃ", true},  {"xu", u"ぅ", true},
  {"xe", u"ぇ", true},  {"xo", u"ぉ", true},
  {"lya", u"ゃ", false},{"lyu", u"ゅ", false},{"lyo", u"ょ", false},
  {"xya", u"ゃ", true}, {"xyu", u"ゅ", true}, {"xyo", u"ょ", true},
  {"ltu", u"っ", false},{"xtu", u"っ", true}, {"ltsu", u"っ", false},
  {"lwa", u"ゎ", false},{"xwa", u"ゎ", true},
};

const int kTableCount = (int)(sizeof(kTable) / sizeof(kTable[0]));

// 撥音「ん」と促音「っ」
const UTF16 kKanaN      = 0x3093;  // ん
const UTF16 kKanaSokuon = 0x3063;  // っ

int Utf16Len(const char16_t* s) {
  int n = 0;
  while (s[n]) { n++; }
  return n;
}

bool IsVowelChar(UTF16 c) {
  return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o';
}

// src[0..len) が roma と前方一致するか
bool MatchRoma(const UTF16* src, int len, const char* roma, int romaLen) {
  if (romaLen > len) {
    return false;
  }
  for (int i = 0; i < romaLen; i++) {
    if (src[i] != (UTF16)(unsigned char)roma[i]) {
      return false;
    }
  }
  return true;
}

int StrLen(const char* s) {
  int n = 0;
  while (s[n]) { n++; }
  return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// ローマ字 → かな（前方一致・最長一致）
// ---------------------------------------------------------------------------
int LookupRoma(const UTF16* src, int len, UTF16* out, int* outlen) {
  *outlen = 0;
  if (len <= 0) {
    return 0;
  }

  // --- 「ん」の判定（na/ni/nya… より先に処理する） ---
  if (src[0] == 'n') {
    if (len >= 2) {
      // nn / n' は確定で「ん」
      if (src[1] == 'n' || src[1] == '\'') {
        out[0] = kKanaN;
        *outlen = 1;
        return 2;
      }
      // n + 子音（母音でも y でもない英小文字）は「ん」1文字消費
      if (src[1] >= 'a' && src[1] <= 'z' && !IsVowelChar(src[1]) && src[1] != 'y') {
        out[0] = kKanaN;
        *outlen = 1;
        return 1;
      }
    }
    // len == 1（続き待ち）や na/ni/nya… はテーブルへ落とす
  }

  // --- 促音（同じ子音の連打） ---
  if (len >= 2 && src[0] >= 'a' && src[0] <= 'z' &&
      !IsVowelChar(src[0]) && src[0] != 'n' && src[1] == src[0]) {
    out[0] = kKanaSokuon;
    *outlen = 1;
    return 1;  // 1文字だけ消費して、残りは次の走査で解決させる
  }

  // --- テーブル最長一致 ---
  int bestLen  = 0;
  const RomaEntry* best = 0;
  for (int i = 0; i < kTableCount; i++) {
    int rl = StrLen(kTable[i].roma);
    if (rl <= bestLen) {
      continue;   // すでに見つけた候補より短いので見る必要なし
    }
    if (MatchRoma(src, len, kTable[i].roma, rl)) {
      best    = &kTable[i];
      bestLen = rl;
    }
  }
  if (!best) {
    return 0;   // 続き待ち or 変換対象外
  }

  int kl = Utf16Len(best->kana);
  for (int i = 0; i < kl; i++) {
    out[i] = (UTF16)best->kana[i];
  }
  *outlen = kl;
  ASSERT(kl <= bestLen);   // in-place 変換の前提
  return bestLen;
}

// ---------------------------------------------------------------------------
// かな → ローマ字（逆変換）
// ---------------------------------------------------------------------------
int KanaStringToRoma(const UTF16* src, int len, UTF16* out, int maxout) {
  int n = 0;
  bool sokuon = false;   // 直前に「っ」があった

  // 出力ヘルパ
  auto put = [&](UTF16 c) {
    if (n < maxout) { out[n++] = c; }
  };
  auto putRoma = [&](const char* s) {
    for (int i = 0; s[i]; i++) { put((UTF16)(unsigned char)s[i]); }
  };

  for (int i = 0; i < len; ) {
    UTF16 c = src[i];

    // 促音は次のかなを見てから決める
    if (c == kKanaSokuon) {
      if (sokuon) { putRoma("xtu"); }   // 「っっ」
      sokuon = true;
      i++;
      continue;
    }

    // 撥音は一意に "nn"
    if (c == kKanaN) {
      if (sokuon) { putRoma("xtu"); sokuon = false; }
      putRoma("nn");
      i++;
      continue;
    }

    // 拗音（かな2文字）を優先し、なければ1文字で引く
    const RomaEntry* hit = 0;
    int hitKana = 0;
    for (int want = 2; want >= 1 && !hit; want--) {
      if (i + want > len) {
        continue;
      }
      for (int k = 0; k < kTableCount; k++) {
        if (!kTable[k].canonical) {
          continue;
        }
        if (Utf16Len(kTable[k].kana) != want) {
          continue;
        }
        bool same = true;
        for (int j = 0; j < want; j++) {
          if ((UTF16)kTable[k].kana[j] != src[i + j]) { same = false; break; }
        }
        if (same) { hit = &kTable[k]; hitKana = want; break; }
      }
    }

    if (hit) {
      if (sokuon) {
        // 子音始まりなら重ねる。母音始まりなら重ねられないので "xtu"。
        UTF16 head = (UTF16)(unsigned char)hit->roma[0];
        if (IsVowelChar(head)) { putRoma("xtu"); } else { put(head); }
        sokuon = false;
      }
      putRoma(hit->roma);
      i += hitKana;
      continue;
    }

    // かな以外（ASCII・記号・全角英数など）はそのまま通す
    if (sokuon) { putRoma("xtu"); sokuon = false; }
    put(c);
    i++;
  }

  if (sokuon) { putRoma("xtu"); }   // 末尾の「っ」
  return n;
}
