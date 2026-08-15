// editor.c — バッファ操作・カーソル制御（環境非依存ロジック）
//
// このファイルは curses / M5Stack / FEP のいずれにも依存しない。
// #include できるのは標準ヘッダと editor.h のみ。

#include "editor.h"

#include <string.h>

// ---------------------------------------------------------------------------
// 初期化・文字幅
// ---------------------------------------------------------------------------

void opur_init(OpurEditor* ed) {
    memset(ed, 0, sizeof(*ed));
    ed->goal_col = -1;
}

int opur_char_width(uint16_t ch) {
    if (ch >= 0x0020 && ch <= 0x007E) return 1;  // ASCII 印字可能文字
    if (ch == OPUR_LF)                return 0;  // LF は幅ゼロ（改行）
    return 2;                                    // それ以外はすべて全角扱い
}

// ---------------------------------------------------------------------------
// 折り返し
// ---------------------------------------------------------------------------

static void emit_line(OpurLayout* out, int start, int end, int width, int has_lf) {
    OpurLine* l;
    if (out->count >= OPUR_MAX_LINES) return;
    l = &out->line[out->count++];
    l->start  = start;
    l->end    = end;
    l->width  = width;
    l->has_lf = has_lf;
}

void opur_layout(const OpurEditor* ed, OpurLayout* out) {
    int i = 0, col = 0, start = 0;

    out->count = 0;

    while (i < ed->len) {
        uint16_t ch = ed->buf[i];
        int w;

        if (ch == OPUR_LF) {
            // LF は行の一部（end に含める）。次の文字から新しい行。
            emit_line(out, start, i + 1, col, 1);
            i++;
            start = i;
            col = 0;
            continue;
        }

        w = opur_char_width(ch);
        if (col + w > OPUR_COLS && col > 0) {
            // 収まらない → 行確定。この文字は次行の先頭に置く（i は進めない）。
            // col > 0 の条件は「1 行に必ず 1 文字は置く」保証（無限ループ防止）。
            emit_line(out, start, i, col, 0);
            start = i;
            col = 0;
            continue;
        }

        col += w;
        i++;
    }

    // 最終行。末尾が LF なら start == len となり、空行が 1 行できる。
    emit_line(out, start, ed->len, col, 0);
}

// ---------------------------------------------------------------------------
// カーソル位置の解決
// ---------------------------------------------------------------------------

// 折り返し境界のインデックスは「前行の end」と「次行の start」の両方に一致する。
// どちらの行に属すると見なすかで ←→ と ↑↓ の見え方が変わる。
//
// ここでは次行の col 0 に属するものとする（last-line affinity）。
//   ・→ で行末まで進んだとき、次行の先頭に自然に降りる
//   ・↓ で goal_col = 0 の行に降りるケース（頻出）が正しく動く
//
// 反対に「前行に属する」とすると、goal_col = 0 での ↓ が動かなくなる。
// なお clamp で折り返し行の行末に着地するのは goal_col が行幅を超える場合のみで、
// 折り返し行の幅は 29 か 30 なので goal_col = 30 のときだけの稀なケース。
int opur_cursor_line(const OpurLayout* lay, int cursor) {
    int l;
    for (l = 0; l < lay->count; l++) {
        if (cursor < lay->line[l].end) return l;
    }
    return lay->count - 1;  // cursor == len（最終行の末尾）
}

int opur_cursor_col(const OpurEditor* ed, const OpurLayout* lay,
                    int line, int cursor) {
    int col = 0;
    int i;
    for (i = lay->line[line].start; i < cursor && i < ed->len; i++) {
        col += opur_char_width(ed->buf[i]);
    }
    return col;
}

int opur_index_at_col(const OpurEditor* ed, const OpurLayout* lay,
                      int line, int goal) {
    const OpurLine* ln = &lay->line[line];
    int content_end = ln->has_lf ? (ln->end - 1) : ln->end;
    int col = 0;
    int i = ln->start;

    while (i < content_end) {
        int w = opur_char_width(ed->buf[i]);
        if (col + w > goal) break;  // goal はこの文字の左端 or 途中 → 左端に吸着
        col += w;
        i++;
    }

    // ループを抜けた i が行末 clamp の 3 パターンをそのまま表す:
    //   ① LF 終端行   → content_end = end - 1 なので LF のインデックス
    //   ② 折り返し行   → content_end = end     なので次行先頭文字のインデックス
    //   ③ 最終行末     → content_end = len     なので len（バッファ末尾）
    return i;
}

// ---------------------------------------------------------------------------
// スクロール
// ---------------------------------------------------------------------------

void opur_update_scroll(OpurEditor* ed) {
    OpurLayout lay;
    int line, max_top;

    opur_layout(ed, &lay);
    line = opur_cursor_line(&lay, ed->cursor);

    if (line < ed->scroll_top) {
        ed->scroll_top = line;
    }
    if (line >= ed->scroll_top + OPUR_ROWS) {
        ed->scroll_top = line - (OPUR_ROWS - 1);
    }

    // 削除で行数が減ったときに scroll_top が浮かないようクランプ。
    max_top = lay.count - OPUR_ROWS;
    if (max_top < 0) max_top = 0;
    if (ed->scroll_top > max_top) ed->scroll_top = max_top;
    if (ed->scroll_top < 0)       ed->scroll_top = 0;
}

// ---------------------------------------------------------------------------
// 編集操作
// ---------------------------------------------------------------------------

void opur_insert(OpurEditor* ed, uint16_t ch) {
    if (ed->len >= OPUR_BUF_MAX) return;             // 満杯
    if (ch < 0x20 && ch != OPUR_LF) return;          // 制御文字は入力させない
    if (ch == 0x7F) return;                          // DEL も同様

    memmove(&ed->buf[ed->cursor + 1], &ed->buf[ed->cursor],
            (size_t)(ed->len - ed->cursor) * sizeof(uint16_t));
    ed->buf[ed->cursor] = ch;
    ed->len++;
    ed->cursor++;
    ed->goal_col = -1;
    opur_update_scroll(ed);
}

void opur_backspace(OpurEditor* ed) {
    if (ed->cursor == 0) return;                     // 先頭

    ed->cursor--;
    memmove(&ed->buf[ed->cursor], &ed->buf[ed->cursor + 1],
            (size_t)(ed->len - ed->cursor - 1) * sizeof(uint16_t));
    ed->len--;
    ed->goal_col = -1;
    opur_update_scroll(ed);
}

// ---------------------------------------------------------------------------
// カーソル移動
// ---------------------------------------------------------------------------

void opur_left(OpurEditor* ed) {
    if (ed->cursor > 0) ed->cursor--;
    ed->goal_col = -1;
    opur_update_scroll(ed);
}

void opur_right(OpurEditor* ed) {
    if (ed->cursor < ed->len) ed->cursor++;
    ed->goal_col = -1;
    opur_update_scroll(ed);
}

// 上下移動では goal_col を書き換えない。短い行を経由して長い行に戻ったとき、
// 元のカラム位置に復帰するために必要。
static void move_vertical(OpurEditor* ed, int delta) {
    OpurLayout lay;
    int line, target;

    opur_layout(ed, &lay);
    line = opur_cursor_line(&lay, ed->cursor);
    target = line + delta;

    // 最終行より下は無い。何もしないのではなく、行末（= バッファ末尾）へ寄せる。
    // 最終行の途中にいるとき、末尾まで行くのに → を連打するしかなかったため。
    // goal_col は捨てる（→ と同じで、明示的に横方向を決め直した扱い）。
    if (target >= lay.count) {
        ed->cursor   = ed->len;
        ed->goal_col = -1;
        opur_update_scroll(ed);
        return;
    }

    if (target < 0) return;   // 先頭行より上は無い。↑ は従来どおり何もしない

    if (ed->goal_col < 0) {
        ed->goal_col = opur_cursor_col(ed, &lay, line, ed->cursor);
    }
    ed->cursor = opur_index_at_col(ed, &lay, target, ed->goal_col);
    opur_update_scroll(ed);
}

void opur_up(OpurEditor* ed)   { move_vertical(ed, -1); }
void opur_down(OpurEditor* ed) { move_vertical(ed, +1); }
