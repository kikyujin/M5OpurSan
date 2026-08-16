// m5curses.cpp — curses 互換薄層の実機実装
//
// 描画は必ず Canvas（オフスクリーン）に対して行い、refresh() で一括転送する。
// 直接 Display に描くとちらつくため。curses の refresh と同じ役割。
//
// 色深度は 1bpp。240x135 で約 4KB。文字は前景・背景の 2 色しか使わないので
// 階調は要らない。8bpp（32KB）から落として 28KB 空けてある。
// **この 28KB が無いと TLS のハンドシェイクが張れない**（CLAUDE.md 参照）。

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
// なお getch() は HID コードを使わず、keysState() の bool フラグと
// keyList() / getKeyValue() の**素の文字**しか見ないので、剥がしても困らない。
#undef KEY_LEFT
#undef KEY_RIGHT
#undef KEY_UP
#undef KEY_DOWN
#undef KEY_ENTER
#undef KEY_BACKSPACE
#undef KEY_TAB

#include "m5curses.h"
#include "splash_png.h"

namespace {

M5Canvas *g_canvas = nullptr;

// スプラッシュを出しておく最短時間（ms）。
// SD マウントはこの裏で走るので、実際の待ちはこれより短い。
constexpr uint32_t kSplashMs = 1500;

// Canvas は 1bpp なので、これは色そのものではなくパレット番号。
// 実際の色は canvas_alloc() の setPaletteColor で入れる。
// A_REVERSE のときは入れ替えて使う。
constexpr uint16_t kFg = 1;
constexpr uint16_t kBg = 0;

int g_attrs = A_NORMAL;

bool g_sd_ok = false;

inline uint16_t fg() { return (g_attrs & A_REVERSE) ? kBg : kFg; }
inline uint16_t bg() { return (g_attrs & A_REVERSE) ? kFg : kBg; }

// スプライトを確保して、描画に必要な設定を入れる。取れなければ false。
bool canvas_alloc(M5Canvas *c) {
    // 1bpp。この画面は前景・背景の 2 色しか使わないので、階調は要らない。
    //
    // 8bpp（1 ピクセル 1 バイト）だと 240x135 = 32KB を起動から終了まで
    // 握りっぱなしになる。この機体は PSRAM を積んでいない ESP32-S3FN8 で、
    // WiFi 接続後の内部ヒープは 44KB しか残らず、TLS ハンドシェイクに要る
    // 45〜50KB が作れなかった（実機で GET -1 を確認）。
    // 1bpp なら 4KB で済み、28KB がまるごと空く。
    c->setColorDepth(1);

    // 画面まるごと確保する。下端の余白も clear() で消えてほしいため。
    if (!c->createSprite(M5C_SCREEN_W, M5C_SCREEN_H)) return false;

    // パレット。kBg = 黒、kFg = 白。A_REVERSE はこの 2 つを入れ替えるだけ。
    c->setPaletteColor(kBg, 0, 0, 0);
    c->setPaletteColor(kFg, 255, 255, 255);

    c->setFont(&fonts::efontJA_16);   // 東雲 16px（JIS 第二水準まで）
    c->setTextSize(1);
    c->setTextWrap(false);            // 折り返しは呼び出し側の責任
    c->setTextColor(kFg, kBg);
    c->fillSprite(kBg);
    return true;
}

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

    auto &d = M5Cardputer.Display;
    d.setRotation(1);              // 240x135 の横向き

    // スプラッシュ。画面が使えるようになった直後に出し、SD マウントは
    // その裏で済ませる。数百 ms かかるので、
    // 先に出しておくと待ち時間がまるごと隠れる。
    // Canvas ではなく Display に直接描く。Canvas はまだ無いし、
    // 8bpp に落とすと写真的な階調が潰れるため。
    d.drawPng(splash_png, sizeof(splash_png), 0, 0);
    const uint32_t splash_start = millis();

    g_sd_ok = mount_sd();

    // NVS はここでは触らない。setup() より前に走る Arduino の initArduino() が
    // 同じ手順（nvs_flash_init → 壊れていたら消して再 init）を済ませていて、
    // 二重にやる意味が無い。WiFi の校正情報もその NVS に入っているので、
    // こちらから消しにいかないこと（021 で自前の初期化を撤去した）。

    // 裏の初期化で使ったぶんを差し引いて、残りだけ待つ。
    // 初期化のほうが長引いていたら待たずに進む。
    {
        const uint32_t elapsed = millis() - splash_start;
        if (elapsed < kSplashMs) delay(kSplashMs - elapsed);
    }

    d.fillScreen(TFT_BLACK);

    g_canvas = new M5Canvas(&d);
    if (!canvas_alloc(g_canvas)) {
        // ここで失敗したら以降の描画はすべて no-op になる（各関数の先頭で
        // g_canvas を見ている）。表示は出ないが、起動は止めない。
        delete g_canvas;
        g_canvas = nullptr;
    }
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

int g_timeout_ms = -1;          // <0 = 入力があるまで待つ

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

        // Fn + S。ライブラリのキーマップでは S の Fn 層が KEY_NONE なので、
        // フラグにも hid_keys にも何も入らない（Keyboard.cpp の PASS 2 が
        // KEY_NONE を continue で捨てる）。押されている座標を自分で引いて、
        // Fn 層ではなく**素の文字**が 's' かどうかで判定する。
        //
        // keyList() / getKeyValue() はどちらも公開 API で、キーマップは
        // Cardputer と ADV で共通なので、この判定は両方で同じように効く。
        for (const auto &pos : M5Cardputer.Keyboard.keyList()) {
            if (M5Cardputer.Keyboard.getKeyValue(pos).value_first == 's') {
                if (n < kKeysMax) out[n++] = KEY_SAVE;
                break;
            }
        }
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

// バッテリー残量。ADC を叩くのは 30 秒に 1 回だけで、あいだはキャッシュを返す。
//
// getBatteryLevel() は毎回 ADC を 1 回読んで較正表を引くだけなので重くはないが、
// 描画のたびに呼ぶ意味も無い。間引きをここに置いたのは、呼び出し側の
// view_m5.c が C で millis() を持たないため。
//
// 初回だけ M5Unified が較正構造体を calloc する（数十バイト・一度きり）。
// ヒープを継続的に食わないので、TLS の枠には影響しない。
int m5c_battery_level(void) {
    constexpr uint32_t kIntervalMs = 30000;

    static int      cached = -1;
    static uint32_t last   = 0;
    static bool     primed = false;   // 一度でも読んだか

    const uint32_t now = millis();

    // 起動直後は now が小さく last == 0 と区別できないので、
    // 経過ではなく primed で初回を判定する。
    // 失敗（負）もそのままキャッシュする。毎描画で ADC を叩き直さないため。
    if (!primed || (uint32_t)(now - last) >= kIntervalMs) {
        primed = true;
        cached = (int)M5.Power.getBatteryLevel();   // 取れなければ負
        last   = now;
    }
    return cached;
}

int m5c_battery_mv(void) {
    return (int)M5.Power.getBatteryVoltage();   // 取れなければ 0
}

// ARDUINO_USB_CDC_ON_BOOT=1 / ARDUINO_USB_MODE=1 なので Serial は HWCDC。
// bool 変換の実体は HWCDC::isCDC_Connected() で、直近に SOF が来ているか
// （＝ホストが繋がっているか）を見ている。
int m5c_usb_connected(void) {
    return Serial ? 1 : 0;
}

// ---------------------------------------------------------------------------

// sleep() はバックライトを 0 にしてからパネルにスリープコマンドを送る。
// wakeup() は明るさを元に戻すところまでやってくれる（M5GFX が覚えている）。
void m5c_display_off(void) {
    M5Cardputer.Display.sleep();
}

void m5c_display_on(void) {
    M5Cardputer.Display.wakeup();
}

// ---------------------------------------------------------------------------

void timeout(int ms) {
    g_timeout_ms = ms;
}

int getch(void) {
    const uint32_t start = millis();

    for (;;) {
        // 溜まっているものがあれば待ち時間に関係なく先に返す
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

        if (g_pend_n > 0) continue;   // 積めた。次の周回で返す

        if (g_timeout_ms == 0) return ERR;
        // 引き算は uint32_t なので millis() が一周しても正しく効く
        if (g_timeout_ms > 0 && (millis() - start) >= (uint32_t)g_timeout_ms) {
            return ERR;
        }

        delay(kPollMs);
    }
}
