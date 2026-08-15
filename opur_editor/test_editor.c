// test_editor.c — editor.c の自動テスト（curses を使わない）
//
//   ./test_editor          全テスト実行
//   ./test_editor --dump   ついでに代表ケースの画面イメージを表示
//
// editor.o しかリンクしないので、これが通れば editor.c の curses 非依存も
// 同時に保証される（ncurses をリンクしていないのでシンボルが解決しない）。

#include "editor.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// UTF-16 / UTF-8 ユーティリティ（fep/test_fep.cpp の実装を C に移植）
// ---------------------------------------------------------------------------

static int utf16_to_utf8(const uint16_t* src, int len, char* out, int maxout) {
    int n = 0;
    int i;
    for (i = 0; i < len; i++) {
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

// UTF-8 → UTF-16（BMP のみ）。テストで日本語リテラルを直接書くため。
static int utf8_to_utf16(const char* s, uint16_t* out, int maxout) {
    int n = 0;
    const unsigned char* p = (const unsigned char*)s;
    while (*p && n < maxout) {
        unsigned int c = *p;
        if (c < 0x80) {
            out[n++] = (uint16_t)c;
            p += 1;
        } else if ((c & 0xE0) == 0xC0) {
            out[n++] = (uint16_t)(((c & 0x1F) << 6) | (p[1] & 0x3F));
            p += 2;
        } else {
            out[n++] = (uint16_t)(((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) |
                                  (p[2] & 0x3F));
            p += 3;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// テストフレームワーク
// ---------------------------------------------------------------------------

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char* title, int cond) {
    if (cond) g_pass++; else g_fail++;
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", title);
}

static void eq_int(const char* title, int got, int want) {
    int c = (got == want);
    if (c) g_pass++; else g_fail++;
    printf("  [%s] %-42s got=%d  want=%d\n", c ? "PASS" : "FAIL",
           title, got, want);
}

static void eq_str(const char* title, const char* got, const char* want) {
    int c = (strcmp(got, want) == 0);
    if (c) g_pass++; else g_fail++;
    printf("  [%s] %-42s got=\"%s\"  want=\"%s\"\n", c ? "PASS" : "FAIL",
           title, got, want);
}

// ---------------------------------------------------------------------------
// 入力ヘルパ
// ---------------------------------------------------------------------------

static void feed(OpurEditor* ed, const char* utf8) {
    uint16_t tmp[OPUR_BUF_MAX + 8];
    int n = utf8_to_utf16(utf8, tmp, OPUR_BUF_MAX + 8);
    int i;
    for (i = 0; i < n; i++) opur_insert(ed, tmp[i]);
}

static void feed_n(OpurEditor* ed, uint16_t ch, int count) {
    int i;
    for (i = 0; i < count; i++) opur_insert(ed, ch);
}

static void line_utf8(const OpurEditor* ed, const OpurLayout* lay, int li,
                      char* out, int maxout) {
    const OpurLine* ln = &lay->line[li];
    int content_end = ln->has_lf ? (ln->end - 1) : ln->end;
    utf16_to_utf8(&ed->buf[ln->start], content_end - ln->start, out, maxout);
}

// ---------------------------------------------------------------------------
// 不変条件チェック（全操作後に必ず成り立つべきこと）
// ---------------------------------------------------------------------------

static int g_inv_fail = 0;

static void inv(const char* where, const char* what, int cond) {
    if (!cond) {
        g_inv_fail++;
        if (g_inv_fail <= 10) {
            printf("  [INV ] %s: %s\n", where, what);
        }
    }
}

static void invariants(const OpurEditor* ed, const char* where) {
    OpurLayout lay;
    int i, line, col, max_top;

    inv(where, "0 <= len <= BUF_MAX", ed->len >= 0 && ed->len <= OPUR_BUF_MAX);
    inv(where, "0 <= cursor <= len", ed->cursor >= 0 && ed->cursor <= ed->len);
    inv(where, "goal_col >= -1", ed->goal_col >= -1);
    inv(where, "goal_col <= COLS", ed->goal_col <= OPUR_COLS);

    opur_layout(ed, &lay);

    inv(where, "line count >= 1", lay.count >= 1);
    inv(where, "line[0].start == 0", lay.line[0].start == 0);
    inv(where, "last line ends at len", lay.line[lay.count - 1].end == ed->len);

    for (i = 0; i < lay.count; i++) {
        inv(where, "line width <= COLS", lay.line[i].width <= OPUR_COLS);
        inv(where, "line start <= end", lay.line[i].start <= lay.line[i].end);
        if (i + 1 < lay.count) {
            inv(where, "lines are contiguous",
                lay.line[i].end == lay.line[i + 1].start);
        }
    }

    line = opur_cursor_line(&lay, ed->cursor);
    inv(where, "cursor line in range", line >= 0 && line < lay.count);
    if (line < 0 || line >= lay.count) return;

    col = opur_cursor_col(ed, &lay, line, ed->cursor);
    inv(where, "cursor col <= COLS", col >= 0 && col <= OPUR_COLS);

    max_top = lay.count - OPUR_ROWS;
    if (max_top < 0) max_top = 0;
    inv(where, "scroll_top in range",
        ed->scroll_top >= 0 && ed->scroll_top <= max_top);
    inv(where, "cursor is visible",
        line >= ed->scroll_top && line < ed->scroll_top + OPUR_ROWS);
}

// ---------------------------------------------------------------------------
// 画面イメージのダンプ（目視用）
// ---------------------------------------------------------------------------

static void dump(const OpurEditor* ed, const char* title) {
    OpurLayout lay;
    char utf8[256];
    int r, cur_line, cur_col;

    opur_layout(ed, &lay);
    cur_line = opur_cursor_line(&lay, ed->cursor);
    cur_col  = opur_cursor_col(ed, &lay, cur_line, ed->cursor);

    printf("\n--- %s ---\n", title);
    printf("+------------------------------+\n");
    for (r = 0; r < OPUR_ROWS; r++) {
        int li = ed->scroll_top + r;
        if (li >= lay.count) { printf("|~                             |\n"); continue; }
        line_utf8(ed, &lay, li, utf8, sizeof(utf8));
        printf("|%s%*s|", utf8, OPUR_COLS - lay.line[li].width, "");
        if (li == cur_line) printf("  <- cursor col %d", cur_col);
        printf("\n");
    }
    printf("+------------------------------+\n");
    printf("L:%d C:%d  %d/%d   (cursor=%d goal_col=%d scroll_top=%d lines=%d)\n",
           cur_line + 1, cur_col + 1, ed->len, OPUR_BUF_MAX,
           ed->cursor, ed->goal_col, ed->scroll_top, lay.count);
}

// ---------------------------------------------------------------------------
// 観点 1〜4: 折り返し
// ---------------------------------------------------------------------------

static void test_wrap_ascii(void) {
    OpurEditor ed;
    OpurLayout lay;

    printf("\n=== 観点1: 半角のみ 30 文字で折り返し ===\n");

    opur_init(&ed);
    feed_n(&ed, 'a', 30);
    opur_layout(&ed, &lay);
    eq_int("30 half: 行数", lay.count, 1);
    eq_int("30 half: 行0 の幅", lay.line[0].width, 30);

    opur_insert(&ed, 'b');            // 31 文字目
    opur_layout(&ed, &lay);
    eq_int("31 half: 行数", lay.count, 2);
    eq_int("31 half: 行0 の幅", lay.line[0].width, 30);
    eq_int("31 half: 行1 の先頭 index", lay.line[1].start, 30);
    eq_int("31 half: 行1 の幅", lay.line[1].width, 1);
    eq_int("31 half: カーソル行", opur_cursor_line(&lay, ed.cursor), 1);
    invariants(&ed, "wrap_ascii");
}

static void test_wrap_zenkaku(void) {
    OpurEditor ed;
    OpurLayout lay;

    printf("\n=== 観点2: 全角のみ 15 文字で折り返し ===\n");

    opur_init(&ed);
    feed_n(&ed, 0x3042, 15);          // あ x15 = 30 幅
    opur_layout(&ed, &lay);
    eq_int("15 zen: 行数", lay.count, 1);
    eq_int("15 zen: 行0 の幅", lay.line[0].width, 30);

    opur_insert(&ed, 0x3042);         // 16 文字目
    opur_layout(&ed, &lay);
    eq_int("16 zen: 行数", lay.count, 2);
    eq_int("16 zen: 行1 の先頭 index", lay.line[1].start, 15);
    eq_int("16 zen: 行1 の幅", lay.line[1].width, 2);
    invariants(&ed, "wrap_zenkaku");
}

static void test_wrap_overhang(void) {
    OpurEditor ed;
    OpurLayout lay;

    printf("\n=== 観点3: 行末はみ出し（半角29 + 全角1）===\n");

    opur_init(&ed);
    feed_n(&ed, 'a', 29);
    opur_insert(&ed, 0x6F22);         // 漢
    opur_layout(&ed, &lay);

    eq_int("行数", lay.count, 2);
    eq_int("行0 の幅（1幅の空白が残る）", lay.line[0].width, 29);
    eq_int("行0 の文字数", lay.line[0].end - lay.line[0].start, 29);
    eq_int("行1 の先頭 index（全角が次行へ）", lay.line[1].start, 29);
    eq_int("行1 の幅", lay.line[1].width, 2);
    invariants(&ed, "wrap_overhang");
}

static void test_wrap_mixed(void) {
    OpurEditor ed;
    OpurLayout lay;
    char utf8[256];

    printf("\n=== 観点4: 混在の折り返し ===\n");

    // 半角27 + 全角3。27 + 2 = 29 で1文字目の全角は収まり、2文字目で折り返す。
    opur_init(&ed);
    feed_n(&ed, 'a', 27);
    feed(&ed, "あいう");
    opur_layout(&ed, &lay);

    eq_int("行数", lay.count, 2);
    eq_int("行0 の幅", lay.line[0].width, 29);
    eq_int("行0 の文字数（半角27 + 全角1）", lay.line[0].end, 28);
    eq_int("行1 の幅", lay.line[1].width, 4);
    line_utf8(&ed, &lay, 1, utf8, sizeof(utf8));
    eq_str("行1 の内容", utf8, "いう");

    // 短い混在
    opur_init(&ed);
    feed(&ed, "abcあいう");
    opur_layout(&ed, &lay);
    eq_int("abcあいう: 行数", lay.count, 1);
    eq_int("abcあいう: 幅", lay.line[0].width, 9);
    invariants(&ed, "wrap_mixed");
}

// ---------------------------------------------------------------------------
// 観点 5: 上下移動 + 全角吸着 + goal_col 保持
// ---------------------------------------------------------------------------

static void test_snap_examples(void) {
    OpurEditor ed;
    OpurLayout lay;

    printf("\n=== 観点5-a: 全角吸着（発注書の例 [あ][い][u][え]）===\n");

    opur_init(&ed);
    feed(&ed, "あいuえ");            // 幅 0,2 / 2,4 / 4,5 / 5,7
    opur_layout(&ed, &lay);

    eq_int("goal_col=0 -> index", opur_index_at_col(&ed, &lay, 0, 0), 0);
    eq_int("goal_col=1 -> index（あ の左端）", opur_index_at_col(&ed, &lay, 0, 1), 0);
    eq_int("goal_col=2 -> index（い の左端）", opur_index_at_col(&ed, &lay, 0, 2), 1);
    eq_int("goal_col=3 -> index（い に吸着）", opur_index_at_col(&ed, &lay, 0, 3), 1);
    eq_int("goal_col=4 -> index（u の左端）",  opur_index_at_col(&ed, &lay, 0, 4), 2);
    eq_int("goal_col=5 -> index（え の左端）", opur_index_at_col(&ed, &lay, 0, 5), 3);
    eq_int("goal_col=6 -> index（え に吸着）", opur_index_at_col(&ed, &lay, 0, 6), 3);
}

static void test_goal_col_persist(void) {
    OpurEditor ed;
    OpurLayout lay;
    int line;

    printf("\n=== 観点5-b: goal_col は上下移動で保持される ===\n");

    // あいuえ / x / あいuえ  の 3 行
    opur_init(&ed);
    feed(&ed, "あいuえ\nx\nあいuえ");
    opur_layout(&ed, &lay);
    eq_int("行数", lay.count, 3);

    // 3行目の「え」（col 5）にカーソルを置く
    ed.cursor = 10;
    ed.goal_col = -1;
    opur_layout(&ed, &lay);
    line = opur_cursor_line(&lay, ed.cursor);
    eq_int("開始位置: 行", line, 2);
    eq_int("開始位置: 列", opur_cursor_col(&ed, &lay, line, ed.cursor), 5);

    // ↑ で短い行へ。行末 clamp（パターン①: LF の上）に着地するが goal_col は 5 のまま。
    opur_up(&ed);
    opur_layout(&ed, &lay);
    line = opur_cursor_line(&lay, ed.cursor);
    eq_int("↑ 1回目: 行", line, 1);
    eq_int("↑ 1回目: 列（行末 clamp）", opur_cursor_col(&ed, &lay, line, ed.cursor), 1);
    eq_int("↑ 1回目: cursor（LF の index）", ed.cursor, 6);
    eq_int("↑ 1回目: goal_col 保持", ed.goal_col, 5);

    // ↑ でもう一段。goal_col=5 なので「え」に復帰する。
    opur_up(&ed);
    opur_layout(&ed, &lay);
    line = opur_cursor_line(&lay, ed.cursor);
    eq_int("↑ 2回目: 行", line, 0);
    eq_int("↑ 2回目: 列（goal_col で復帰）",
           opur_cursor_col(&ed, &lay, line, ed.cursor), 5);
    eq_int("↑ 2回目: cursor（え の index）", ed.cursor, 3);

    // ← を挟むと goal_col はリセットされる
    opur_left(&ed);
    eq_int("← 後: goal_col リセット", ed.goal_col, -1);
    invariants(&ed, "goal_col_persist");
}

static void test_clamp_patterns(void) {
    OpurEditor ed;
    OpurLayout lay;

    printf("\n=== 観点5-c: 行末 clamp の 3 パターン ===\n");

    // ① LF 終端行 → LF の index に止まる
    opur_init(&ed);
    feed(&ed, "ab\ncdefg");
    opur_layout(&ed, &lay);
    eq_int("① LF終端行: clamp 先 index", opur_index_at_col(&ed, &lay, 0, 30), 2);
    ok("① それは LF である", ed.buf[2] == OPUR_LF);

    // ② 折り返し行 → 次行先頭文字の index に止まる
    opur_init(&ed);
    feed_n(&ed, 'a', 29);
    feed(&ed, "ああ");
    opur_layout(&ed, &lay);
    eq_int("② 折り返し行: 行0 の幅", lay.line[0].width, 29);
    eq_int("② 折り返し行: clamp 先 index", opur_index_at_col(&ed, &lay, 0, 30), 29);
    eq_int("② それは行1 の先頭", lay.line[1].start, 29);

    // ③ 最終行末（LF なし）→ len に止まる
    opur_init(&ed);
    feed(&ed, "abc");
    opur_layout(&ed, &lay);
    eq_int("③ 最終行末: clamp 先 index", opur_index_at_col(&ed, &lay, 0, 30), 3);
    eq_int("③ それは len", ed.len, 3);
}

static void test_vertical_bounds(void) {
    OpurEditor ed;
    OpurLayout lay;
    int before;

    printf("\n=== 観点5-d: 先頭行で↑ は何もしない / 最終行で↓ は行末へ ===\n");

    opur_init(&ed);
    feed(&ed, "abc\ndef");

    ed.cursor = 1;
    ed.goal_col = -1;
    before = ed.cursor;
    opur_up(&ed);
    eq_int("先頭行で↑", ed.cursor, before);

    // 既に末尾にいるなら動かない（見かけ上は従来と同じ）
    ed.cursor = ed.len;
    ed.goal_col = -1;
    before = ed.cursor;
    opur_down(&ed);
    eq_int("最終行の末尾で↓", ed.cursor, before);

    // 最終行の途中からは末尾へ飛ぶ（021 の変更点）
    ed.cursor = 5;              // "abc\ndef" の 'e' の手前
    ed.goal_col = -1;
    opur_down(&ed);
    eq_int("最終行の途中で↓ → 行末", ed.cursor, ed.len);
    eq_int("↓ で goal_col は捨てる", ed.goal_col, -1);

    // ↓ で降りて ↑ で戻る往復
    ed.cursor = 1;
    ed.goal_col = -1;
    opur_down(&ed);
    opur_layout(&ed, &lay);
    eq_int("↓ で 2 行目 col 1", ed.cursor, 5);
    opur_up(&ed);
    eq_int("↑ で戻る", ed.cursor, 1);
    invariants(&ed, "vertical_bounds");
}

// ---------------------------------------------------------------------------
// 観点 6, 7: LF
// ---------------------------------------------------------------------------

static void test_lf_with_wrap(void) {
    OpurEditor ed;
    OpurLayout lay;

    printf("\n=== 観点6: LF 改行と折り返しの共存 ===\n");

    opur_init(&ed);
    feed_n(&ed, 'a', 30);
    opur_insert(&ed, OPUR_LF);
    opur_insert(&ed, 'b');
    opur_layout(&ed, &lay);

    eq_int("行数", lay.count, 2);
    eq_int("行0 は LF 終端", lay.line[0].has_lf, 1);
    eq_int("行0 の end（LF を含む）", lay.line[0].end, 31);
    eq_int("行0 の幅（LF は 0 幅）", lay.line[0].width, 30);
    eq_int("行1 の先頭", lay.line[1].start, 31);

    // LF だけの行が潰れないこと
    opur_init(&ed);
    feed(&ed, "a\n\n\nb");
    opur_layout(&ed, &lay);
    eq_int("a\\n\\n\\nb: 行数", lay.count, 4);
    eq_int("空行1 の幅", lay.line[1].width, 0);
    eq_int("空行2 の幅", lay.line[2].width, 0);

    // 末尾 LF は空行を作る
    opur_init(&ed);
    feed(&ed, "ab\n");
    opur_layout(&ed, &lay);
    eq_int("末尾 LF: 行数", lay.count, 2);
    eq_int("末尾 LF: カーソル行", opur_cursor_line(&lay, ed.cursor), 1);
    invariants(&ed, "lf_with_wrap");
}

static void test_backspace_lf(void) {
    OpurEditor ed;
    OpurLayout lay;
    char utf8[256];

    printf("\n=== 観点7: Backspace で LF 削除 → 行結合 ===\n");

    opur_init(&ed);
    feed(&ed, "ab\ncd");
    opur_layout(&ed, &lay);
    eq_int("結合前: 行数", lay.count, 2);

    ed.cursor = 3;                    // "cd" の先頭
    opur_backspace(&ed);              // LF を消す
    opur_layout(&ed, &lay);
    eq_int("結合後: 行数", lay.count, 1);
    eq_int("結合後: len", ed.len, 4);
    eq_int("結合後: cursor", ed.cursor, 2);
    line_utf8(&ed, &lay, 0, utf8, sizeof(utf8));
    eq_str("結合後: 内容", utf8, "abcd");

    // 先頭での Backspace は何もしない
    ed.cursor = 0;
    opur_backspace(&ed);
    eq_int("先頭 Backspace: len 不変", ed.len, 4);
    eq_int("先頭 Backspace: cursor 不変", ed.cursor, 0);
    invariants(&ed, "backspace_lf");
}

// ---------------------------------------------------------------------------
// 観点 8: スクロール
// ---------------------------------------------------------------------------

static void test_scroll(void) {
    OpurEditor ed;
    OpurLayout lay;
    int i;

    printf("\n=== 観点8: スクロール ===\n");

    opur_init(&ed);
    for (i = 0; i < 10; i++) {
        opur_insert(&ed, (uint16_t)('0' + i));
        if (i < 9) opur_insert(&ed, OPUR_LF);
    }
    opur_layout(&ed, &lay);
    eq_int("行数", lay.count, 10);
    eq_int("最終行にいるとき scroll_top", ed.scroll_top, 4);
    eq_int("カーソル行", opur_cursor_line(&lay, ed.cursor), 9);

    for (i = 0; i < 9; i++) opur_up(&ed);
    opur_layout(&ed, &lay);
    eq_int("先頭行まで↑: カーソル行", opur_cursor_line(&lay, ed.cursor), 0);
    eq_int("先頭行まで↑: scroll_top", ed.scroll_top, 0);

    for (i = 0; i < 9; i++) opur_down(&ed);
    eq_int("最終行まで↓: scroll_top", ed.scroll_top, 4);

    // 本文行数（6 行）以下なら scroll_top は 0 のまま
    opur_init(&ed);
    feed(&ed, "a\nb\nc");
    eq_int("3 行: scroll_top", ed.scroll_top, 0);

    // 行が減ったら scroll_top も戻る
    opur_init(&ed);
    for (i = 0; i < 10; i++) {
        opur_insert(&ed, (uint16_t)('0' + i));
        if (i < 9) opur_insert(&ed, OPUR_LF);
    }
    eq_int("削除前: scroll_top", ed.scroll_top, 4);
    while (ed.len > 3) opur_backspace(&ed);
    eq_int("削除後: scroll_top", ed.scroll_top, 0);
    invariants(&ed, "scroll");
}

// ---------------------------------------------------------------------------
// 観点 9: 512 文字上限
// ---------------------------------------------------------------------------

static void test_buffer_limit(void) {
    OpurEditor ed;

    printf("\n=== 観点9: 512 文字上限 ===\n");

    opur_init(&ed);
    feed_n(&ed, 'a', OPUR_BUF_MAX);
    eq_int("満杯時の len", ed.len, OPUR_BUF_MAX);
    eq_int("満杯時の cursor", ed.cursor, OPUR_BUF_MAX);

    opur_insert(&ed, 'z');
    eq_int("超過挿入後の len", ed.len, OPUR_BUF_MAX);
    eq_int("超過挿入後の cursor", ed.cursor, OPUR_BUF_MAX);
    ok("末尾は 'a' のまま", ed.buf[OPUR_BUF_MAX - 1] == 'a');

    opur_insert(&ed, OPUR_LF);
    eq_int("超過 LF 後の len", ed.len, OPUR_BUF_MAX);

    // 先頭に挿入しようとしても増えない
    ed.cursor = 0;
    opur_insert(&ed, 'z');
    eq_int("先頭への超過挿入", ed.len, OPUR_BUF_MAX);
    ok("先頭は 'a' のまま", ed.buf[0] == 'a');

    // 1 文字消せばまた入る
    ed.cursor = ed.len;
    opur_backspace(&ed);
    opur_insert(&ed, 'z');
    eq_int("1 減らして 1 増やす", ed.len, OPUR_BUF_MAX);
    ok("末尾が 'z'", ed.buf[OPUR_BUF_MAX - 1] == 'z');
    invariants(&ed, "buffer_limit");
}

// ---------------------------------------------------------------------------
// 観点 10: 空バッファ
// ---------------------------------------------------------------------------

static void test_empty_buffer(void) {
    OpurEditor ed;
    OpurLayout lay;

    printf("\n=== 観点10: 空バッファで各操作 ===\n");

    opur_init(&ed);
    opur_layout(&ed, &lay);
    eq_int("空: 行数", lay.count, 1);
    eq_int("空: 行0 の幅", lay.line[0].width, 0);
    eq_int("空: カーソル行", opur_cursor_line(&lay, ed.cursor), 0);

    opur_left(&ed);      invariants(&ed, "empty ←");
    opur_right(&ed);     invariants(&ed, "empty →");
    opur_up(&ed);        invariants(&ed, "empty ↑");
    opur_down(&ed);      invariants(&ed, "empty ↓");
    opur_backspace(&ed); invariants(&ed, "empty BS");

    eq_int("空: len", ed.len, 0);
    eq_int("空: cursor", ed.cursor, 0);
    eq_int("空: scroll_top", ed.scroll_top, 0);
    ok("空バッファで全操作しても不変条件 OK", g_inv_fail == 0);
}

// ---------------------------------------------------------------------------
// 追加: 制御文字の拒否
// ---------------------------------------------------------------------------

static void test_control_chars(void) {
    OpurEditor ed;
    int i;

    printf("\n=== 追加: 制御文字は入力させない ===\n");

    opur_init(&ed);
    for (i = 0x00; i <= 0x1F; i++) {
        if (i == OPUR_LF) continue;
        opur_insert(&ed, (uint16_t)i);
    }
    opur_insert(&ed, 0x7F);
    eq_int("制御文字は 1 つも入らない", ed.len, 0);

    opur_insert(&ed, OPUR_LF);
    eq_int("LF は入る", ed.len, 1);
}

// ---------------------------------------------------------------------------
// 追加: ランダム操作で不変条件を壊さないか
// ---------------------------------------------------------------------------

static unsigned int g_seed = 20260813u;

static unsigned int xorshift32(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

static void test_fuzz(void) {
    OpurEditor ed;
    int i;
    const int kOps = 200000;
    int before = g_inv_fail;

    printf("\n=== 追加: ランダム操作 %d 回（シード固定）===\n", kOps);

    opur_init(&ed);
    for (i = 0; i < kOps; i++) {
        unsigned int r = xorshift32() % 100;
        if (r < 40) {
            /* 半角 */ opur_insert(&ed, (uint16_t)(0x20 + (xorshift32() % 95)));
        } else if (r < 60) {
            /* 全角 */ opur_insert(&ed, (uint16_t)(0x3041 + (xorshift32() % 80)));
        } else if (r < 66) {
            opur_insert(&ed, OPUR_LF);
        } else if (r < 78) {
            opur_backspace(&ed);
        } else if (r < 84) {
            opur_left(&ed);
        } else if (r < 90) {
            opur_right(&ed);
        } else if (r < 95) {
            opur_up(&ed);
        } else {
            opur_down(&ed);
        }
        invariants(&ed, "fuzz");
        if (g_inv_fail > before) {
            printf("  最初の違反は %d 回目の操作（種別 r=%u）\n", i, r);
            break;
        }
    }
    ok("ランダム操作で不変条件が保たれる", g_inv_fail == before);
}

// ---------------------------------------------------------------------------

static void run_dump(void) {
    OpurEditor ed;

    opur_init(&ed);
    feed_n(&ed, 'a', 29);
    feed(&ed, "漢字");
    dump(&ed, "観点3: 半角29 + 全角 → 行末に 1 幅の空白");

    opur_init(&ed);
    feed(&ed, "あいuえ\nx\nあいuえ");
    ed.cursor = 10; ed.goal_col = -1;
    opur_up(&ed);
    dump(&ed, "観点5: ↑ で短い行に clamp（goal_col は 5 のまま）");
    opur_up(&ed);
    dump(&ed, "観点5: さらに↑ で goal_col=5 に復帰");

    opur_init(&ed);
    feed(&ed, "0\n1\n2\n3\n4\n5\n6\n7\n8\n9");
    dump(&ed, "観点8: 10 行のうち末尾 6 行を表示");
}

int main(int argc, char** argv) {
    printf("=== opur_editor selftest ===\n");

    test_wrap_ascii();
    test_wrap_zenkaku();
    test_wrap_overhang();
    test_wrap_mixed();
    test_snap_examples();
    test_goal_col_persist();
    test_clamp_patterns();
    test_vertical_bounds();
    test_lf_with_wrap();
    test_backspace_lf();
    test_scroll();
    test_buffer_limit();
    test_empty_buffer();
    test_control_chars();
    test_fuzz();

    if (argc >= 2 && strcmp(argv[1], "--dump") == 0) {
        run_dump();
    }

    printf("\n---- %d passed, %d failed, %d invariant violations ----\n",
           g_pass, g_fail, g_inv_fail);
    return (g_fail == 0 && g_inv_fail == 0) ? 0 : 1;
}
