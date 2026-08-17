// m5curses.h — curses 互換の薄層（M5Cardputer / M5Cardputer ADV 用）
//
// view_curses.c が PC で ncurses を叩くのと同じ形で、実機側では
// この層を叩く。将来の view_m5.c は C11 のままでいられるように
// extern "C" で囲ってある（実装 m5curses.cpp は C++）。
//
// 画面は 240x135 / efontJA_16 → 半角 8x16px、全角 16x16px。
// したがって 30 半角 x 8 行。座標は文字セル単位で、
//   x = 半角文字幅（8px）単位、y = 行（16px）単位。
// 全角文字は 2 セルぶんの幅を占める。
//
// Phase E-0 では表示のみ。getch() は未実装（常に ERR を返す）。

#ifndef M5CURSES_H_INCLUDED
#define M5CURSES_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

// 文字セルの寸法（px）
#define M5C_CELL_W 8
#define M5C_CELL_H 16

// 画面サイズ（px / 文字セル単位）。実機 240x135 に対応する。
#define M5C_SCREEN_W 240
#define M5C_SCREEN_H 135
#define M5C_COLS 30
#define M5C_ROWS 8

// 仕切り線。M5C_SEP_ROW 行の下に 1px の水平線を入れる。
//
// 8 行 x 16px = 128px なので 135px の画面には 7px 余る。その 1px を線に使い、
// 残り 6px は下端の余白にする。線を入れたぶん、M5C_SEP_ROW より下の行は
// 1px だけ下にずれる（行を 1 行まるごと潰さずに区切りを入れるため）。
//
// -1 にすると仕切り無しになり、全行が 16px 刻みに戻る。
#define M5C_SEP_ROW    5
#define M5C_SEP_HEIGHT 1

// 属性。ncurses の同名定数と同じ用途で使う。
//   A_REVERSE   候補バーの選択中の候補（view_curses.c の draw_cand 相当）
//   A_UNDERLINE FEP の未確定文字列（view_curses.c の draw_fep 相当）
#define A_NORMAL    0x0000
#define A_REVERSE   0x0001
#define A_UNDERLINE 0x0002

// curses と同じく「入力なし」を表す値
#define ERR (-1)

// getch() が返す特殊キー。**値は ncurses と同じ**にしてある。
// こうしておくと main.cpp の curses_to_vkey() が PC と実機で共通で使える。
#define KEY_DOWN      0x102   /* ncurses 0402 */
#define KEY_UP        0x103   /* ncurses 0403 */
#define KEY_LEFT      0x104   /* ncurses 0404 */
#define KEY_RIGHT     0x105   /* ncurses 0405 */
#define KEY_BACKSPACE 0x107   /* ncurses 0407 */
#define KEY_DC        0x14A   /* ncurses 0512 — Delete */
#define KEY_ENTER     0x157   /* ncurses 0527 */

// 印字可能文字と同じくそのまま返すもの
#define KEY_ESC 0x1B
#define KEY_TAB 0x09

// ncurses に相当が無い、この層だけのキー。
// ncurses の KEY_MAX は 0777（511）なので、0x200 以上なら衝突しない。
#define KEY_SAVE (0x200 + 'S')   /* Fn + S。保存のショートカット */

// M5.begin() → Display 初期化 → 描画用 Canvas 確保 → efontJA_16 設定。
// 失敗しても戻り値では通知しない（curses の initscr と同じ流儀）。
void initscr(void);

// curses では端末を元に戻す処理。実機では戻す先が無いので何もしない。
void endwin(void);

// Canvas を背景色で塗りつぶす。画面にはまだ出ない。
void clear(void);

// Canvas を画面に転送する。ここで初めて表示が変わる。
void refresh(void);

// UTF-8 文字列を (y, x) に描く。x は半角セル単位。
// 属性（A_REVERSE）は描画時点の状態が適用される。
void mvaddstr(int y, int x, const char *s);

// ASCII 1 文字版。
void mvaddch(int y, int x, char c);

// M5C_SEP_ROW 行の下に仕切り線を引く。M5C_SEP_ROW が -1 なら何もしない。
// ncurses の mvhline は 1 行まるごと使ってしまうが、実機は 8 行しか無いので
// 行を消費しないこちらを使う。
void m5c_separator(void);

// SD カードのマウント先。initscr() の中で ESP-IDF の VFS にマウントするので、
// このパスの下は普通の fopen / fseek / fread で読める。
#define M5C_SD_MOUNT "/sdcard"

// SD がマウントできていれば 1。initscr() のあとで意味を持つ。
int m5c_sd_ready(void);

// バッテリー残量（0〜100 %）。取れないときは負。
//
// 実測は 30 秒に 1 回だけで、あいだはキャッシュを返す。**呼び出し側で
// 間引かなくていい**（view_m5.c は millis() を持たない C なのでこちらに置いた）。
//
// Cardputer には PMIC が無く、GPIO10 の 1/2 分圧を ADC1 で読んでいる
// （M5Unified が面倒を見る。無印・ADV とも同じ）。ADC1 なので WiFi と併用できる。
// 充電中かどうかは取れない（M5Unified の isCharging() は Cardputer を扱わない）。
// 充電中は電圧が上がるので 100% に張り付いて見える。
int m5c_battery_level(void);

// バッテリー電圧（mV）。取れないときは 0。
//
// % は M5Unified の 3300mV=0% / 4100mV=100% というざっくり換算なので、
// 表示が怪しいときはこちらの生値で切り分ける。キャッシュしていないので、
// 定期的に呼ぶ用途には向かない（起動ログ用）。
int m5c_battery_mv(void);

// USB が挿さっているかを返す関数は**置いていない**。
//
// Serial（HWCDC）の bool 変換は「ホストが CDC エンドポイントに読み書きした」
// ときにしか 1 にならず、このアプリは Serial を一度も使わない（ログの printf は
// ESP-IDF のコンソール経由で HWCDC を通らない）。つまり構造的に常に 0 で、
// 判定に使えないまま置いておくと「USB を見ている」と読めてしまう。
// 開発用の都合にそこまでコストを掛ける理由が無いので消した（2026-08-17）。
//
// 給電中に寝ないようにする判定は main.cpp の on_wall_power() 一本。

// 画面を消す / 点ける。スリープの前後で使う。
//
// 消すのはバックライトとパネルの両方（M5.Display.sleep()）。Canvas の中身は
// 触らないので、点け直したあと refresh() すれば元の絵がそのまま出る。
// 明るさは M5GFX 側が覚えていて、m5c_display_on() で元に戻る。
void m5c_display_off(void);
void m5c_display_on(void);

// 属性のオン・オフ。
//   A_REVERSE   前景色と背景色を入れ替える
//   A_UNDERLINE 描いた文字列の下端に 1px の水平線を引く
// 複数同時に立てられる（ncurses と同じくビットフラグ）。
void attron(int attrs);
void attroff(int attrs);

// キーが押されるまでブロックして、curses 互換のキーコードを 1 つ返す。
//
//   ASCII 印字可能 (0x20-0x7E)  そのまま
//   Enter                       '\r'
//   BS                          KEY_BACKSPACE
//   Tab                         KEY_TAB
//   Fn + , . ; /                KEY_LEFT / KEY_DOWN / KEY_UP / KEY_RIGHT
//   Fn + `                      KEY_ESC
//   Fn + BS                     KEY_DC
//   Fn + S                      KEY_SAVE
//
// 押しっぱなしでは繰り返さない（キーリピートは未実装）。
// Ctrl+英字 は生成しない —— 実機では ESC でメニューに入る仕様のため不要。
//
// timeout() を設定していれば、待ち時間を過ぎたときに ERR を返す。
int getch(void);

// getch() の待ち方を決める（ncurses の timeout と同じ）。
//   ms <  0   入力があるまで待つ（既定）
//   ms == 0   待たない。無ければすぐ ERR
//   ms >  0   ms ミリ秒待って無ければ ERR
//
// ERR を返すのは「本当に何も来ていないとき」だけで、
// 溜まっているキーがあれば待ち時間に関係なく先に返す。
void timeout(int ms);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // M5CURSES_H_INCLUDED
