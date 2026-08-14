// opur_log.h — 画面から読める起動ログ（リングバッファ）
//
// 実機にはシリアルコンソールを常時繋いでおけない。ESP32-S3 の USB CDC は
// リセットのたびに再列挙するので、起動直後のログはホスト側でまず取りこぼす。
// そこで本体側に溜めておき、ESC メニューの「3.ログ」から後で読む。
//
// 環境非依存の C11。SD も M5GFX も使わないが、PC 版の main.cpp からは
// 呼んでいないので Makefile には足していない。
//
// 容量は 32 行 x 64 バイト = 2KB の静的領域。溢れたら古い行から消える。

#ifndef OPUR_LOG_H_INCLUDED
#define OPUR_LOG_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#define OPUR_LOG_LINES    32

// 1 行の最大バイト数。画面は 30 半角桁なので、全角だけでも 30 バイト強。
// 余裕を見て 64。超えたぶんは切り捨てる。
#define OPUR_LOG_LINE_MAX 64

// 全部消す。
void opur_log_clear(void);

// 1 行足す。printf と同じ書式。改行は入れない（1 呼び出し = 1 行）。
void opur_log_add(const char *fmt, ...);

// 今持っている行数（0〜OPUR_LOG_LINES）。
int opur_log_count(void);

// i 番目の行。**0 が最も古い**。範囲外なら ""。
const char *opur_log_line(int i);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPUR_LOG_H_INCLUDED
