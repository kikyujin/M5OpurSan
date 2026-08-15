// test_candidate_bar.c — candidate_bar / conv_utf8 / utf8_utf16 の自動テスト
//
//   ./test_candidate_bar            全テスト実行
//   ./test_candidate_bar -i         対話モード（printf 描画。curses 不要）
//   ./test_candidate_bar -d PATH    辞書のパスを指定
//
// curses をリンクしていないので、これが通れば candidate_bar.c の
// curses 非依存も同時に保証される。

#include "candidate_bar.h"
#include "conv_utf8.h"
#include "utf8_utf16.h"

#include <stdio.h>
#include <string.h>

#define DEFAULT_DICT "../dict/output/system.dic"

// CandBar は約 84KB あるのでスタックに置かない。
static CandBar g_bar;
static OpurDict g_dict;
static int g_have_dict = 0;

static const CandConv g_conv = {
    conv_utf8_to_katakana,
    conv_utf8_to_fullwidth,
    conv_utf8_to_halfwidth,
};

// ---------------------------------------------------------------------------
// テストフレームワーク（test_editor.c に合わせる）
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

static void eq_str(const char *title, const char *got, const char *want) {
    int c = (strcmp(got, want) == 0);
    if (c) g_pass++; else g_fail++;
    printf("  [%s] %-38s got=\"%s\"  want=\"%s\"\n", c ? "PASS" : "FAIL",
           title, got, want);
}

// ---------------------------------------------------------------------------
// 1. UTF-8 ⇔ UTF-16
// ---------------------------------------------------------------------------

static void t1_utf(void) {
    const char *src = "こんにちは、Hello 世界！";
    uint16_t u16[64];
    char back[256];
    int n;

    printf("\n=== 1. UTF-8 ⇔ UTF-16 ===\n");

    n = utf8_to_utf16(src, u16, 64);
    eq_int("文字数", n, 15);   // こんにちは、 6 + Hello 5 + 空白 1 + 世界 2 + ！ 1
    utf16_to_utf8(u16, n, back, sizeof(back));
    eq_str("往復して元に戻る", back, src);

    // ASCII のみ
    n = utf8_to_utf16("abc", u16, 64);
    eq_int("ASCII の文字数", n, 3);
    eq_int("a のコード", u16[0], 'a');

    // dst_max で打ち切る
    n = utf8_to_utf16(src, u16, 5);
    eq_int("dst_max で打ち切る", n, 5);

    // 出力バッファが足りないときは文字境界で止まる
    {
        uint16_t kana[3];
        char small[8];
        int m;
        utf8_to_utf16("あいう", kana, 3);
        m = utf16_to_utf8(kana, 3, small, 7);   // 「あい」= 6 バイト + NUL
        eq_int("境界で打ち切る（バイト数）", m, 6);
        eq_str("境界で打ち切る（内容）", small, "あい");
    }

    // コードポイント単位
    {
        uint32_t cp;
        int used = u8_decode("あ", &cp);
        eq_int("u8_decode の消費バイト数", used, 3);
        eq_int("u8_decode のコードポイント", (long)cp, 0x3042);
    }

    // 不正なバイト列は U+FFFD にして 1 バイト進む（脱落しない）
    {
        uint32_t cp;
        int used = u8_decode("\xC0\xAF", &cp);
        eq_int("不正バイトの消費数", used, 1);
        eq_int("不正バイトは U+FFFD", (long)cp, 0xFFFD);
    }

    // BMP 外はサロゲート非対応なので U+FFFD になる
    {
        uint16_t one[4];
        int m = utf8_to_utf16("\xF0\x9F\xA6\x8A", one, 4);   // 🦊
        eq_int("BMP 外は 1 要素", m, 1);
        eq_int("BMP 外は U+FFFD", one[0], 0xFFFD);
    }
}

// ---------------------------------------------------------------------------
// 2. 文字種変換
// ---------------------------------------------------------------------------

static void t2_conv(void) {
    char buf[CAND_TEXT_MAX];

    printf("\n=== 2. 文字種変換（conv_utf8）===\n");

    conv_utf8_to_katakana("きょう", buf, sizeof(buf));
    eq_str("カタカナ: きょう", buf, "キョウ");
    conv_utf8_to_katakana("がっこう", buf, sizeof(buf));
    eq_str("カタカナ: がっこう", buf, "ガッコウ");
    conv_utf8_to_katakana("あa亜", buf, sizeof(buf));
    eq_str("カタカナ: かな以外は素通し", buf, "アa亜");

    conv_utf8_kana_to_roma("きょう", buf, sizeof(buf));
    eq_str("ローマ字: 拗音", buf, "kyou");
    conv_utf8_kana_to_roma("がっこう", buf, sizeof(buf));
    eq_str("ローマ字: 促音", buf, "gakkou");
    conv_utf8_kana_to_roma("にっぽん", buf, sizeof(buf));
    eq_str("ローマ字: 促音+撥音", buf, "nipponn");
    conv_utf8_kana_to_roma("あった", buf, sizeof(buf));
    eq_str("ローマ字: 母音+促音", buf, "atta");
    conv_utf8_kana_to_roma("あっ", buf, sizeof(buf));
    eq_str("ローマ字: 末尾の促音", buf, "axtu");
    conv_utf8_kana_to_roma("しつじゃふぁゔ", buf, sizeof(buf));
    eq_str("ローマ字: 代表つづり", buf, "shitsujafavu");

    conv_utf8_to_fullwidth("あ", buf, sizeof(buf));
    eq_str("全角英字: あ", buf, "ａ");
    conv_utf8_to_fullwidth("きょう", buf, sizeof(buf));
    eq_str("全角英字: きょう", buf, "ｋｙｏｕ");

    conv_utf8_to_halfwidth("あ", buf, sizeof(buf));
    eq_str("半角英字: あ", buf, "a");
    conv_utf8_to_halfwidth("きょう", buf, sizeof(buf));
    eq_str("半角英字: きょう", buf, "kyou");
}

// ---------------------------------------------------------------------------
// 合成候補リスト（辞書に依存しないテスト用）
// ---------------------------------------------------------------------------

static const char *kFake[] = { "あ", "亜", "阿", "唖", "娃", "痾", "婀" };
#define FAKE_N ((int)(sizeof(kFake) / sizeof(kFake[0])))

static void fake_setup(CandBar *bar) {
    int i;
    cand_bar_init(bar, NULL, NULL);
    snprintf(bar->reading, CAND_READING_MAX, "%s", kFake[0]);
    for (i = 0; i < FAKE_N; i++) {
        snprintf(bar->item[i].text, CAND_TEXT_MAX, "%s", kFake[i]);
        bar->item[i].is_tankan = 1;
    }
    bar->count = FAKE_N;
    bar->dict_count = FAKE_N - 1;
    bar->sel = 0;
    bar->win_start = 0;
    bar->committed[0] = '\0';
}

// ---------------------------------------------------------------------------
// 3. スクロールと ◀▶
// ---------------------------------------------------------------------------

static void t3_scroll(void) {
    CandRender r;
    int i;

    printf("\n=== 3. スクロールと ◀▶ ===\n");
    printf("  （全角2幅の候補 7 件、番号 \"N:\" が 2 幅、区切り 1 幅）\n");

    fake_setup(&g_bar);
    cand_bar_render(&g_bar, &r);
    printf("  初期  : %s  (width=%d)\n", r.text, r.width);
    eq_int("初期の先頭候補", r.first, 0);
    eq_int("初期の末尾候補", r.last, 5);
    eq_int("初期の表示幅", r.width, CAND_VIEW_WIDTH);
    ok("左端外に候補なし → ◀ は出ない", r.has_left == 0);
    ok("右端外に候補あり → ▶ が出る", r.has_right == 1);
    eq_str("初期の描画内容", r.text, "1:あ 2:亜 3:阿 4:唖 5:娃 6:痾▶");
    eq_int("選択候補のカラム", r.sel_col, 0);
    eq_int("選択候補の幅", r.sel_width, 4);

    // 画面内で動くあいだはウィンドウが動かない
    for (i = 0; i < 5; i++) eq_int("右キー", cand_bar_key(&g_bar, CAND_KEY_RIGHT), CAND_UPDATED);
    cand_bar_render(&g_bar, &r);
    eq_int("画面内なら win_start 据え置き", r.first, 0);
    eq_int("選択が末尾に来た", cand_bar_selected(&g_bar), 5);

    // はみ出したら次候補が左端に来る
    eq_int("右キー（はみ出す）", cand_bar_key(&g_bar, CAND_KEY_RIGHT), CAND_UPDATED);
    cand_bar_render(&g_bar, &r);
    printf("  はみ出し後: %s  (width=%d)\n", r.text, r.width);
    eq_int("次候補が左端に来る", r.first, 6);
    ok("左端外に候補あり → ◀ が出る", r.has_left == 1);
    ok("右端外に候補なし → ▶ は出ない", r.has_right == 0);
    eq_str("はみ出し後の描画内容", r.text, "◀1:婀");

    // これ以上は進めない
    eq_int("末尾で右キーは無視", cand_bar_key(&g_bar, CAND_KEY_RIGHT), CAND_IGNORED);

    // 左キーで戻ると選択が左端に来る
    eq_int("左キー", cand_bar_key(&g_bar, CAND_KEY_LEFT), CAND_UPDATED);
    cand_bar_render(&g_bar, &r);
    printf("  左に戻る  : %s  (width=%d)\n", r.text, r.width);
    eq_int("戻った先が左端", r.first, 5);
    eq_int("末尾まで見える", r.last, 6);
    eq_str("左に戻った描画内容", r.text, "◀1:痾 2:婀");

    // スペースは右キーと同じ
    fake_setup(&g_bar);
    eq_int("スペースは右キー相当", cand_bar_key(&g_bar, ' '), CAND_UPDATED);
    eq_int("スペースで 1 つ進む", cand_bar_selected(&g_bar), 1);
    eq_int("左キーで先頭に戻る", cand_bar_key(&g_bar, CAND_KEY_LEFT), CAND_UPDATED);
    eq_int("先頭で左キーは無視", cand_bar_key(&g_bar, CAND_KEY_LEFT), CAND_IGNORED);
}

// ---------------------------------------------------------------------------
// 4. 数字キー / Enter / ESC
// ---------------------------------------------------------------------------

static void t4_keys(void) {
    CandRender r;

    printf("\n=== 4. 数字キー / Enter / ESC ===\n");

    // 数字キーは画面内の番号で直接確定する
    fake_setup(&g_bar);
    eq_int("数字キー '3' で確定", cand_bar_key(&g_bar, '3'), CAND_COMMITTED);
    eq_str("確定した文字列", cand_bar_committed(&g_bar), "阿");
    eq_int("選択も移動している", cand_bar_selected(&g_bar), 2);

    // 画面に出ていない番号は無視
    fake_setup(&g_bar);
    cand_bar_render(&g_bar, &r);
    eq_int("画面内は 6 件", r.last - r.first + 1, 6);
    eq_int("見えていない '7' は無視", cand_bar_key(&g_bar, '7'), CAND_IGNORED);
    eq_str("確定していない", cand_bar_committed(&g_bar), "");

    // スクロール後は番号が振り直される
    fake_setup(&g_bar);
    g_bar.sel = 6;
    g_bar.win_start = 6;
    eq_int("スクロール後の '1'", cand_bar_key(&g_bar, '1'), CAND_COMMITTED);
    eq_str("スクロール後に '1' が指す候補", cand_bar_committed(&g_bar), "婀");

    // Enter は現在選択中で確定
    fake_setup(&g_bar);
    cand_bar_key(&g_bar, CAND_KEY_RIGHT);
    cand_bar_key(&g_bar, CAND_KEY_RIGHT);
    eq_int("Enter で確定", cand_bar_key(&g_bar, CAND_KEY_ENTER), CAND_COMMITTED);
    eq_str("Enter が確定した文字列", cand_bar_committed(&g_bar), "阿");

    // ESC は取り消し
    fake_setup(&g_bar);
    eq_int("ESC は取り消し", cand_bar_key(&g_bar, CAND_KEY_ESC), CAND_CANCELLED);
    eq_str("取り消し後は空", cand_bar_committed(&g_bar), "");
}

// ---------------------------------------------------------------------------
// 5. 候補リスト構成（辞書あり）
// ---------------------------------------------------------------------------

static void print_items(const CandBar *bar, int max) {
    int i;
    for (i = 0; i < bar->count && i < max; i++) {
        printf("%s%s", i ? " " : "  ", cand_bar_text(bar, i));
    }
    if (bar->count > max) printf(" …他 %d 件", bar->count - max);
    printf("\n");
}

static int find_item(const CandBar *bar, const char *text) {
    int i;
    for (i = 0; i < bar->count; i++) {
        if (strcmp(cand_bar_text(bar, i), text) == 0) return i;
    }
    return -1;
}

static void t5_build(void) {
    char kata[CAND_TEXT_MAX], full[CAND_TEXT_MAX], half[CAND_TEXT_MAX];
    int n;

    printf("\n=== 5. 候補リスト構成（辞書あり）===\n");

    cand_bar_init(&g_bar, &g_dict, &g_conv);
    n = cand_bar_start(&g_bar, "きょう");
    printf("  きょう → %d 件\n", n);
    print_items(&g_bar, 10);

    ok("候補が組み立てられた", n > 4);
    eq_str("候補 0 はひらがな", cand_bar_text(&g_bar, 0), "きょう");
    ok("辞書候補に 今日 がある", find_item(&g_bar, "今日") > 0);

    conv_utf8_to_katakana("きょう", kata, sizeof(kata));
    conv_utf8_to_fullwidth("きょう", full, sizeof(full));
    conv_utf8_to_halfwidth("きょう", half, sizeof(half));
    eq_str("候補 N+1 はカタカナ", cand_bar_text(&g_bar, n - 3), kata);
    eq_str("候補 N+2 は全角英字", cand_bar_text(&g_bar, n - 2), full);
    eq_str("候補 N+3 は半角英字", cand_bar_text(&g_bar, n - 1), half);
    eq_int("辞書候補の数はつじつまが合う", g_bar.dict_count, n - 4);

    // 空の読みは受け付けない
    eq_int("空文字は -1", cand_bar_start(&g_bar, ""), -1);

    // 辞書にない読みでも文字種候補は出る
    n = cand_bar_start(&g_bar, "ぬんぬんぬん");
    eq_int("辞書ヒット 0 でも 4 件", n, 4);
    eq_int("辞書候補は 0 件", g_bar.dict_count, 0);
    eq_int("辞書候補が無ければ選択は 0（ひらがな）",
           cand_bar_selected(&g_bar), 0);

    // 初期選択は辞書の第 1 候補。ひらがなから始めない
    cand_bar_start(&g_bar, "きょう");
    eq_int("初期選択は 1（辞書の第 1 候補）", cand_bar_selected(&g_bar), 1);
    ok("初期選択はひらがなではない",
       strcmp(cand_bar_text(&g_bar, cand_bar_selected(&g_bar)), "きょう") != 0);

    // 初期選択が画面外に出ていない（ensure_visible が効いている）
    {
        CandRender r;
        cand_bar_render(&g_bar, &r);
        ok("初期選択が画面内にある",
           r.first <= cand_bar_selected(&g_bar) &&
           cand_bar_selected(&g_bar) <= r.last);
        ok("選択候補に幅がついている", r.sel_width > 0);
    }

    // 左キーでひらがなに戻れる（0 番が消えたわけではない）
    eq_int("左キーで 0 番へ", cand_bar_key(&g_bar, CAND_KEY_LEFT), CAND_UPDATED);
    eq_int("0 番はひらがなのまま", cand_bar_selected(&g_bar), 0);
    eq_str("0 番の中身", cand_bar_text(&g_bar, 0), "きょう");

    // 辞書に「キョウ」があるので、放っておくとカタカナ候補と二重に並ぶ
    cand_bar_start(&g_bar, "きょう");
    {
        int i, dup = 0;
        for (i = 0; i < g_bar.count; i++) {
            int j;
            for (j = i + 1; j < g_bar.count; j++) {
                if (strcmp(cand_bar_text(&g_bar, i),
                           cand_bar_text(&g_bar, j)) == 0) dup++;
            }
        }
        eq_int("同じ表記が二重に並ばない", dup, 0);
        eq_int("カタカナは 1 箇所だけ", find_item(&g_bar, "キョウ"),
               g_bar.count - 3);
    }
}

// ---------------------------------------------------------------------------
// 5b. 候補数の上限に当たったとき単漢字を先に捨てる
// ---------------------------------------------------------------------------

static void t5b_overflow(void) {
    // 「こう」は全 255 件中 245 件が単漢字。複数文字の候補は 10 件あるが、
    // そのうち「こう」はひらがな候補と、「コウ」はカタカナ候補と重複するので
    // 辞書候補として残るのは 8 件。しかも 5 件は辞書の 250 番目以降に出てくる。
    // 素直に先頭から詰めると CAND_MAX で切られて落ちてしまう。
    static const char *kWords[] = {
        "国府", "府中", "濃う", "乞う", "恋う", "請う", "丐う", "戀う"
    };
    int i, n, missing = 0, n_word = 0;

    printf("\n=== 5b. 候補数の上限（単漢字を先に捨てる）===\n");

    cand_bar_init(&g_bar, &g_dict, &g_conv);
    n = cand_bar_start(&g_bar, "こう");
    printf("  こう → %d 件（辞書候補 %d 件）\n", n, g_bar.dict_count);
    print_items(&g_bar, 12);

    ok("候補が上限近くまで埋まる", n >= CAND_MAX - 3);

    for (i = 1; i <= g_bar.dict_count; i++) {
        if (!cand_bar_is_tankan(&g_bar, i)) n_word++;
    }
    printf("  複数文字の辞書候補: %d 件\n", n_word);
    eq_int("複数文字候補が 8 件そろっている", n_word, 8);

    for (i = 0; i < (int)(sizeof(kWords) / sizeof(kWords[0])); i++) {
        if (find_item(&g_bar, kWords[i]) < 0) {
            printf("  欠落: %s\n", kWords[i]);
            missing++;
        }
    }
    ok("上限に当たっても複数文字候補が落ちない", missing == 0);
}

// ---------------------------------------------------------------------------
// 6. 単漢字の末尾送り
// ---------------------------------------------------------------------------

static void t6_tankan(void) {
    int i, seen_tankan = 0, violation = 0, n_tankan = 0, n_word = 0;
    int n;

    printf("\n=== 6. 単漢字の末尾送り ===\n");

    cand_bar_init(&g_bar, &g_dict, &g_conv);
    n = cand_bar_start(&g_bar, "こう");
    printf("  こう → %d 件（辞書候補 %d 件）\n", n, g_bar.dict_count);
    print_items(&g_bar, 12);

    for (i = 1; i <= g_bar.dict_count; i++) {
        int t = cand_bar_is_tankan(&g_bar, i);
        if (t) { seen_tankan = 1; n_tankan++; }
        else { n_word++; if (seen_tankan) violation = 1; }
    }
    printf("  複数文字 %d 件 → 単漢字 %d 件\n", n_word, n_tankan);
    ok("単漢字より前に複数文字候補がある", n_word > 0);
    ok("単漢字がある", n_tankan > 0);
    ok("単漢字は辞書候補の末尾にまとまっている", violation == 0);
}

// ---------------------------------------------------------------------------
// 7. LRU 昇格
// ---------------------------------------------------------------------------

static void t7_lru(void) {
    char picked[CAND_TEXT_MAX];
    int pick = -1, i, n;

    printf("\n=== 7. LRU 昇格 ===\n");

    cand_bar_init(&g_bar, &g_dict, &g_conv);
    cand_bar_start(&g_bar, "こう");

    // 先頭以外の「複数文字の辞書候補」を選ぶ（単漢字は末尾グループなので除く）
    for (i = 2; i <= g_bar.dict_count; i++) {
        if (!cand_bar_is_tankan(&g_bar, i)) { pick = i; break; }
    }
    ok("2 番目以降に複数文字の辞書候補がある", pick > 0);
    if (pick < 0) return;

    snprintf(picked, sizeof(picked), "%s", cand_bar_text(&g_bar, pick));
    printf("  1 回目: 候補 %d の「%s」を確定\n", pick, picked);
    g_bar.sel = pick;
    eq_int("Enter で確定", cand_bar_key(&g_bar, CAND_KEY_ENTER), CAND_COMMITTED);

    n = cand_bar_start(&g_bar, "こう");
    printf("  2 回目: ");
    print_items(&g_bar, 8);
    eq_str("前回選んだ候補が辞書候補の先頭に来る",
           cand_bar_text(&g_bar, 1), picked);
    ok("候補数は変わらない", n == g_bar.count);

    // さらに別の候補を確定すると、そちらが先頭・前回のが 2 番目になる
    {
        char second[CAND_TEXT_MAX];
        int p2 = -1;
        for (i = 2; i <= g_bar.dict_count; i++) {
            if (!cand_bar_is_tankan(&g_bar, i)) { p2 = i; break; }
        }
        if (p2 > 0) {
            snprintf(second, sizeof(second), "%s", cand_bar_text(&g_bar, p2));
            g_bar.sel = p2;
            cand_bar_key(&g_bar, CAND_KEY_ENTER);
            cand_bar_start(&g_bar, "こう");
            printf("  3 回目: ");
            print_items(&g_bar, 8);
            eq_str("最後に選んだものが先頭", cand_bar_text(&g_bar, 1), second);
            eq_str("その前に選んだものが 2 番目", cand_bar_text(&g_bar, 2), picked);
        }
    }

    // 別の読みは影響を受けない
    {
        CandBar *b = &g_bar;
        char before[CAND_TEXT_MAX];
        cand_bar_start(b, "にほんご");
        snprintf(before, sizeof(before), "%s", cand_bar_text(b, 1));
        cand_bar_start(b, "こう");
        cand_bar_start(b, "にほんご");
        eq_str("別の読みの並びは変わらない", cand_bar_text(b, 1), before);
    }

    // 文字種候補（カタカナ）を確定しても LRU には載らない
    {
        int cnt = cand_bar_start(&g_bar, "こう");
        char top[CAND_TEXT_MAX];
        snprintf(top, sizeof(top), "%s", cand_bar_text(&g_bar, 1));
        g_bar.sel = cnt - 3;                      // カタカナ
        cand_bar_key(&g_bar, CAND_KEY_ENTER);
        cand_bar_start(&g_bar, "こう");
        eq_str("文字種候補の確定は辞書候補の順に影響しない",
               cand_bar_text(&g_bar, 1), top);
    }
}

// ---------------------------------------------------------------------------
// 対話モード
// ---------------------------------------------------------------------------

static void draw(const CandBar *bar) {
    CandRender r;
    int i;

    cand_bar_render(bar, &r);
    printf("    +------------------------------+\n");
    printf("    |%s", r.text);
    for (i = r.width; i < CAND_VIEW_WIDTH; i++) printf(" ");
    printf("|\n");
    printf("    |");
    for (i = 0; i < r.sel_col; i++) printf(" ");
    for (i = 0; i < r.sel_width; i++) printf("^");
    for (i = r.sel_col + r.sel_width; i < CAND_VIEW_WIDTH; i++) printf(" ");
    printf("|\n");
    printf("    +------------------------------+\n");
    printf("    候補 %d/%d  表示 %d〜%d  幅 %d  %s%s\n",
           cand_bar_selected(bar) + 1, cand_bar_count(bar),
           r.first, r.last, r.width,
           r.has_left ? "◀" : "", r.has_right ? "▶" : "");
}

static void interactive(void) {
    char line[512];

    printf("変換候補バー対話モード\n");
    printf("  読みを入力 → 候補バーを表示\n");
    printf("  キー: '>' か ' ' = 次候補 / '<' = 前候補 / '0'-'9' = 直接確定\n");
    printf("        空行 = Enter で確定 / 'q' = 読み入力に戻る\n");
    printf("  quit で終了\n");

    for (;;) {
        char *nl;

        printf("\nreading> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line[0] == '\0') continue;
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) break;

        if (cand_bar_start(&g_bar, line) <= 0) {
            printf("  候補なし\n");
            continue;
        }
        draw(&g_bar);

        for (;;) {
            int done = 0, back = 0;
            char *p;

            printf("key> ");
            fflush(stdout);
            if (!fgets(line, sizeof(line), stdin)) return;
            nl = strchr(line, '\n');
            if (nl) *nl = '\0';

            if (line[0] == '\0') {
                cand_bar_key(&g_bar, CAND_KEY_ENTER);
                printf("  確定: 「%s」\n", cand_bar_committed(&g_bar));
                break;
            }

            for (p = line; *p && !done && !back; p++) {
                CandResult res;
                switch (*p) {
                case '>': case ' ': res = cand_bar_key(&g_bar, CAND_KEY_RIGHT); break;
                case '<':           res = cand_bar_key(&g_bar, CAND_KEY_LEFT);  break;
                case 'q':           back = 1; continue;
                default:            res = cand_bar_key(&g_bar, *p);             break;
                }
                if (res == CAND_COMMITTED) {
                    printf("  確定: 「%s」\n", cand_bar_committed(&g_bar));
                    done = 1;
                } else if (res == CAND_IGNORED) {
                    printf("  (無視: '%c')\n", *p);
                }
            }
            if (done || back) break;
            draw(&g_bar);
        }
    }
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    const char *path = DEFAULT_DICT;
    int want_interactive = 0;
    int i, rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0) want_interactive = 1;
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) path = argv[++i];
    }

    rc = opur_dict_open(&g_dict, path);
    g_have_dict = (rc == 0);

    if (want_interactive) {
        if (!g_have_dict) {
            fprintf(stderr, "辞書を開けない: %s (rc=%d)\n", path, rc);
            return 1;
        }
        cand_bar_init(&g_bar, &g_dict, &g_conv);
        interactive();
        opur_dict_close(&g_dict);
        return 0;
    }

    printf("=== 変換候補バーテスト ===\n");
    printf("辞書: %s%s\n", path, g_have_dict ? "" : "  (開けなかった)");

    t1_utf();
    t2_conv();
    t3_scroll();
    t4_keys();

    if (g_have_dict) {
        t5_build();
        t5b_overflow();
        t6_tankan();
        t7_lru();
    } else {
        printf("\n辞書が開けないので 5〜7 をスキップした。\n");
        printf("  ../dict/ で make build-dict を実行して\n");
        g_fail++;   // 素通ししたと誤解しないよう失敗扱いにする
    }

    printf("\n---- %d passed, %d failed ----\n", g_pass, g_fail);

    if (g_have_dict) opur_dict_close(&g_dict);
    return (g_fail == 0) ? 0 : 1;
}
