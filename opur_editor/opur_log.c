// opur_log.c — 起動ログのリングバッファ

#include "opur_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// 静的に持つ。ログのために malloc したくない（失敗したときに
// 「失敗したこと」を記録できなくなる）。
static char g_line[OPUR_LOG_LINES][OPUR_LOG_LINE_MAX];

// 書き込んだ延べ行数。OPUR_LOG_LINES を超えたら古いものが上書きされる。
static int g_total = 0;

void opur_log_clear(void) {
    g_total = 0;
}

void opur_log_add(const char *fmt, ...) {
    va_list ap;
    char   *dst = g_line[g_total % OPUR_LOG_LINES];
    int     need;

    if (!fmt) return;

    va_start(ap, fmt);
    need = vsnprintf(dst, OPUR_LOG_LINE_MAX, fmt, ap);
    va_end(ap);

    // vsnprintf は途中で切っても終端を付けるが、UTF-8 の途中で切れると
    // 画面に豆腐が出る。**切り詰めが起きたときだけ**文字境界まで戻す。
    //
    // 収まった文字列にこの処理を掛けてはいけない。末尾が正常な多バイト文字でも
    // 継続バイト → 先頭バイトと辿って 1 文字まるごと落としてしまう
    // （実際に「ev:なし」が「ev:な」になっていた）。
    if (need >= OPUR_LOG_LINE_MAX) {
        size_t n = strlen(dst);
        while (n > 0 && ((unsigned char)dst[n - 1] & 0xC0) == 0x80) n--;
        // 継続バイトを剥がした先に残る先頭バイトが、切れた文字の頭。落とす。
        if (n > 0 && ((unsigned char)dst[n - 1] & 0xC0) == 0xC0) n--;
        dst[n] = '\0';
    }

    // 実機ではシリアルにも流す。画面は 30 桁 x 7 行しか無く長い行は右が切れるので、
    // 切り分け中はホスト側で全文を読めたほうが早い。
    //
    // stdout は sdkconfig の CONFIG_ESP_CONSOLE_UART_DEFAULT に従って UART0 に
    // 出るが、CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG も立っているので
    // **USB 側にも出る**（実機で受信を確認済み）。
    //
    // ホスト側で読むときは、データが無い間の read が 0 を返しても
    // 読み続けること。`cat /dev/cu.usbmodem*` はそれを EOF と解釈して
    // すぐ終わってしまい、「何も出ていない」ように見える。
    //
    // 誰も繋いでいなければ捨てられるだけで、実機側は止まらない。
#ifdef ESP_PLATFORM
    printf("%s\n", dst);
    fflush(stdout);
#endif

    g_total++;
}

int opur_log_count(void) {
    return (g_total < OPUR_LOG_LINES) ? g_total : OPUR_LOG_LINES;
}

const char *opur_log_line(int i) {
    const int count = opur_log_count();
    int start;

    if (i < 0 || i >= count) return "";

    // 溢れていなければ 0 番から、溢れていれば次に上書きされる位置が最古。
    start = (g_total <= OPUR_LOG_LINES) ? 0 : (g_total % OPUR_LOG_LINES);

    return g_line[(start + i) % OPUR_LOG_LINES];
}
