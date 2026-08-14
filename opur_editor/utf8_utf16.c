// utf8_utf16.c — UTF-8 ⇔ UTF-16 変換（BMP のみ）

#include "utf8_utf16.h"

// 継続バイトか
static int is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

// ---------------------------------------------------------------------------
// 復号
// ---------------------------------------------------------------------------

int u8_decode(const char *s, uint32_t *cp) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char b0 = p[0];

    if (b0 == 0) {
        *cp = 0;
        return 0;
    }

    if (b0 < 0x80) {
        *cp = b0;
        return 1;
    }

    // 0xC0 / 0xC1 は必ず overlong なので受け付けない
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        if (!is_cont(p[1])) goto bad;
        *cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        return 2;
    }

    if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (!is_cont(p[1]) || !is_cont(p[2])) goto bad;
        if (b0 == 0xE0 && p[1] < 0xA0) goto bad;              // overlong
        if (b0 == 0xED && p[1] >= 0xA0) goto bad;             // サロゲート域
        *cp = ((uint32_t)(b0 & 0x0F) << 12) |
              ((uint32_t)(p[1] & 0x3F) << 6) |
              (uint32_t)(p[2] & 0x3F);
        return 3;
    }

    if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (!is_cont(p[1]) || !is_cont(p[2]) || !is_cont(p[3])) goto bad;
        if (b0 == 0xF0 && p[1] < 0x90) goto bad;              // overlong
        if (b0 == 0xF4 && p[1] >= 0x90) goto bad;             // U+10FFFF 超え
        *cp = ((uint32_t)(b0 & 0x07) << 18) |
              ((uint32_t)(p[1] & 0x3F) << 12) |
              ((uint32_t)(p[2] & 0x3F) << 6) |
              (uint32_t)(p[3] & 0x3F);
        return 4;   // BMP 外。呼び出し側が U+FFFD に落とす
    }

bad:
    *cp = U8_REPLACEMENT;
    return 1;
}

// ---------------------------------------------------------------------------
// 符号化
// ---------------------------------------------------------------------------

int u8_encode(uint32_t cp, char *out) {
    // BMP 外とサロゲートは表現できないので置き換える
    if (cp > 0xFFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        cp = U8_REPLACEMENT;
    }

    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

// ---------------------------------------------------------------------------
// 変換
// ---------------------------------------------------------------------------

int utf8_to_utf16(const char *src, uint16_t *dst, int dst_max) {
    int n = 0;

    while (*src && n < dst_max) {
        uint32_t cp;
        int used = u8_decode(src, &cp);
        if (used == 0) break;
        src += used;
        if (cp > 0xFFFFu) cp = U8_REPLACEMENT;   // BMP のみ
        dst[n++] = (uint16_t)cp;
    }
    return n;
}

int utf16_to_utf8(const uint16_t *src, int len, char *dst, size_t dst_size) {
    size_t n = 0;
    int i;

    if (dst_size == 0) return 0;

    for (i = 0; i < len; i++) {
        char tmp[3];
        int w = u8_encode(src[i], tmp);
        int j;
        if (n + (size_t)w + 1 > dst_size) break;   // NUL のぶんを残す
        for (j = 0; j < w; j++) dst[n++] = tmp[j];
    }
    dst[n] = '\0';
    return (int)n;
}
