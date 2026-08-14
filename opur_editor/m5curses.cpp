// m5curses.cpp — curses 互換薄層の実機実装
//
// 描画は必ず Canvas（オフスクリーン）に対して行い、refresh() で一括転送する。
// 直接 Display に描くとちらつくため。curses の refresh と同じ役割。
//
// 色深度は 8bpp。240x135x1byte = 約 32KB で SRAM に収まる。
// 16bpp だと 64KB 必要で、本文バッファや辞書キャッシュと取り合いになる。
// 文字は前景・背景の 2 色しか使わないので 8bpp で足りる。

#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>

// M5Cardputer の Keyboard_def.h は KEY_LEFT / KEY_ENTER などを
// **USB HID のスキャンコード**として定義している（KEY_LEFT = 0x50 など）。
// m5curses は同じ名前を **ncurses のキーコード**として使うので、
// ここで剥がしてから m5curses.h を読み込む。
//
// 衝突するのはこのファイルの中だけ。view_m5.c や main.cpp は
// M5Cardputer.h を include しないので影響を受けない。
// なお getch() は HID コードを使わず keysState() の bool フラグだけを見るので、
// 剥がしたことで困る箇所は無い。
#undef KEY_LEFT
#undef KEY_RIGHT
#undef KEY_UP
#undef KEY_DOWN
#undef KEY_ENTER
#undef KEY_BACKSPACE
#undef KEY_TAB

#include "m5curses.h"

namespace {

M5Canvas *g_canvas = nullptr;

// 通常時の色。A_REVERSE のときは入れ替えて使う。
constexpr uint16_t kFg = TFT_WHITE;
constexpr uint16_t kBg = TFT_BLACK;

int g_attrs = A_NORMAL;

bool g_sd_ok = false;

inline uint16_t fg() { return (g_attrs & A_REVERSE) ? kBg : kFg; }
inline uint16_t bg() { return (g_attrs & A_REVERSE) ? kFg : kBg; }

// 行の先頭 y 座標（px）。仕切り線より下の行は 1px ぶん下にずれる。
inline int row_y(int row) {
    const int shift = (M5C_SEP_ROW >= 0 && row > M5C_SEP_ROW) ? M5C_SEP_HEIGHT : 0;
    return row * M5C_CELL_H + shift;
}

}  // namespace

// ---------------------------------------------------------------------------

// SD カードを ESP-IDF の VFS にマウントする。
// マウントできれば普通の fopen("/sdcard/...") が使えるようになるので、
// opur_dict.c は fopen / fseek / fread のまま実機でも動く。
//
// ピン番号は M5.getPin() から取る。無印 Cardputer と ADV で同じ値だが、
// 直書きするとボードが増えたときに壊れるため。
static bool mount_sd(void) {
    if (!M5.hasSD()) return false;

    SPI.begin(M5.getPin(m5::pin_name_t::sd_spi_sclk),
              M5.getPin(m5::pin_name_t::sd_spi_miso),
              M5.getPin(m5::pin_name_t::sd_spi_mosi),
              M5.getPin(m5::pin_name_t::sd_spi_ss));

    // Arduino の SD.begin は既定のマウント先が "/sd" なので明示的に指定する。
    return SD.begin(M5.getPin(m5::pin_name_t::sd_spi_ss), SPI,
                    25000000, M5C_SD_MOUNT, 5, false);
}

void initscr(void) {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);

    g_sd_ok = mount_sd();

    auto &d = M5Cardputer.Display;
    d.setRotation(1);              // 240x135 の横向き
    d.fillScreen(kBg);

    g_canvas = new M5Canvas(&d);
    g_canvas->setColorDepth(8);
    // 画面まるごと確保する。下端の余白も clear() で消えてほしいため。
    g_canvas->createSprite(M5C_SCREEN_W, M5C_SCREEN_H);

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
    const int y0 = row_y(y);

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

void m5c_separator(void) {
    if (!g_canvas || M5C_SEP_ROW < 0) return;
    // 仕切り行の直下、ずらしたぶんの隙間そのものに引く。
    const int y = (M5C_SEP_ROW + 1) * M5C_CELL_H;
    g_canvas->drawFastHLine(0, y, M5C_SCREEN_W, kFg);
}

// ---------------------------------------------------------------------------

void attron(int attrs) {
    g_attrs |= attrs;
}

void attroff(int attrs) {
    g_attrs &= ~attrs;
}

// ---------------------------------------------------------------------------

// --- キー入力 ---------------------------------------------------------------
//
// M5Cardputer.Keyboard はキーの「今の状態」しか教えてくれないので、
// 前回のスキャン結果と突き合わせて「新しく押されたもの」だけを拾う。
//
// ライブラリの isChange() は押下**数**の増減しか見ておらず、
// 同時押し数が変わらないままキーが入れ替わると取りこぼす（007 調査）。
// なので自前で差分を取る。
//
// キーリピートは実装しない。押しっぱなしでは 1 回しか返らない。

namespace {

constexpr int kPollMs   = 10;   // ポーリング間隔。CPU を回しっぱなしにしないため
constexpr int kKeysMax  = 8;    // 1 スキャンで拾う最大数

int g_prev[kKeysMax];           // 前回のスキャンで押されていたキー
int g_prev_n = 0;

int g_pend[kKeysMax];           // 未返却のキー（同時押しを取りこぼさないため）
int g_pend_n = 0;

// 今押されているキーを curses のコードに直して out に集める。個数を返す。
int scan_keys(int *out) {
    const auto &st = M5Cardputer.Keyboard.keysState();
    int n = 0;

    // Fn 押下中はライブラリが Fn 層だけを処理して return するので、
    // word は必ず空になる。フラグから拾うしかない（007 調査）。
    if (st.fn) {
        if (st.left  && n < kKeysMax) out[n++] = KEY_LEFT;
        if (st.right && n < kKeysMax) out[n++] = KEY_RIGHT;
        if (st.up    && n < kKeysMax) out[n++] = KEY_UP;
        if (st.down  && n < kKeysMax) out[n++] = KEY_DOWN;
        if (st.esc   && n < kKeysMax) out[n++] = KEY_ESC;
        if (st.del   && n < kKeysMax) out[n++] = KEY_DC;
        return n;
    }

    // Enter / BS / Tab は word には入らないのでフラグから拾う。
    if (st.enter     && n < kKeysMax) out[n++] = '\r';
    if (st.backspace && n < kKeysMax) out[n++] = KEY_BACKSPACE;
    if (st.tab       && n < kKeysMax) out[n++] = KEY_TAB;

    // 印字可能文字。Space(0x20) もここに含まれる。
    for (const char c : st.word) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u <= 0x7E && n < kKeysMax) out[n++] = static_cast<int>(u);
    }
    return n;
}

bool was_down(int code) {
    for (int i = 0; i < g_prev_n; i++) {
        if (g_prev[i] == code) return true;
    }
    return false;
}

}  // namespace

int m5c_sd_ready(void) {
    return g_sd_ok ? 1 : 0;
}

// ---------------------------------------------------------------------------

int getch(void) {
    for (;;) {
        // 溜まっているものがあれば先に返す
        if (g_pend_n > 0) {
            const int c = g_pend[0];
            for (int i = 1; i < g_pend_n; i++) g_pend[i - 1] = g_pend[i];
            g_pend_n--;
            return c;
        }

        M5Cardputer.update();

        int cur[kKeysMax];
        const int n = scan_keys(cur);

        // 前回押されていなかったものだけを積む。
        // 1 スキャンで 2 つ以上増えても落とさないようキューにする。
        for (int i = 0; i < n; i++) {
            if (!was_down(cur[i]) && g_pend_n < kKeysMax) {
                g_pend[g_pend_n++] = cur[i];
            }
        }

        for (int i = 0; i < n; i++) g_prev[i] = cur[i];
        g_prev_n = n;

        if (g_pend_n == 0) delay(kPollMs);
    }
}
