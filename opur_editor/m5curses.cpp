// m5curses.cpp — curses 互換薄層の実機実装
//
// 描画は必ず Canvas（オフスクリーン）に対して行い、refresh() で一括転送する。
// 直接 Display に描くとちらつくため。curses の refresh と同じ役割。
//
// 色深度は 8bpp。240x135x1byte = 約 32KB で SRAM に収まる。
// 16bpp だと 64KB 必要で、本文バッファや辞書キャッシュと取り合いになる。
// 文字は前景・背景の 2 色しか使わないので 8bpp で足りる。

#include "m5curses.h"

#include <M5Cardputer.h>

namespace {

M5Canvas *g_canvas = nullptr;

// 通常時の色。A_REVERSE のときは入れ替えて使う。
constexpr uint16_t kFg = TFT_WHITE;
constexpr uint16_t kBg = TFT_BLACK;

int g_attrs = A_NORMAL;

inline uint16_t fg() { return (g_attrs & A_REVERSE) ? kBg : kFg; }
inline uint16_t bg() { return (g_attrs & A_REVERSE) ? kFg : kBg; }

}  // namespace

// ---------------------------------------------------------------------------

void initscr(void) {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);

    auto &d = M5Cardputer.Display;
    d.setRotation(1);              // 240x135 の横向き
    d.fillScreen(kBg);

    g_canvas = new M5Canvas(&d);
    g_canvas->setColorDepth(8);
    g_canvas->createSprite(M5C_COLS * M5C_CELL_W, M5C_ROWS * M5C_CELL_H);

    g_canvas->setFont(&fonts::efontJA_16);   // 東雲 16px（JIS 第二水準まで）
    g_canvas->setTextSize(1);
    g_canvas->setTextWrap(false);            // 折り返しは呼び出し側の責任
    g_canvas->setTextColor(kFg, kBg);
    g_canvas->fillSprite(kBg);
}

void endwin(void) {
    // 実機には戻す先の端末が無いので何もしない。
}

void clear(void) {
    if (!g_canvas) return;
    g_canvas->fillSprite(bg());
}

void refresh(void) {
    if (!g_canvas) return;
    g_canvas->pushSprite(0, 0);
}

// ---------------------------------------------------------------------------

void mvaddstr(int y, int x, const char *s) {
    if (!g_canvas || !s) return;
    if (y < 0 || y >= M5C_ROWS || x < 0 || x >= M5C_COLS) return;

    const int x0 = x * M5C_CELL_W;
    const int y0 = y * M5C_CELL_H;

    // 背景を塗りながら描くので、A_REVERSE がそのまま反転表示になる。
    g_canvas->setTextColor(fg(), bg());
    g_canvas->setCursor(x0, y0);
    g_canvas->print(s);

    if (g_attrs & A_UNDERLINE) {
        // 実際に描かれた幅はカーソルの進みぶん。全角・半角が混ざっても正しく出る。
        const int w = g_canvas->getCursorX() - x0;
        if (w > 0) {
            g_canvas->drawFastHLine(x0, y0 + M5C_CELL_H - 1, w, fg());
        }
    }
}

void mvaddch(int y, int x, char c) {
    char buf[2] = { c, '\0' };
    mvaddstr(y, x, buf);
}

// ---------------------------------------------------------------------------

void attron(int attrs) {
    g_attrs |= attrs;
}

void attroff(int attrs) {
    g_attrs &= ~attrs;
}

// ---------------------------------------------------------------------------

int getch(void) {
    // Phase E-0 では入力を扱わない。実装は Phase E-1。
    delay(10);
    return ERR;
}
