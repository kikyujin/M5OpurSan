// opur_config.c — /config.txt のパーサー
//
// SD は initscr() の中で ESP-IDF の VFS にマウント済みなので、
// opur_dict.c と同じく普通の fopen / fgets で読める。
//
// 実機の config.txt は「LF 改行・最終行に改行なし・コメントなし」だが、
// 手で書き換えることを考えて CRLF・行頭 '#' のコメント・空行・
// '=' 前後の空白は受け付ける。知らないキーは黙って無視する
// （将来キーが増えたとき、古いファームでも起動できるように）。

#include "opur_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 値を 1 つ写す。長すぎる値は切り詰める（起動を止める理由にはしない）。
static void set_str(char *dst, const char *src) {
    strncpy(dst, src, OPUR_CFG_VAL_MAX - 1);
    dst[OPUR_CFG_VAL_MAX - 1] = '\0';
}

static int is_space(char c) {
    return c == ' ' || c == '\t';
}

// 前後の空白と行末の改行を落とす。s を直接書き換え、先頭を返す。
static char *trim(char *s) {
    char *end;

    while (*s && is_space(*s)) s++;

    end = s + strlen(s);
    while (end > s) {
        char c = end[-1];
        if (c == '\r' || c == '\n' || is_space(c)) end--;
        else break;
    }
    *end = '\0';

    return s;
}

void opur_config_clear(OpurConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
}

int opur_config_load(OpurConfig *cfg, const char *path) {
    char  line[OPUR_CFG_LINE_MAX];
    FILE *fp;
    int   found = 0;

    if (!cfg || !path) return -1;

    fp = fopen(path, "r");
    if (!fp) return -1;

    // 最終行に改行が無い場合も fgets はその行を返してから EOF になるので、
    // 特別扱いは要らない。
    while (fgets(line, sizeof(line), fp)) {
        char *key, *val, *eq;

        // 行が長すぎて途中で切れた場合、残りは次の行として読まずに捨てる。
        // 半端な尻尾を key=value として誤解釈しないため。
        if (!strchr(line, '\n') && !feof(fp)) {
            int c;
            while ((c = fgetc(fp)) != '\n' && c != EOF) { }
            continue;
        }

        eq = strchr(line, '=');
        if (!eq) continue;              // 空行・コメント・壊れた行はここで落ちる

        *eq = '\0';
        key = trim(line);
        val = trim(eq + 1);

        if (key[0] == '#' || key[0] == '\0') continue;

        if      (strcmp(key, "WIFI_SSID")    == 0) set_str(cfg->wifi_ssid,    val);
        else if (strcmp(key, "WIFI_PASS")    == 0) set_str(cfg->wifi_pass,    val);
        else if (strcmp(key, "OPUR_HOST")    == 0) set_str(cfg->opur_host,    val);
        else if (strcmp(key, "DANTONG_HOST") == 0) set_str(cfg->dantong_host, val);
        else if (strcmp(key, "OPUR_PORT")    == 0) cfg->opur_port    = atoi(val);
        else if (strcmp(key, "DANTONG_PORT") == 0) cfg->dantong_port = atoi(val);
        else continue;                  // 知らないキー。found に数えない

        found++;
    }

    fclose(fp);
    return found;
}

int opur_config_has_wifi(const OpurConfig *cfg) {
    return cfg && cfg->wifi_ssid[0] != '\0';
}
