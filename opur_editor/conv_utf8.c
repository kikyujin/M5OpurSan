// conv_utf8.c — 文字種変換（C11 / UTF-8）
//
// テーブルは fep/roma_table.cpp の canonical=true エントリと 1:1 対応。
// 向こうを触ったらこちらも合わせること（Phase D 以降で片方に寄せる予定）。

#include "conv_utf8.h"
#include "utf8_utf16.h"

#include <string.h>

// 逆変換の一時バッファ。かな 1 文字が最大 3 バイトのローマ字に化けるので、
// UTF-8 のかな（3 バイト）に対しては増えない。余裕をみて 512。
#define CONV_TEMP_MAX 512

#define KANA_SOKUON 0x3063u  // っ
#define KANA_N      0x3093u  // ん

#define HIRAGANA_FIRST 0x3041u  // ぁ
#define HIRAGANA_LAST  0x3096u  // ゖ
#define KANA_OFFSET    0x0060u  // ひらがな → カタカナ

#define WIDE_OFFSET 0xFEE0u  // ASCII → 全角英数
#define WIDE_SPACE  0x3000u  // 全角スペース

// ---------------------------------------------------------------------------
// かな → ローマ字テーブル（canonical のみ）
// ---------------------------------------------------------------------------

typedef struct {
    const char *roma;
    const char *kana;   // UTF-8
} RomaEntry;

static const RomaEntry kTable[] = {
    // --- 母音 ---
    {"a", "あ"}, {"i", "い"}, {"u", "う"}, {"e", "え"}, {"o", "お"},

    // --- か行 ---
    {"ka", "か"}, {"ki", "き"}, {"ku", "く"}, {"ke", "け"}, {"ko", "こ"},
    {"ga", "が"}, {"gi", "ぎ"}, {"gu", "ぐ"}, {"ge", "げ"}, {"go", "ご"},

    // --- さ行 ---
    {"sa", "さ"}, {"shi", "し"}, {"su", "す"}, {"se", "せ"}, {"so", "そ"},
    {"za", "ざ"}, {"ji", "じ"},  {"zu", "ず"}, {"ze", "ぜ"}, {"zo", "ぞ"},

    // --- た行 ---
    {"ta", "た"}, {"chi", "ち"}, {"tsu", "つ"}, {"te", "て"}, {"to", "と"},
    {"da", "だ"}, {"di", "ぢ"},  {"du", "づ"},  {"de", "で"}, {"do", "ど"},

    // --- な行 ---
    {"na", "な"}, {"ni", "に"}, {"nu", "ぬ"}, {"ne", "ね"}, {"no", "の"},

    // --- は行 ---
    {"ha", "は"}, {"hi", "ひ"}, {"fu", "ふ"}, {"he", "へ"}, {"ho", "ほ"},
    {"ba", "ば"}, {"bi", "び"}, {"bu", "ぶ"}, {"be", "べ"}, {"bo", "ぼ"},
    {"pa", "ぱ"}, {"pi", "ぴ"}, {"pu", "ぷ"}, {"pe", "ぺ"}, {"po", "ぽ"},

    // --- ま行 ---
    {"ma", "ま"}, {"mi", "み"}, {"mu", "む"}, {"me", "め"}, {"mo", "も"},

    // --- や行 ---
    {"ya", "や"}, {"yu", "ゆ"}, {"yo", "よ"},

    // --- ら行 ---
    {"ra", "ら"}, {"ri", "り"}, {"ru", "る"}, {"re", "れ"}, {"ro", "ろ"},

    // --- わ行 ---
    {"wa", "わ"}, {"wo", "を"},

    // --- 拗音 ---
    {"kya", "きゃ"}, {"kyu", "きゅ"}, {"kyo", "きょ"},
    {"gya", "ぎゃ"}, {"gyu", "ぎゅ"}, {"gyo", "ぎょ"},
    {"sha", "しゃ"}, {"shu", "しゅ"}, {"sho", "しょ"},
    {"ja",  "じゃ"}, {"ju",  "じゅ"}, {"jo",  "じょ"},
    {"cha", "ちゃ"}, {"chu", "ちゅ"}, {"cho", "ちょ"},
    {"dya", "ぢゃ"}, {"dyu", "ぢゅ"}, {"dyo", "ぢょ"},
    {"nya", "にゃ"}, {"nyu", "にゅ"}, {"nyo", "にょ"},
    {"hya", "ひゃ"}, {"hyu", "ひゅ"}, {"hyo", "ひょ"},
    {"bya", "びゃ"}, {"byu", "びゅ"}, {"byo", "びょ"},
    {"pya", "ぴゃ"}, {"pyu", "ぴゅ"}, {"pyo", "ぴょ"},
    {"mya", "みゃ"}, {"myu", "みゅ"}, {"myo", "みょ"},
    {"rya", "りゃ"}, {"ryu", "りゅ"}, {"ryo", "りょ"},

    // --- 外来音 ---
    {"fa", "ふぁ"}, {"fi", "ふぃ"}, {"fe", "ふぇ"}, {"fo", "ふぉ"},
    {"va", "ゔぁ"}, {"vi", "ゔぃ"}, {"vu", "ゔ"},
    {"ve", "ゔぇ"}, {"vo", "ゔぉ"},
    {"she", "しぇ"}, {"je", "じぇ"}, {"che", "ちぇ"},
    {"tsa", "つぁ"}, {"tsi", "つぃ"}, {"tse", "つぇ"}, {"tso", "つぉ"},
    {"wi", "うぃ"}, {"we", "うぇ"}, {"ye", "いぇ"},

    // --- 小書き ---
    {"xa", "ぁ"}, {"xi", "ぃ"}, {"xu", "ぅ"}, {"xe", "ぇ"}, {"xo", "ぉ"},
    {"xya", "ゃ"}, {"xyu", "ゅ"}, {"xyo", "ょ"},
    {"xwa", "ゎ"},
    // 「っ」は促音として先に処理されるのでテーブルには来ない。
    // 原典（roma_table.cpp）と対応を崩さないために置いておく。
    {"xtu", "っ"},
};

static const int kTableCount = (int)(sizeof(kTable) / sizeof(kTable[0]));

// ---------------------------------------------------------------------------
// 出力ヘルパ（常に NUL 終端し、文字境界で打ち切る）
// ---------------------------------------------------------------------------

typedef struct {
    char *buf;
    size_t size;   // NUL を含む
    size_t n;
} Sink;

static void sink_init(Sink *sk, char *buf, size_t size) {
    sk->buf = buf;
    sk->size = size;
    sk->n = 0;
    if (size > 0) buf[0] = '\0';
}

static void sink_put_str(Sink *sk, const char *s) {
    size_t len = strlen(s);
    if (sk->size == 0) return;
    if (sk->n + len + 1 > sk->size) return;   // 入らなければ丸ごと捨てる
    memcpy(sk->buf + sk->n, s, len);
    sk->n += len;
    sk->buf[sk->n] = '\0';
}

static void sink_put_cp(Sink *sk, uint32_t cp) {
    char tmp[3];
    int w = u8_encode(cp, tmp);
    if (sk->size == 0) return;
    if (sk->n + (size_t)w + 1 > sk->size) return;
    memcpy(sk->buf + sk->n, tmp, (size_t)w);
    sk->n += (size_t)w;
    sk->buf[sk->n] = '\0';
}

static int is_vowel(char c) {
    return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o';
}

// ---------------------------------------------------------------------------
// かな → ローマ字（逆変換）
// ---------------------------------------------------------------------------
//
// 原典 KanaStringToRoma と同じ手順:
//   促音は次のかなを見てから決める / 撥音は一意に "nn" /
//   拗音（かな 2 文字）を優先し、なければ 1 文字で引く /
//   かな以外はそのまま通す。

void conv_utf8_kana_to_roma(const char *src, char *dst, size_t dst_size) {
    Sink sk;
    int sokuon = 0;   // 直前に「っ」があった

    sink_init(&sk, dst, dst_size);

    while (*src) {
        uint32_t cp;
        int used = u8_decode(src, &cp);
        const RomaEntry *hit = NULL;
        size_t hit_bytes = 0;
        int i;

        if (used == 0) break;

        // 促音
        if (cp == KANA_SOKUON) {
            if (sokuon) sink_put_str(&sk, "xtu");   // 「っっ」
            sokuon = 1;
            src += used;
            continue;
        }

        // 撥音
        if (cp == KANA_N) {
            if (sokuon) { sink_put_str(&sk, "xtu"); sokuon = 0; }
            sink_put_str(&sk, "nn");
            src += used;
            continue;
        }

        // テーブル最長一致（UTF-8 のバイト前方一致。エントリは常に文字境界で
        // 終わるので、前方一致 = 文字単位の一致になる）
        for (i = 0; i < kTableCount; i++) {
            size_t kb = strlen(kTable[i].kana);
            if (kb <= hit_bytes) continue;
            if (strncmp(src, kTable[i].kana, kb) == 0) {
                hit = &kTable[i];
                hit_bytes = kb;
            }
        }

        if (hit) {
            if (sokuon) {
                // 子音始まりなら重ねる。母音始まりなら重ねられないので "xtu"。
                if (is_vowel(hit->roma[0])) {
                    sink_put_str(&sk, "xtu");
                } else {
                    sink_put_cp(&sk, (uint32_t)(unsigned char)hit->roma[0]);
                }
                sokuon = 0;
            }
            sink_put_str(&sk, hit->roma);
            src += hit_bytes;
            continue;
        }

        // かな以外（ASCII・記号・漢字・全角英数など）はそのまま通す
        if (sokuon) { sink_put_str(&sk, "xtu"); sokuon = 0; }
        sink_put_cp(&sk, cp);
        src += used;
    }

    if (sokuon) sink_put_str(&sk, "xtu");   // 末尾の「っ」
}

// ---------------------------------------------------------------------------
// 文字種変換
// ---------------------------------------------------------------------------

void conv_utf8_to_katakana(const char *src, char *dst, size_t dst_size) {
    Sink sk;
    sink_init(&sk, dst, dst_size);

    while (*src) {
        uint32_t cp;
        int used = u8_decode(src, &cp);
        if (used == 0) break;
        if (cp >= HIRAGANA_FIRST && cp <= HIRAGANA_LAST) cp += KANA_OFFSET;
        sink_put_cp(&sk, cp);
        src += used;
    }
}

void conv_utf8_to_fullwidth(const char *src, char *dst, size_t dst_size) {
    char roma[CONV_TEMP_MAX];
    const char *p = roma;
    Sink sk;

    conv_utf8_kana_to_roma(src, roma, sizeof(roma));
    sink_init(&sk, dst, dst_size);

    while (*p) {
        uint32_t cp;
        int used = u8_decode(p, &cp);
        if (used == 0) break;
        if (cp == ' ') {
            cp = WIDE_SPACE;
        } else if (cp >= 0x21u && cp <= 0x7Eu) {
            cp += WIDE_OFFSET;
        }
        sink_put_cp(&sk, cp);
        p += used;
    }
}

void conv_utf8_to_halfwidth(const char *src, char *dst, size_t dst_size) {
    char roma[CONV_TEMP_MAX];
    const char *p = roma;
    Sink sk;

    conv_utf8_kana_to_roma(src, roma, sizeof(roma));
    sink_init(&sk, dst, dst_size);

    while (*p) {
        uint32_t cp;
        int used = u8_decode(p, &cp);
        if (used == 0) break;
        if (cp == WIDE_SPACE) {
            cp = ' ';
        } else if (cp >= 0x21u + WIDE_OFFSET && cp <= 0x7Eu + WIDE_OFFSET) {
            cp -= WIDE_OFFSET;
        }
        sink_put_cp(&sk, cp);
        p += used;
    }
}
