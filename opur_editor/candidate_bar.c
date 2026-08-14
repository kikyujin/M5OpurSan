// candidate_bar.c — 変換候補バー
//
// curses にも M5 にも依存しない。ファイル I/O は opur_dict 経由の辞書引きだけ。

#include "candidate_bar.h"
#include "utf8_utf16.h"

#include <stdio.h>
#include <string.h>

// ビューポート外に候補があることを示すマーク。どちらも半角 1 文字ぶんとして数える。
static const char *kMarkLeft  = "◀";
static const char *kMarkRight = "▶";

#define MARK_WIDTH 1

// LRU に載っていない候補のソートキー（rank より必ず大きい値）
#define LRU_RANK_NONE 0xFFFFF
// 単漢字グループへ落とすためのオフセット
#define TANKAN_BIAS   0x100000

// ---------------------------------------------------------------------------
// 表示幅
// ---------------------------------------------------------------------------

// editor.c の opur_char_width と同じ規則: ASCII 印字可能 = 1、それ以外 = 2。
static int cp_width(uint32_t cp) {
    return (cp >= 0x20u && cp <= 0x7Eu) ? 1 : 2;
}

static int text_width(const char *s) {
    int w = 0;
    while (*s) {
        uint32_t cp;
        int used = u8_decode(s, &cp);
        if (used == 0) break;
        w += cp_width(cp);
        s += used;
    }
    return w;
}

// 画面内の何番目かで番号 "N:" が付く。付くのは先頭 10 件まで。
static int has_number(int vpos) { return vpos < 10; }

static char number_char(int vpos) {
    return (vpos < 9) ? (char)('1' + vpos) : '0';
}

static int label_width(const CandBar *bar, int index, int vpos) {
    return (has_number(vpos) ? 2 : 0) + text_width(bar->item[index].text);
}

// ---------------------------------------------------------------------------
// ビューポート
// ---------------------------------------------------------------------------

// win_start を左端としたとき、画面内に収まる最後の候補インデックスを返す。
// 先頭 1 件はビューポートより広くても必ず入れる（そうしないと進めなくなる）。
static int window_last(const CandBar *bar, int start,
                       int *out_left, int *out_right) {
    int left  = (start > 0) ? 1 : 0;
    int right = 0;
    int last  = start;
    int pass;

    for (pass = 0; pass < 2; pass++) {
        int avail = CAND_VIEW_WIDTH - (left ? MARK_WIDTH : 0)
                                    - (right ? MARK_WIDTH : 0);
        int used = 0;
        int i, vpos = 0;

        last = start;
        for (i = start; i < bar->count; i++, vpos++) {
            int w = label_width(bar, i, vpos) + (i > start ? 1 : 0);  // 区切りの空白
            if (i > start && used + w > avail) break;
            used += w;
            last = i;
        }

        if (last >= bar->count - 1) {   // 最後まで見えた → ▶ は要らない
            right = 0;
            break;
        }
        if (right) break;               // ▶ を引いた 2 周目。これで確定
        right = 1;                      // ▶ のぶん狭めてやり直す
    }

    if (out_left)  *out_left  = left;
    if (out_right) *out_right = right;
    return last;
}

// 選択中の候補が画面内に入るよう win_start を寄せる。
// 右へはみ出したら「次候補が左端に来る」ようにずらす（発注書の指定）。
static void ensure_visible(CandBar *bar) {
    int last;

    if (bar->win_start > bar->sel) {
        bar->win_start = bar->sel;
        return;
    }
    last = window_last(bar, bar->win_start, NULL, NULL);
    if (bar->sel > last) bar->win_start = bar->sel;
}

// ---------------------------------------------------------------------------
// LRU
// ---------------------------------------------------------------------------

// (reading, surface) の LRU 内の位置。見つからなければ -1。
static int lru_rank(const CandBar *bar, const char *surface) {
    int i;
    for (i = 0; i < bar->lru_count; i++) {
        if (strcmp(bar->lru[i].reading, bar->reading) != 0) continue;
        if (strcmp(bar->lru[i].surface, surface) == 0) return i;
    }
    return -1;
}

static void lru_touch(CandBar *bar, const char *surface) {
    int at = lru_rank(bar, surface);
    int move;

    if (strlen(bar->reading) >= CAND_READING_MAX) return;
    if (strlen(surface) >= CAND_LRU_TEXT_MAX) return;   // 覚えられないものは諦める

    if (at == 0) return;    // すでに先頭

    if (at > 0) {
        move = at;          // [0..at-1] を 1 つ後ろへ
    } else {
        if (bar->lru_count < CAND_LRU_MAX) bar->lru_count++;
        move = bar->lru_count - 1;   // 末尾を押し出す
    }

    if (move > 0) {
        memmove(&bar->lru[1], &bar->lru[0], (size_t)move * sizeof(CandLruEntry));
    }
    snprintf(bar->lru[0].reading, CAND_READING_MAX, "%s", bar->reading);
    snprintf(bar->lru[0].surface, CAND_LRU_TEXT_MAX, "%s", surface);
}

// ---------------------------------------------------------------------------
// 候補リストの構築
// ---------------------------------------------------------------------------

static int already_have(const CandBar *bar, const char *text) {
    int i;
    for (i = 0; i < bar->count; i++) {
        if (strcmp(bar->item[i].text, text) == 0) return 1;
    }
    return 0;
}

// 追加できたら 1。容量超過・長すぎる場合は 0。
// allow_dup=0 なら既出の表記は捨てる。文字種候補は位置が仕様で決まっているので
// allow_dup=1 で必ず入れる（辞書に同じ表記があると重複するが、位置の予測可能性を優先）。
static int add_item(CandBar *bar, const char *text, int reserve_tail, int allow_dup) {
    size_t len = strlen(text);
    CandItem *it;

    if (text[0] == '\0') return 0;
    if (len >= CAND_TEXT_MAX) return 0;          // 切ると UTF-8 が壊れるので捨てる
    if (bar->count >= CAND_MAX - reserve_tail) return 0;
    if (!allow_dup && already_have(bar, text)) return 0;

    it = &bar->item[bar->count];
    memcpy(it->text, text, len + 1);
    it->is_tankan = (opur_u8_count(text) == 1);
    bar->count++;
    return 1;
}

// index の候補を取り除く（辞書候補の範囲のみ）。
static void remove_item(CandBar *bar, int index) {
    int rest = bar->count - index - 1;
    if (rest > 0) {
        memmove(&bar->item[index], &bar->item[index + 1],
                (size_t)rest * sizeof(CandItem));
    }
    bar->count--;
    if (index >= 1 && index <= bar->dict_count) bar->dict_count--;
}

// 末尾に近い単漢字を 1 件捨てる。捨てられたら 1。
static int drop_last_tankan(CandBar *bar) {
    int i;
    for (i = bar->count - 1; i >= 1; i--) {
        if (bar->item[i].is_tankan) {
            remove_item(bar, i);
            return 1;
        }
    }
    return 0;
}

static int on_hit(const char *reading, const char *surface, void *ctx) {
    CandBar *bar = (CandBar *)ctx;
    (void)reading;

    // カタカナ・全角英字・半角英字の 3 枠を残しておく
    if (add_item(bar, surface, 3, 0)) return 0;

    // 満杯だった。単漢字はどのみち末尾送りなので、複数文字の候補が来たら
    // 単漢字を 1 件捨てて席を空ける。
    // （「こう」は全 255 件中 245 件が単漢字で、複数文字候補が 250 番目以降にも
    //   現れる。素直に先頭から詰めると有用な候補が落ちてしまう）
    if (bar->count >= CAND_MAX - 3 && opur_u8_count(surface) > 1) {
        if (!already_have(bar, surface) && drop_last_tankan(bar)) {
            add_item(bar, surface, 3, 0);
        }
    }
    return 0;   // 打ち切らずに最後まで見る
}

// 辞書候補を「LRU 順 → 元の辞書順」「単漢字は末尾」に並べ替える。
// 安定な挿入ソート。単漢字グループの中でも LRU 昇格は効く。
static void reorder_dict(CandBar *bar) {
    int key[CAND_MAX];
    int lo = 1, hi = 1 + bar->dict_count;
    int i;

    if (bar->dict_count <= 1) return;

    for (i = lo; i < hi; i++) {
        int rank = lru_rank(bar, bar->item[i].text);
        key[i] = (bar->item[i].is_tankan ? TANKAN_BIAS : 0) +
                 (rank >= 0 ? rank : LRU_RANK_NONE);
    }

    for (i = lo + 1; i < hi; i++) {
        CandItem it = bar->item[i];
        int k = key[i];
        int j = i;
        while (j > lo && key[j - 1] > k) {
            bar->item[j] = bar->item[j - 1];
            key[j] = key[j - 1];
            j--;
        }
        bar->item[j] = it;
        key[j] = k;
    }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

void cand_bar_init(CandBar *bar, OpurDict *dict, const CandConv *conv) {
    memset(bar, 0, sizeof(*bar));
    bar->dict = dict;
    if (conv) bar->conv = *conv;
}

void cand_bar_clear(CandBar *bar) {
    bar->reading[0] = '\0';
    bar->count = 0;
    bar->dict_count = 0;
    bar->sel = 0;
    bar->win_start = 0;
    bar->committed[0] = '\0';
}

// 文字種候補と同じ表記の辞書候補を取り除く。
// 文字種候補は N+1〜N+3 に必ず置く仕様なので、放っておくと二重に並ぶ
// （「きょう」の辞書候補には「キョウ」が入っている）。
static void drop_duplicate_of(CandBar *bar, const char *text) {
    int i;
    if (text[0] == '\0') return;
    for (i = 1; i <= bar->dict_count; i++) {
        if (strcmp(bar->item[i].text, text) == 0) {
            remove_item(bar, i);
            return;
        }
    }
}

int cand_bar_start(CandBar *bar, const char *reading_utf8) {
    char kata[CAND_TEXT_MAX], full[CAND_TEXT_MAX], half[CAND_TEXT_MAX];

    if (!reading_utf8 || reading_utf8[0] == '\0') return -1;

    cand_bar_clear(bar);

    // 長すぎる読みは文字境界で切る
    {
        size_t nb = strlen(reading_utf8);
        if (nb >= CAND_READING_MAX) {
            int nchars = opur_u8_count(reading_utf8);
            while (nchars > 0) {
                nb = opur_u8_bytes(reading_utf8, nchars);
                if (nb < CAND_READING_MAX) break;
                nchars--;
            }
            if (nchars == 0) return -1;
        }
        memcpy(bar->reading, reading_utf8, nb);
        bar->reading[nb] = '\0';
    }

    // 0: ひらがな
    add_item(bar, bar->reading, 0, 0);

    // 1〜N: 辞書候補
    if (bar->dict) {
        opur_dict_search(bar->dict, bar->reading, -1, on_hit, bar);
    }
    bar->dict_count = bar->count - 1;

    // 文字種候補の文面を先に作り、同じものが辞書候補にあれば辞書側を落とす
    kata[0] = full[0] = half[0] = '\0';
    if (bar->conv.to_katakana)  bar->conv.to_katakana(bar->reading, kata, sizeof(kata));
    if (bar->conv.to_fullwidth) bar->conv.to_fullwidth(bar->reading, full, sizeof(full));
    if (bar->conv.to_halfwidth) bar->conv.to_halfwidth(bar->reading, half, sizeof(half));
    drop_duplicate_of(bar, kata);
    drop_duplicate_of(bar, full);
    drop_duplicate_of(bar, half);

    reorder_dict(bar);

    // N+1〜N+3: 文字種変換（位置が仕様で決まっているので必ずこの順で入れる）
    if (bar->conv.to_katakana)  add_item(bar, kata, 0, 1);
    if (bar->conv.to_fullwidth) add_item(bar, full, 0, 1);
    if (bar->conv.to_halfwidth) add_item(bar, half, 0, 1);

    return bar->count;
}

int cand_bar_count(const CandBar *bar) { return bar->count; }
int cand_bar_selected(const CandBar *bar) { return bar->sel; }

const char *cand_bar_text(const CandBar *bar, int index) {
    if (index < 0 || index >= bar->count) return "";
    return bar->item[index].text;
}

int cand_bar_is_tankan(const CandBar *bar, int index) {
    if (index < 0 || index >= bar->count) return 0;
    return bar->item[index].is_tankan;
}

const char *cand_bar_committed(const CandBar *bar) { return bar->committed; }

static void commit(CandBar *bar, int index) {
    const char *text = bar->item[index].text;
    snprintf(bar->committed, CAND_TEXT_MAX, "%s", text);
    // 辞書候補を選んだときだけ LRU に覚える。
    // 文字種候補（カタカナなど）は位置が固定なので覚えても意味がない。
    if (index >= 1 && index <= bar->dict_count) lru_touch(bar, text);
}

// 数字キーが指している候補インデックス。該当なしなら -1。
static int index_for_digit(const CandBar *bar, char digit) {
    int last = window_last(bar, bar->win_start, NULL, NULL);
    int i, vpos = 0;

    for (i = bar->win_start; i <= last; i++, vpos++) {
        if (!has_number(vpos)) break;
        if (number_char(vpos) == digit) return i;
    }
    return -1;
}

CandResult cand_bar_key(CandBar *bar, int key) {
    if (bar->count == 0) return CAND_IGNORED;

    switch (key) {
    case CAND_KEY_RIGHT:
    case ' ':
        if (bar->sel + 1 < bar->count) {
            bar->sel++;
            ensure_visible(bar);
            return CAND_UPDATED;
        }
        return CAND_IGNORED;

    case CAND_KEY_LEFT:
        if (bar->sel > 0) {
            bar->sel--;
            ensure_visible(bar);
            return CAND_UPDATED;
        }
        return CAND_IGNORED;

    case CAND_KEY_ENTER:
        commit(bar, bar->sel);
        return CAND_COMMITTED;

    case CAND_KEY_ESC:
        bar->committed[0] = '\0';
        return CAND_CANCELLED;

    default:
        if (key >= '0' && key <= '9') {
            int target = index_for_digit(bar, (char)key);
            if (target >= 0) {
                bar->sel = target;
                commit(bar, target);
                return CAND_COMMITTED;
            }
        }
        return CAND_IGNORED;
    }
}

// ---------------------------------------------------------------------------
// レンダリング
// ---------------------------------------------------------------------------

static void put(CandRender *r, size_t *n, const char *s) {
    size_t len = strlen(s);
    if (*n + len + 1 > CAND_RENDER_MAX) return;
    memcpy(r->text + *n, s, len);
    *n += len;
    r->text[*n] = '\0';
}

void cand_bar_render(const CandBar *bar, CandRender *out) {
    size_t n = 0;
    int left = 0, right = 0;
    int i, vpos = 0;
    int col = 0;

    memset(out, 0, sizeof(*out));
    out->text[0] = '\0';
    out->first = bar->win_start;
    out->last = bar->win_start;
    out->sel_col = 0;
    out->sel_width = 0;

    if (bar->count == 0) return;

    out->last = window_last(bar, bar->win_start, &left, &right);
    out->has_left = left;
    out->has_right = right;

    if (left) {
        put(out, &n, kMarkLeft);
        col += MARK_WIDTH;
    }

    for (i = bar->win_start; i <= out->last; i++, vpos++) {
        int w;

        if (i > bar->win_start) {
            put(out, &n, " ");
            col += 1;
        }

        w = label_width(bar, i, vpos);
        if (i == bar->sel) {
            out->sel_col = col;
            out->sel_width = w;
        }

        if (has_number(vpos)) {
            char num[3];
            num[0] = number_char(vpos);
            num[1] = ':';
            num[2] = '\0';
            put(out, &n, num);
        }
        put(out, &n, bar->item[i].text);
        col += w;
    }

    if (right) {
        put(out, &n, kMarkRight);
        col += MARK_WIDTH;
    }

    out->width = col;
}
