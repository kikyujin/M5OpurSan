// test_dict.c — system.dic の辞書引きテスト（Mac 上）
//
//   ./test_dict           テスト 9 項目を自動実行
//   ./test_dict -i        対話モード
//
// 辞書アクセス層そのものは opur_dict.h / opur_dict.c に切り出した。
// ここに残るのはテストと対話モードだけ。

#include "opur_dict.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define HEADER_SIZE OPUR_DICT_HEADER_SIZE
#define FIELD_MAX   OPUR_DICT_FIELD_MAX

// 切り出し前の呼び名でテスト本体から使えるようにする薄いラッパ。
static long entry_offset(OpurDict *d, uint32_t i) {
    return opur_dict_entry_offset(d, i);
}

static long read_record(OpurDict *d, long pos,
                        char *rbuf, size_t *rlen, char *sbuf, size_t *slen) {
    return opur_dict_read_record(d, pos, rbuf, rlen, sbuf, slen);
}

// 計測値はライブラリ側が持っているので、都度読み出す。
static long stat_seeks(void) {
    OpurDictStats s;
    opur_dict_get_stats(&s);
    return s.seeks;
}

// ---------------------------------------------------------------------------
// UTF-8 ユーティリティ（実体は opur_dict.c）
// ---------------------------------------------------------------------------

static int u8_count(const char *s) { return opur_u8_count(s); }

// 先頭から nchars 文字ぶんのバイト数
static size_t u8_bytes(const char *s, int nchars) { return opur_u8_bytes(s, nchars); }

// ---------------------------------------------------------------------------
// テスト用のヒット収集
// ---------------------------------------------------------------------------

#define MAX_HITS 512

typedef struct {
    char surface[MAX_HITS][FIELD_MAX];
    int n;
} HitList;

static int collect(const char *reading, const char *surface, void *ctx) {
    HitList *h = (HitList *)ctx;
    (void)reading;
    if (h->n < MAX_HITS) {
        snprintf(h->surface[h->n], FIELD_MAX, "%s", surface);
        h->n++;
    }
    return 0;
}

static int has_surface(const HitList *h, const char *want) {
    int i;
    for (i = 0; i < h->n; i++) {
        if (strcmp(h->surface[i], want) == 0) return 1;
    }
    return 0;
}

static void print_some(const HitList *h, int max) {
    int i;
    for (i = 0; i < h->n && i < max; i++) {
        printf("%s%s", i ? " " : "", h->surface[i]);
    }
    if (h->n > max) printf(" …他 %d 件", h->n - max);
}

// ---------------------------------------------------------------------------
// テストフレームワーク
// ---------------------------------------------------------------------------

static int g_pass = 0, g_fail = 0;

static void ok(const char *title, int cond) {
    if (cond) g_pass++; else g_fail++;
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", title);
}

static void eq_int(const char *title, long got, long want) {
    int c = (got == want);
    if (c) g_pass++; else g_fail++;
    printf("  [%s] %-38s got=%ld  want=%ld\n", c ? "PASS" : "FAIL",
           title, got, want);
}

// ---------------------------------------------------------------------------
// 最長一致
// ---------------------------------------------------------------------------

// text の先頭から最長一致する読みを探す。見つかった文字数を返す（0 = なし）。
static int longest_match(OpurDict *d, const char *text, HitList *out) {
    int nchars = u8_count(text);
    int L;
    for (L = nchars; L >= 1; L--) {
        char cand[FIELD_MAX];
        size_t nb = u8_bytes(text, L);
        if (nb >= FIELD_MAX) continue;
        memcpy(cand, text, nb);
        cand[nb] = '\0';
        out->n = 0;
        if (opur_dict_search(d, cand, -1, collect, out) > 0) return L;
    }
    out->n = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// テスト本体
// ---------------------------------------------------------------------------

static void t1_open(OpurDict *d) {
    printf("\n=== 1. 辞書オープン ===\n");
    ok("magic / version が正しい", d->fp != NULL);
    printf("  entry_count   : %u\n", d->entry_count);
    printf("  table_offset  : %ld\n", d->table_offset);
    ok("entry_count が 10 万以上", d->entry_count >= 100000);
    ok("table_offset がヘッダより後ろ", d->table_offset > HEADER_SIZE);
}

static void t2_known_words(OpurDict *d) {
    HitList h;
    printf("\n=== 2. 既知の単語検索 ===\n");

    h.n = 0;
    opur_dict_search(d, "ある", -1, collect, &h);
    printf("  ある → "); print_some(&h, 8); printf("  (計 %d)\n", h.n);
    ok("「ある」に 有る がある", has_surface(&h, "有る"));
    ok("「ある」に 在る がある", has_surface(&h, "在る"));

    h.n = 0;
    opur_dict_search(d, "にほんご", -1, collect, &h);
    printf("  にほんご → "); print_some(&h, 8); printf("  (計 %d)\n", h.n);
    ok("「にほんご」に 日本語 がある", has_surface(&h, "日本語"));

    h.n = 0;
    opur_dict_search(d, "きょう", -1, collect, &h);
    printf("  きょう → "); print_some(&h, 8); printf("  (計 %d)\n", h.n);
    ok("「きょう」に 今日 がある", has_surface(&h, "今日"));
}

static void t3_conjugation(OpurDict *d) {
    HitList h;
    printf("\n=== 3. 活用形検索 ===\n");

    h.n = 0;
    opur_dict_search(d, "たべる", -1, collect, &h);
    printf("  たべる → "); print_some(&h, 8); printf("  (計 %d)\n", h.n);
    ok("一段: 「たべる」に 食べる がある", has_surface(&h, "食べる"));

    h.n = 0;
    opur_dict_search(d, "たべ", -1, collect, &h);
    ok("一段: 「たべ」に 食べ がある（連用形）", has_surface(&h, "食べ"));

    // 五段・カ行イ音便の連用タ接続。辞書に入るのは語幹側の「かい → 書い」で、
    // 「た」は助動詞として別エントリ（最長一致で かい + た に分かれる）。
    h.n = 0;
    opur_dict_search(d, "かい", -1, collect, &h);
    printf("  かい → "); print_some(&h, 8); printf("  (計 %d)\n", h.n);
    ok("五段イ音便: 「かい」に 書い がある", has_surface(&h, "書い"));

    h.n = 0;
    opur_dict_search(d, "かか", -1, collect, &h);
    ok("五段: 「かか」に 書か がある（未然形）", has_surface(&h, "書か"));

    h.n = 0;
    opur_dict_search(d, "かけ", -1, collect, &h);
    ok("五段: 「かけ」に 書け がある（仮定・命令）", has_surface(&h, "書け"));

    // ipadic で「勉強」はサ変接続の名詞であって動詞ではない。サ変動詞として
    // 活用型を持つのは「察する」など（活用型 サ変・−スル）のほう。
    h.n = 0;
    opur_dict_search(d, "さっし", -1, collect, &h);
    printf("  さっし → "); print_some(&h, 8); printf("  (計 %d)\n", h.n);
    ok("サ変: 「さっし」に 察し がある", has_surface(&h, "察し"));

    h.n = 0;
    opur_dict_search(d, "えんじ", -1, collect, &h);
    ok("サ変−ズル: 「えんじ」に 演じ がある", has_surface(&h, "演じ"));

    h.n = 0;
    opur_dict_search(d, "し", -1, collect, &h);
    ok("サ変: 「し」に する の連用形 し がある", has_surface(&h, "し"));

    h.n = 0;
    opur_dict_search(d, "こい", -1, collect, &h);
    ok("カ変: 「こい」に 来い がある", has_surface(&h, "来い"));

    h.n = 0;
    opur_dict_search(d, "たかかっ", -1, collect, &h);
    ok("形容詞: 「たかかっ」に 高かっ がある", has_surface(&h, "高かっ"));
}

static void t4_tankan(OpurDict *d) {
    HitList h;
    printf("\n=== 4. 単漢字検索 ===\n");

    h.n = 0;
    opur_dict_search(d, "あ", -1, collect, &h);
    printf("  あ → "); print_some(&h, 12); printf("\n");
    printf("  候補数: %d\n", h.n);
    ok("「あ」に 亜 がある", has_surface(&h, "亜"));
    ok("「あ」に 阿 がある", has_surface(&h, "阿"));
    ok("「あ」の候補が 20 件以上", h.n >= 20);

    h.n = 0;
    opur_dict_search(d, "こう", -1, collect, &h);
    printf("  こう の候補数: %d\n", h.n);
    ok("「こう」の候補が 100 件以上", h.n >= 100);
}

static void t5_longest(OpurDict *d) {
    const char *text = "きょうはいいてんきです";
    char rest[512];
    int guard = 0;

    printf("\n=== 5. 最長一致テスト ===\n");
    printf("  入力: %s\n", text);

    snprintf(rest, sizeof(rest), "%s", text);
    while (rest[0] && guard++ < 32) {
        HitList h;
        int n = longest_match(d, rest, &h);
        size_t nb;
        if (n == 0) {
            nb = u8_bytes(rest, 1);
            printf("    %.*s → (なし)\n", (int)nb, rest);
        } else {
            nb = u8_bytes(rest, n);
            printf("    %.*s → ", (int)nb, rest);
            print_some(&h, 5);
            printf("\n");
        }
        memmove(rest, rest + nb, strlen(rest + nb) + 1);
    }
    ok("最長一致が最後まで進んだ", rest[0] == '\0');
}

static void t6_hint(OpurDict *d) {
    HitList a, b;
    long p1, p2;
    int n_a, n_b, i, same;

    printf("\n=== 6. 前回位置再利用 ===\n");
    printf("  （既定では使わない経路。ここでは正しさだけを検証する）\n");

    // 「あ」→ 位置を保存
    a.n = 0;
    opur_dict_search(d, "あ", -1, collect, &a);
    p1 = d->last_pos;
    printf("  「あ」の last_pos = %ld\n", p1);

    // 「あい」を hint あり / なしで引いて一致するか
    a.n = 0; n_a = opur_dict_search(d, "あい", p1, collect, &a);
    b.n = 0; n_b = opur_dict_search(d, "あい", -1, collect, &b);
    eq_int("「あい」 hint あり/なしのヒット数", n_a, n_b);
    same = (a.n == b.n);
    for (i = 0; same && i < a.n; i++) {
        if (strcmp(a.surface[i], b.surface[i]) != 0) same = 0;
    }
    ok("「あい」 hint あり/なしの候補が同一", same);

    // 「あい」→「あいう」と伸ばす
    a.n = 0; opur_dict_search(d, "あい", -1, collect, &a);
    p2 = d->last_pos;
    a.n = 0; n_a = opur_dict_search(d, "あいう", p2, collect, &a);
    b.n = 0; n_b = opur_dict_search(d, "あいう", -1, collect, &b);
    eq_int("「あいう」 hint あり/なしのヒット数", n_a, n_b);
    same = (a.n == b.n);
    for (i = 0; same && i < a.n; i++) {
        if (strcmp(a.surface[i], b.surface[i]) != 0) same = 0;
    }
    ok("「あいう」 hint あり/なしの候補が同一", same);
    printf("  あいう → "); print_some(&a, 6); printf("  (計 %d)\n", a.n);

    // 前方一致でない hint を渡してもフォールバックして正しく引ける
    a.n = 0; n_a = opur_dict_search(d, "とうきょう", p1, collect, &a);
    b.n = 0; n_b = opur_dict_search(d, "とうきょう", -1, collect, &b);
    eq_int("無関係な hint でもフォールバックする", n_a, n_b);
    ok("「とうきょう」に 東京 がある", has_surface(&b, "東京"));

    // 存在しない読みを hint 経由で引いても 0 件になる
    a.n = 0;
    n_a = opur_dict_search(d, "あzzz", p1, collect, &a);
    eq_int("hint 経由でも存在しない読みは 0 件", n_a, 0);

    // --- 網羅チェック: 辞書全体から等間隔にサンプリングして hint あり/なしを比較 ---
    {
        const int kSamples = 300;
        uint32_t step = d->entry_count / (uint32_t)kSamples;
        uint32_t k;
        int mismatch = 0, checked = 0, hint_used = 0;

        for (k = 0; k < d->entry_count && checked < kSamples; k += step) {
            char rbuf[FIELD_MAX], pbuf[FIELD_MAX];
            size_t rlen;
            long pos = entry_offset(d, k);
            long hp;
            size_t nb;
            int with, without;

            if (pos < 0) break;
            if (read_record(d, pos, rbuf, &rlen, NULL, NULL) < 0) break;
            if (u8_count(rbuf) < 2) continue;   // 伸長できない読みは飛ばす

            // 先頭 1 文字で引いた位置を hint にして、元の読みを引く
            nb = u8_bytes(rbuf, 1);
            memcpy(pbuf, rbuf, nb);
            pbuf[nb] = '\0';

            a.n = 0;
            if (opur_dict_search(d, pbuf, -1, collect, &a) <= 0) continue;
            hp = d->last_pos;

            a.n = 0; with    = opur_dict_search(d, rbuf, hp, collect, &a);
            b.n = 0; without = opur_dict_search(d, rbuf, -1, collect, &b);

            checked++;
            if (hp >= 0) hint_used++;
            if (with != without || a.n != b.n) { mismatch++; continue; }
            for (i = 0; i < a.n; i++) {
                if (strcmp(a.surface[i], b.surface[i]) != 0) { mismatch++; break; }
            }
        }
        printf("  サンプリング検証: %d 件（うち hint 使用 %d 件）\n",
               checked, hint_used);
        ok("全サンプルで hint あり/なしの結果が一致", mismatch == 0);
        ok("サンプルが十分な件数ある", checked >= 200);
    }
}

static void t7_missing(OpurDict *d) {
    HitList h;
    printf("\n=== 7. 存在しない読み ===\n");

    h.n = 0;
    eq_int("zzz", opur_dict_search(d, "zzz", -1, collect, &h), 0);
    h.n = 0;
    eq_int("ぬんぬんぬん", opur_dict_search(d, "ぬんぬんぬん", -1, collect, &h), 0);
    ok("not found のとき last_result = -1", d->last_result == -1);
}

static void t8_edges(OpurDict *d) {
    char rbuf[FIELD_MAX], sbuf[FIELD_MAX];
    size_t rlen, slen;
    long pos;
    HitList h;

    printf("\n=== 8. 先頭・末尾エントリ ===\n");

    pos = entry_offset(d, 0);
    ok("先頭レコードが読める", read_record(d, pos, rbuf, &rlen, sbuf, &slen) > 0);
    printf("  先頭: %s → %s\n", rbuf, sbuf);
    h.n = 0;
    ok("先頭エントリを検索で引ける",
       opur_dict_search(d, rbuf, -1, collect, &h) > 0 && has_surface(&h, sbuf));

    pos = entry_offset(d, d->entry_count - 1);
    ok("末尾レコードが読める", read_record(d, pos, rbuf, &rlen, sbuf, &slen) > 0);
    printf("  末尾: %s → %s\n", rbuf, sbuf);
    h.n = 0;
    ok("末尾エントリを検索で引ける",
       opur_dict_search(d, rbuf, -1, collect, &h) > 0 && has_surface(&h, sbuf));
}

// 実際の FEP の使い方（1 文字ずつ伸ばし、hint は直前の読みの位置）でのコスト。
// サンプリング検証で使った「hint = 先頭 1 文字」は最悪ケースにあたる。
static void bench_incremental(OpurDict *d) {
    const int kSamples = 300;
    uint32_t step = d->entry_count / (uint32_t)kSamples;
    uint32_t k;
    int checked = 0;
    long hint_seeks = 0, bs_seeks = 0;
    HitList h;

    printf("\n  --- 伸長入力の実測（hint = 直前の読み = 1 文字短い）---\n");

    for (k = 0; k < d->entry_count && checked < kSamples; k += step) {
        char rbuf[FIELD_MAX], pbuf[FIELD_MAX];
        size_t rlen, nb;
        long pos = entry_offset(d, k);
        long hp, s0;
        int nchars;

        if (pos < 0) break;
        if (read_record(d, pos, rbuf, &rlen, NULL, NULL) < 0) break;
        nchars = u8_count(rbuf);
        if (nchars < 2) continue;

        nb = u8_bytes(rbuf, nchars - 1);      // 末尾 1 文字を落とした読み
        memcpy(pbuf, rbuf, nb);
        pbuf[nb] = '\0';

        h.n = 0;
        if (opur_dict_search(d, pbuf, -1, collect, &h) <= 0) continue;
        hp = d->last_pos;

        s0 = stat_seeks();
        h.n = 0; opur_dict_search(d, rbuf, hp, collect, &h);
        hint_seeks += stat_seeks() - s0;

        s0 = stat_seeks();
        h.n = 0; opur_dict_search(d, rbuf, -1, collect, &h);
        bs_seeks += stat_seeks() - s0;

        checked++;
    }

    printf("    サンプル数        : %d\n", checked);
    if (checked > 0) {
        printf("    hint あり         : 平均 %.1f シーク\n",
               (double)hint_seeks / checked);
        printf("    hint なし         : 平均 %.1f シーク\n",
               (double)bs_seeks / checked);
        printf("\n");
        if (hint_seeks >= bs_seeks) {
            printf("    ★ hint 経路のほうが高い。同じ前方一致を持つ読みが\n");
            printf("       数百件ある帯を線形に走査するため。バイナリサーチは\n");
            printf("       log2(%u) ≒ %d ステップで済む。\n",
                   d->entry_count, (int)(31 - __builtin_clz(d->entry_count)));
            printf("       → hint 再利用は現状の辞書構成では効果がない。\n");
            printf("          判断はマスター／エルマーに委ねる（発注書の項目6は\n");
            printf("          「結果が一致すること」までを要求しており、それは PASS）。\n");
        }
    }
}

static OpurDict *g_dict = NULL;

static void t9_stats(void) {
    OpurDictStats s;
    opur_dict_get_stats(&s);

    printf("\n=== 9. 統計 ===\n");
    printf("  検索回数            : %ld\n", s.searches);
    printf("  総シーク回数        : %ld\n", s.seeks);
    printf("  平均シーク回数      : %.1f / 検索\n",
           s.searches ? (double)s.seeks / (double)s.searches : 0.0);
    printf("\n  経路別:\n");
    printf("    バイナリサーチ    : %ld 回, 平均 %.1f シーク\n", s.bs_searches,
           s.bs_searches ? (double)s.bs_seeks / (double)s.bs_searches : 0.0);
    printf("    hint 線形走査     : %ld 回, 平均 %.1f シーク\n", s.hint_searches,
           s.hint_searches ? (double)s.hint_seeks / (double)s.hint_searches : 0.0);
    printf("\n  ※ hint 経路は「前回位置から前方一致が切れるまで線形走査」する。\n");
    printf("     上の数字は hint = 先頭 1 文字（最悪ケース）のもの。\n");
    printf("     実際の FEP の使い方での実測は下記。\n");
    ok("検索が実行されている", s.searches > 0);
    bench_incremental(g_dict);
    ok("バイナリサーチは O(log N) に収まる",
       s.bs_searches == 0 || (double)s.bs_seeks / (double)s.bs_searches < 100.0);
}

// ---------------------------------------------------------------------------
// 対話モード
// ---------------------------------------------------------------------------

static int print_hit(const char *reading, const char *surface, void *ctx) {
    int *n = (int *)ctx;
    (void)reading;
    printf("[%d] %s\n", ++(*n), surface);
    return (*n >= 30) ? 1 : 0;   // 表示は 30 件まで
}

static void interactive(OpurDict *d) {
    char line[512];

    printf("辞書引き対話モード（quit で終了）\n");
    for (;;) {
        char *nl;
        int n = 0;

        printf("dict> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line[0] == '\0') continue;
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) break;

        if (opur_dict_search(d, line, -1, print_hit, &n) > 0) continue;

        // 完全一致しなければ最長一致で分割する
        {
            char rest[512];
            int guard = 0;
            snprintf(rest, sizeof(rest), "%s", line);
            while (rest[0] && guard++ < 64) {
                HitList h;
                int m = longest_match(d, rest, &h);
                size_t nb;
                if (m == 0) {
                    nb = u8_bytes(rest, 1);
                    printf("最長一致: %.*s → (なし)\n", (int)nb, rest);
                } else {
                    nb = u8_bytes(rest, m);
                    printf("最長一致: %.*s → [", (int)nb, rest);
                    print_some(&h, 8);
                    printf("]\n");
                }
                memmove(rest, rest + nb, strlen(rest + nb) + 1);
                if (rest[0]) printf("残り: %s\n", rest);
            }
        }
    }
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    OpurDict d;
    const char *path = "../dict/output/system.dic";
    int rc;

    if (argc >= 3 && strcmp(argv[1], "-d") == 0) path = argv[2];

    rc = opur_dict_open(&d, path);
    if (rc != 0) {
        fprintf(stderr, "辞書を開けない: %s (rc=%d)\n", path, rc);
        fprintf(stderr, "  ../dict/ で make build-dict を先に実行して\n");
        return 1;
    }

    if (argc >= 2 && strcmp(argv[1], "-i") == 0) {
        interactive(&d);
        opur_dict_close(&d);
        return 0;
    }

    printf("=== system.dic 辞書引きテスト ===\n");
    printf("辞書: %s\n", path);

    t1_open(&d);
    t2_known_words(&d);
    t3_conjugation(&d);
    t4_tankan(&d);
    t5_longest(&d);
    t6_hint(&d);
    t7_missing(&d);
    t8_edges(&d);
    g_dict = &d;
    t9_stats();

    printf("\n---- %d passed, %d failed ----\n", g_pass, g_fail);

    opur_dict_close(&d);
    return (g_fail == 0) ? 0 : 1;
}
