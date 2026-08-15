// opur_dict.c — system.dic 辞書引き実装
//
// test_dict.c から切り出したもの。ロジックは原典のまま。

#include "opur_dict.h"

#include <stdlib.h>
#include <string.h>

#define FIELD_MAX   OPUR_DICT_FIELD_MAX
#define HEADER_SIZE OPUR_DICT_HEADER_SIZE

// ---------------------------------------------------------------------------
// 計測用カウンタ
// ---------------------------------------------------------------------------

static long g_seeks = 0;
static long g_searches = 0;
static long g_hint_searches = 0, g_hint_seeks = 0;
static long g_bs_searches = 0,   g_bs_seeks = 0;

void opur_dict_get_stats(OpurDictStats *out) {
    if (!out) return;
    out->searches      = g_searches;
    out->seeks         = g_seeks;
    out->hint_searches = g_hint_searches;
    out->hint_seeks    = g_hint_seeks;
    out->bs_searches   = g_bs_searches;
    out->bs_seeks      = g_bs_seeks;
}

void opur_dict_reset_stats(void) {
    g_seeks = g_searches = 0;
    g_hint_searches = g_hint_seeks = 0;
    g_bs_searches = g_bs_seeks = 0;
}

// ---------------------------------------------------------------------------
// 低レベル入出力
// ---------------------------------------------------------------------------

static int read_exact(FILE *fp, void *buf, size_t n) {
    return fread(buf, 1, n, fp) == n ? 0 : -1;
}

static int read_u8(FILE *fp, unsigned *out) {
    unsigned char b;
    if (read_exact(fp, &b, 1) != 0) return -1;
    *out = b;
    return 0;
}

static int read_u32le(FILE *fp, uint32_t *out) {
    unsigned char b[4];
    if (read_exact(fp, b, 4) != 0) return -1;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

// i 番目のレコードの先頭バイト位置を、末尾のオフセットテーブルから引く。
long opur_dict_entry_offset(OpurDict *d, uint32_t i) {
    uint32_t off;
    if (fseek(d->fp, d->table_offset + (long)i * 4, SEEK_SET) != 0) return -1;
    g_seeks++;
    if (read_u32le(d->fp, &off) != 0) return -1;
    return (long)off;
}

// pos のレコードを読む。次のレコードの位置を返す。失敗時 -1。
// surface が不要なら sbuf に NULL を渡す（読み飛ばす）。
long opur_dict_read_record(OpurDict *d, long pos,
                           char *rbuf, size_t *rlen,
                           char *sbuf, size_t *slen) {
    unsigned rn, sn;

    if (fseek(d->fp, pos, SEEK_SET) != 0) return -1;
    g_seeks++;

    if (read_u8(d->fp, &rn) != 0) return -1;
    if (read_exact(d->fp, rbuf, rn) != 0) return -1;
    rbuf[rn] = '\0';
    *rlen = rn;

    if (read_u8(d->fp, &sn) != 0) return -1;
    if (sbuf) {
        if (read_exact(d->fp, sbuf, sn) != 0) return -1;
        sbuf[sn] = '\0';
        if (slen) *slen = sn;
    } else {
        if (fseek(d->fp, (long)sn, SEEK_CUR) != 0) return -1;
    }

    return pos + 1 + (long)rn + 1 + (long)sn;
}

// 内部からは短い名前で呼ぶ（原典のコードをそのまま保つため）
#define entry_offset opur_dict_entry_offset
#define read_record  opur_dict_read_record

// UTF-8 バイト列の辞書順比較。build_dict.py の
// `entries.sort(key=lambda e: e[0].encode("utf-8"))` と同じ順序でなければならない。
// Python の bytes 比較は「共通部分が同じなら短いほうが小さい」。
static int bytecmp(const char *a, size_t alen, const char *b, size_t blen) {
    size_t n = alen < blen ? alen : blen;
    int c = memcmp(a, b, n);
    if (c != 0) return c;
    if (alen == blen) return 0;
    return alen < blen ? -1 : 1;
}

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------

int opur_dict_open(OpurDict *dict, const char *path) {
    char magic[4];
    unsigned char ver[2];
    long fsize;

    memset(dict, 0, sizeof(*dict));
    dict->last_pos = -1;
    dict->last_result = -1;

    dict->fp = fopen(path, "rb");
    if (!dict->fp) return -1;

    if (read_exact(dict->fp, magic, 4) != 0) return -1;
    if (memcmp(magic, OPUR_DICT_MAGIC, 4) != 0) return -2;
    if (read_exact(dict->fp, ver, 2) != 0) return -1;
    if ((ver[0] | (ver[1] << 8)) != OPUR_DICT_VERSION) return -3;
    if (read_u32le(dict->fp, &dict->entry_count) != 0) return -1;

    if (fseek(dict->fp, 0, SEEK_END) != 0) return -1;
    fsize = ftell(dict->fp);
    dict->table_offset = fsize - (long)dict->entry_count * 4;
    if (dict->table_offset < HEADER_SIZE) return -4;

    return 0;
}

void opur_dict_close(OpurDict *dict) {
    if (dict->fp) fclose(dict->fp);
    dict->fp = NULL;
}

// ---------------------------------------------------------------------------
// 検索
// ---------------------------------------------------------------------------

// target 以上の読みを持つ最初のレコードの index（lower_bound）。
static uint32_t lower_bound(OpurDict *d, const char *target, size_t tlen) {
    uint32_t lo = 0, hi = d->entry_count;
    char rbuf[FIELD_MAX];
    size_t rlen;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        long pos = entry_offset(d, mid);
        if (pos < 0) break;
        if (read_record(d, pos, rbuf, &rlen, NULL, NULL) < 0) break;
        if (bytecmp(rbuf, rlen, target, tlen) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// pos から読みが target と一致するレコードを順に返す。ヒット数を返す。
static int emit_from(OpurDict *d, long pos, const char *target, size_t tlen,
                     DictHitCallback cb, void *ctx) {
    char rbuf[FIELD_MAX], sbuf[FIELD_MAX];
    size_t rlen, slen;
    int hits = 0;

    while (pos >= 0 && pos < d->table_offset) {
        long next = read_record(d, pos, rbuf, &rlen, sbuf, &slen);
        if (next < 0) break;
        if (bytecmp(rbuf, rlen, target, tlen) != 0) break;
        if (hits == 0) d->last_pos = pos;
        hits++;
        if (cb && cb(rbuf, sbuf, ctx) != 0) break;
        pos = next;
    }
    return hits;
}

int opur_dict_search(OpurDict *dict, const char *reading, long hint_pos,
                     DictHitCallback cb, void *ctx) {
    size_t tlen = strlen(reading);
    char hbuf[FIELD_MAX];
    size_t hlen;
    int hits = -1;
    long seeks0 = g_seeks;

    g_searches++;

    // --- 前回位置の再利用 ---
    // ※ 既定では使わない経路。素のバイナリサーチのほうが速いと実測で判明した
    //    （同一前方一致の読みが数百件並ぶ帯を線形走査するため。microSD では
    //    シーク 1 回のコストが高いのでさらに不利）。
    //    正しさは検証済みなので、将来インデックスを持たせたときの土台として残す。
    //
    // hint の読みが target の前方一致になっているときだけ使える。
    // ソート済みなので、hint 位置から前方へ線形走査すれば必ず target の
    // 位置に到達する（途中で target を追い越したらその読みは存在しない）。
    if (hint_pos >= HEADER_SIZE && hint_pos < dict->table_offset) {
        if (read_record(dict, hint_pos, hbuf, &hlen, NULL, NULL) >= 0) {
            if (tlen >= hlen && memcmp(reading, hbuf, hlen) == 0) {
                long pos = hint_pos;
                char rbuf[FIELD_MAX];
                size_t rlen;
                hits = 0;
                while (pos >= 0 && pos < dict->table_offset) {
                    long next = read_record(dict, pos, rbuf, &rlen, NULL, NULL);
                    int c;
                    if (next < 0) break;
                    c = bytecmp(rbuf, rlen, reading, tlen);
                    if (c == 0) {
                        hits = emit_from(dict, pos, reading, tlen, cb, ctx);
                        break;
                    }
                    if (c > 0) break;   // 追い越した = 存在しない
                    // ここに来るレコードは必ず hint の前方一致になっているはず。
                    // 念のため確認し、外れていたらバイナリサーチにフォールバック。
                    if (rlen < hlen || memcmp(rbuf, hbuf, hlen) != 0) {
                        hits = -1;
                        break;
                    }
                    pos = next;
                }
            }
        }
    }

    if (hits >= 0) {
        g_hint_searches++;
        g_hint_seeks += g_seeks - seeks0;
    }

    // --- 通常のバイナリサーチ ---
    if (hits < 0) {
        uint32_t idx = lower_bound(dict, reading, tlen);
        g_bs_searches++;
        if (idx >= dict->entry_count) {
            hits = 0;
        } else {
            long pos = entry_offset(dict, idx);
            hits = (pos < 0) ? 0 : emit_from(dict, pos, reading, tlen, cb, ctx);
        }
        g_bs_seeks += g_seeks - seeks0;
    }

    dict->last_result = (hits > 0) ? 0 : -1;
    if (hits == 0) dict->last_pos = -1;
    return hits;
}

// ---------------------------------------------------------------------------
// UTF-8 ユーティリティ
// ---------------------------------------------------------------------------

static int u8_is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

int opur_u8_count(const char *s) {
    int n = 0;
    for (; *s; s++) if (!u8_is_cont((unsigned char)*s)) n++;
    return n;
}

size_t opur_u8_bytes(const char *s, int nchars) {
    const char *p = s;
    int n = 0;
    while (*p) {
        if (!u8_is_cont((unsigned char)*p)) {
            if (n == nchars) break;
            n++;
        }
        p++;
    }
    return (size_t)(p - s);
}
