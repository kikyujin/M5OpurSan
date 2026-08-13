#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_dict.py — M5OpurSan 用バイナリ辞書ビルダー

cannadic / ipadic（natume 同梱、すべて EUC-JP）から (読み, 表記) を抽出し、
読み順にソートしたバイナリ辞書 system.dic と統計 stats.txt を出力する。

  usage: python3 build_dict.py [--natume DIR] [--out DIR]
"""

import argparse
import os
import re
import sys
from collections import Counter, defaultdict

# ---------------------------------------------------------------------------
# 定数
# ---------------------------------------------------------------------------

MAGIC = b"OPUR"
VERSION = 1
HEADER_SIZE = 16
MAX_FIELD_BYTES = 255  # reading_len / surface_len が uint8 のため

SRC_ENCODING = "euc_jp"

# ---------------------------------------------------------------------------
# かなユーティリティ
# ---------------------------------------------------------------------------

def kata_to_hira(s):
    """カタカナ → ひらがな。長音符「ー」やそれ以外の文字はそのまま通す。"""
    out = []
    for ch in s:
        c = ord(ch)
        if 0x30A1 <= c <= 0x30F6:
            out.append(chr(c - 0x60))
        else:
            out.append(ch)
    return "".join(out)


def is_all_hiragana(s):
    return all(0x3041 <= ord(ch) <= 0x3096 for ch in s)


# ---------------------------------------------------------------------------
# 活用型テーブル
# ---------------------------------------------------------------------------
#
# 各活用型を (strip, [(読み語尾, 表記語尾), ...]) で表す。
# strip = 見出し語・読みの末尾から取り除く文字数。
# ほとんどの型は読みと表記で同じ語尾が付くので、文字列 1 本から生成する。

def _same(strip, suffixes):
    return (strip, [(s, s) for s in suffixes])


# 五段: 語尾 1 文字を各段に置換。末尾の 1 文字は音便形（タ接続）。
_GODAN = {
    "五段・カ行イ音便":       "かきくけこい",
    "五段・カ行促音便":       "かきくけこっ",
    "五段・カ行促音便ユク":   "かきくけこっ",
    "五段・ガ行":             "がぎぐげごい",
    "五段・サ行":             "さしすせそ",
    "五段・タ行":             "たちつてとっ",
    "五段・ナ行":             "なにぬねのん",
    "五段・バ行":             "ばびぶべぼん",
    "五段・マ行":             "まみむめもん",
    "五段・ラ行":             "らりるれろっ",
    "五段・ラ行特殊":         "らりるれろっ",
    "五段・ワ行促音便":       "わいうえおっ",
    "五段・ワ行ウ音便":       "わいうえおう",
}

# 四段（文語）: 音便なし。
_YODAN = {
    "四段・ハ行": "はひふへほ",
    "四段・タ行": "たちつてと",
    "四段・サ行": "さしすせそ",
    "四段・バ行": "ばびぶべぼ",
}

CONJ = {}

for _name, _row in _GODAN.items():
    CONJ[_name] = _same(1, list(_row))
for _name, _row in _YODAN.items():
    CONJ[_name] = _same(1, list(_row))

# 一段: 「る」を落として語幹。命令形は ろ / よ の両方。
for _name in ("一段", "一段・得ル", "一段・クレル"):
    CONJ[_name] = _same(1, ["", "る", "れ", "ろ", "よ"])

# 形容詞
for _name in ("形容詞・アウオ段", "形容詞・イ段"):
    CONJ[_name] = _same(1, ["い", "く", "かっ", "けれ", "かろ"])
CONJ["形容詞・イイ"] = _same(2, ["いい", "よく", "よかっ", "よけれ", "よかろ"])

# 下二・上二（文語）
CONJ["下二・カ行"] = _same(1, ["け", "く", "くる", "くれ", "けよ"])
CONJ["下二・ガ行"] = _same(1, ["げ", "ぐ", "ぐる", "ぐれ", "げよ"])
CONJ["下二・タ行"] = _same(1, ["て", "つ", "つる", "つれ", "てよ"])
CONJ["下二・ダ行"] = _same(1, ["で", "づ", "づる", "づれ", "でよ"])
CONJ["下二・ハ行"] = _same(1, ["へ", "ふ", "ふる", "ふれ", "へよ"])
CONJ["下二・マ行"] = _same(1, ["め", "む", "むる", "むれ", "めよ"])
CONJ["下二・得"]   = _same(1, ["え", "う", "うる", "うれ", "えよ"])
CONJ["上二・ハ行"] = _same(1, ["ひ", "ふ", "ふる", "ふれ", "ひよ"])
CONJ["上二・ダ行"] = _same(1, ["ぢ", "づ", "づる", "づれ", "ぢよ"])
CONJ["ラ変"]       = _same(1, ["ら", "り", "る", "れ"])

# 「ある」専用。未然形が特殊なだけで残りは五段・ラ行と同じ。
CONJ["五段・ラ行アル"] = _same(1, ["ら", "り", "る", "れ", "ろ", "っ"])

# カ変・サ変は語幹の読みが変化するので、読みと表記を別々に並べる。
CONJ["カ変・来ル"] = (2, [("こ", "来"), ("き", "来"), ("くる", "来る"),
                          ("くれ", "来れ"), ("こい", "来い")])
CONJ["カ変・クル"] = (2, [("こ", "こ"), ("き", "き"), ("くる", "くる"),
                          ("くれ", "くれ"), ("こい", "こい")])
CONJ["サ変・スル"] = (2, [("し", "し"), ("す", "す"), ("する", "する"),
                          ("すれ", "すれ"), ("しろ", "しろ"), ("せよ", "せよ"),
                          ("さ", "さ"), ("せ", "せ")])
CONJ["サ変・−ズル"] = (2, [("じ", "じ"), ("ず", "ず"), ("ずる", "ずる"),
                            ("ずれ", "ずれ"), ("じろ", "じろ"), ("じよ", "じよ")])

# ipadic の活用型名には U+2212（MINUS SIGN）を使うものがある。
# 「サ変・−スル」がまさにそれで、ハイフン無しの「サ変・スル」とは別の文字列。
# 398 語がここに該当するので取りこぼすと影響が大きい。
CONJ["サ変・−スル"] = CONJ["サ変・スル"]

# 助動詞（特殊・*）と文語（文語・*）、および不変化型は展開しない。
# 合計 20 語程度で、いずれも単独で変換する場面がほぼ無いため。
# 見出し語はそのまま 1 エントリとして登録される。
def is_no_expand(name):
    return (name == "不変化型"
            or name.startswith("特殊・")
            or name.startswith("文語・"))

# ---------------------------------------------------------------------------
# 収集器
# ---------------------------------------------------------------------------

class Collector:
    def __init__(self):
        self.pairs = []            # [(reading, surface, source)]
        self.by_source = Counter()
        self.dropped = Counter()

    def add(self, reading, surface, source):
        if not reading or not surface:
            self.dropped["空の読み/表記"] += 1
            return
        if len(reading.encode("utf-8")) > MAX_FIELD_BYTES:
            self.dropped["読みが 255 バイト超"] += 1
            return
        if len(surface.encode("utf-8")) > MAX_FIELD_BYTES:
            self.dropped["表記が 255 バイト超"] += 1
            return
        self.pairs.append((reading, surface, source))
        self.by_source[source] += 1


# ---------------------------------------------------------------------------
# cannadic パーサ
# ---------------------------------------------------------------------------

def read_lines(path):
    with open(path, "rb") as f:
        raw = f.read()
    return raw.decode(SRC_ENCODING, errors="replace").splitlines()


def parse_cannadic(path, source, col):
    """`読み #品詞*頻度 表記1 表記2 …` 形式。# トークンは読み飛ばす。"""
    for line in read_lines(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        tok = line.split()
        if len(tok) < 2:
            continue
        reading = tok[0]
        for t in tok[1:]:
            if t.startswith("#"):
                continue  # 品詞・頻度タグ
            col.add(reading, t, source)


def split_okuri(reading, kanji, ref=None):
    """送りがなを決める。

    読みの末尾 N 文字を送りがな候補とし、漢字側に読みが 1 文字以上残る
    最小の N を採用する（発注書の決定ルール 1〜3）。

    ただし N=1 を機械的に採ると「あかす + 明 → 明す」「たべる + 食 → 食る」の
    ように誤った表記を作ってしまう。240x135 の画面では候補バーに並ぶゴミが
    そのまま邪魔になるため、ref（他ソース由来の (読み, 表記) 集合）に照合して、
    実在する表記が見つかる最小の N を採る。
    どの N も照合できないときだけ N=1 にフォールバックする。

    決まらなければ None を返す（呼び出し側でフォールバック）。
    """
    cands = []
    for n in range(1, len(reading)):
        if len(reading) - n >= 1:
            cands.append(kanji + reading[-n:])
    if not cands:
        return None, "none"
    if ref is not None:
        for i, c in enumerate(cands):
            if (reading, c) in ref:
                return c, ("n1" if i == 0 else "extended")
    return cands[0], "unverified"


def parse_gt_okuri(path, source, col, stats, ref):
    """`読みt #品詞*頻度 漢字1 漢字2 …` 形式。漢字は語幹 1 文字。

    ref は先に読み込んだ他ソースの (読み, 表記) 集合。送りがなの検証に使う。
    """
    for line in read_lines(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        tok = line.split()
        if len(tok) < 2:
            continue
        reading = tok[0]
        if reading.endswith("t"):
            reading = reading[:-1]   # 送りがな付きマーカー
        if not reading:
            continue
        for t in tok[1:]:
            if t.startswith("#"):
                continue
            surface, how = split_okuri(reading, t, ref)
            if surface is None:
                # 判定不能 → 読み全体を表記としてフォールバック
                surface = reading
                stats["okuri_fallback"] += 1
            elif how == "n1":
                stats["okuri_n1"] += 1
            elif how == "extended":
                stats["okuri_extended"] += 1
            else:
                stats["okuri_unverified"] += 1
            stats["okuri_ok"] += 1
            col.add(reading, surface, source)


# ---------------------------------------------------------------------------
# ipadic パーサ
# ---------------------------------------------------------------------------

RE_POS     = re.compile(r"\(品詞\s+\(([^)]*)\)\)")
RE_SURFACE = re.compile(r"\(見出し語\s+\((\S+)\s+-?\d+\)\)")
RE_READING = re.compile(r"\(読み\s+(\S+?)\)")
RE_CONJ    = re.compile(r"\(活用型\s+(\S+?)\)")


def expand_conj(reading, surface, conj_name, stats):
    """活用型に従って (読み, 表記) を展開する。展開できなければ元の 1 件のみ。"""
    rule = CONJ.get(conj_name)
    if rule is None:
        if is_no_expand(conj_name):
            stats["conj_no_expand"][conj_name] += 1
        elif conj_name:
            stats["conj_unknown"][conj_name] += 1
        return [(reading, surface)]

    strip, suffixes = rule
    if len(reading) <= strip or len(surface) <= strip:
        # 語幹が残らない（「する」「くる」単体など）→ 語幹は空文字で展開する
        if len(reading) < strip or len(surface) < strip:
            stats["conj_too_short"] += 1
            return [(reading, surface)]

    r_stem = reading[:len(reading) - strip]
    s_stem = surface[:len(surface) - strip]

    out = []
    for r_suf, s_suf in suffixes:
        out.append((r_stem + r_suf, s_stem + s_suf))
    stats["conj_expanded"] += 1
    return out


def parse_ipadic(path, source, col, stats):
    for line in read_lines(path):
        line = line.strip()
        if not line:
            continue
        m_s = RE_SURFACE.search(line)
        m_r = RE_READING.search(line)
        if not m_s or not m_r:
            continue
        surface = m_s.group(1)
        reading = kata_to_hira(m_r.group(1))
        m_c = RE_CONJ.search(line)
        conj = m_c.group(1) if m_c else ""

        if conj:
            for r, s in expand_conj(reading, surface, conj, stats):
                col.add(r, s, source)
        else:
            col.add(reading, surface, source)


# ---------------------------------------------------------------------------
# バイナリ出力
# ---------------------------------------------------------------------------

def write_dic(path, entries):
    """entries: [(reading, surface)] を読み順ソート済みで受け取る。

    レイアウト:
      [ヘッダ 16 bytes]
      [レコード群 … 可変長]
      [オフセットテーブル: entry_count × uint32 LE]
    """
    offsets = []
    body = bytearray()
    pos = HEADER_SIZE

    for reading, surface in entries:
        rb = reading.encode("utf-8")
        sb = surface.encode("utf-8")
        offsets.append(pos)
        body.append(len(rb))
        body += rb
        body.append(len(sb))
        body += sb
        pos += 1 + len(rb) + 1 + len(sb)

    header = bytearray()
    header += MAGIC
    header += VERSION.to_bytes(2, "little")
    header += len(entries).to_bytes(4, "little")
    header += b"\x00" * 6
    assert len(header) == HEADER_SIZE

    table = bytearray()
    for off in offsets:
        table += off.to_bytes(4, "little")

    with open(path, "wb") as f:
        f.write(header)
        f.write(body)
        f.write(table)

    return HEADER_SIZE + len(body) + len(table), len(body), len(table)


# ---------------------------------------------------------------------------
# 統計
# ---------------------------------------------------------------------------

def write_stats(path, entries, by_source, dropped, dup_removed, sizes, stats):
    total_size, body_size, table_size = sizes

    len_dist = Counter(len(r) for r, _ in entries)
    head_dist = Counter(r[0] for r, _ in entries if r)

    cand = defaultdict(int)
    for r, _ in entries:
        cand[r] += 1
    top = sorted(cand.items(), key=lambda kv: (-kv[1], kv[0]))[:10]

    non_hira = sum(1 for r, _ in entries if not is_all_hiragana(r))

    lines = []
    ap = lines.append

    ap("=== M5OpurSan system.dic 統計 ===")
    ap("")
    ap("--- 全体 ---")
    ap("総エントリ数           : {:,}".format(len(entries)))
    ap("ファイルサイズ         : {:,} bytes ({:.2f} MB)".format(
        total_size, total_size / 1024 / 1024))
    ap("  ヘッダ               : {} bytes".format(HEADER_SIZE))
    ap("  レコード群           : {:,} bytes".format(body_size))
    ap("  オフセットテーブル   : {:,} bytes".format(table_size))
    ap("重複除去で消えた件数   : {:,}".format(dup_removed))
    ap("")

    ap("--- ソース別エントリ数（重複除去前）---")
    for src, n in sorted(by_source.items(), key=lambda kv: -kv[1]):
        ap("  {:<28} {:>9,}".format(src, n))
    ap("")

    if dropped:
        ap("--- 除外したエントリ ---")
        for reason, n in sorted(dropped.items(), key=lambda kv: -kv[1]):
            ap("  {:<28} {:>9,}".format(reason, n))
        ap("")

    ap("--- 展開処理 ---")
    ap("  gt_okuri 送りがな決定    : {:,}".format(stats["okuri_ok"]))
    ap("    N=1 が他ソースと一致   : {:,}".format(stats["okuri_n1"]))
    ap("    N を伸ばして一致       : {:,}  ← N=1 だと誤表記になっていた分".format(
        stats["okuri_extended"]))
    ap("    照合できず N=1 を採用  : {:,}".format(stats["okuri_unverified"]))
    ap("  gt_okuri フォールバック  : {:,}".format(stats["okuri_fallback"]))
    ap("  ipadic 活用展開した語    : {:,}".format(stats["conj_expanded"]))
    ap("  ipadic 語幹不足でスキップ: {:,}".format(stats["conj_too_short"]))
    if stats["conj_no_expand"]:
        n_tot = sum(stats["conj_no_expand"].values())
        ap("  展開しない活用型（助動詞・文語・不変化型、計 {:,} 語）:".format(n_tot))
        for name, n in sorted(stats["conj_no_expand"].items(), key=lambda kv: -kv[1]):
            ap("    {:<26} {:>9,}".format(name, n))
    if stats["conj_unknown"]:
        ap("  ★未対応の活用型（要対応）:")
        for name, n in sorted(stats["conj_unknown"].items(), key=lambda kv: -kv[1]):
            ap("    {:<26} {:>9,}".format(name, n))
    else:
        ap("  未対応の活用型          : なし")
    ap("")

    ap("--- 同一読みの候補数 Top 10 ---")
    for r, n in top:
        ap("  {:<20} {:>6,} 候補".format(r, n))
    ap("")

    ap("--- 読みの長さ分布（文字数）---")
    for n in sorted(len_dist):
        ap("  {:>3} 文字 : {:>9,}".format(n, len_dist[n]))
    ap("")

    ap("--- 先頭かな別エントリ数 ---")
    for ch, n in sorted(head_dist.items(), key=lambda kv: -kv[1]):
        ap("  {:<4} {:>9,}".format(ch, n))
    ap("")

    ap("--- 読みの文字種 ---")
    ap("ひらがな以外を含む読み : {:,} ({:.2f}%)".format(
        non_hira, 100.0 * non_hira / max(1, len(entries))))
    ap("  ※ 長音符「ー」など。UTF-8 バイト順のソートでは、ひらがなより")
    ap("     後ろに並ぶ。検索側も同じ順序で比較する限り正しく引ける。")
    ap("")

    ap("--- 辞書の先頭と末尾 ---")
    if entries:
        ap("  先頭 : {} → {}".format(entries[0][0], entries[0][1]))
        ap("  末尾 : {} → {}".format(entries[-1][0], entries[-1][1]))

    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--natume", default=os.path.join(here, "natume"))
    ap.add_argument("--out", default=os.path.join(here, "output"))
    args = ap.parse_args()

    cannadir = os.path.join(args.natume, "alt-cannadic-110208")
    ipadir = os.path.join(args.natume, "ipadic-2.7.0")

    if not os.path.isdir(cannadir):
        sys.exit("素材が見つからない: {}\n  make build-dict で clone される".format(cannadir))

    os.makedirs(args.out, exist_ok=True)

    col = Collector()
    stats = {
        "okuri_ok": 0,
        "okuri_fallback": 0,
        "okuri_n1": 0,
        "okuri_extended": 0,
        "okuri_unverified": 0,
        "conj_expanded": 0,
        "conj_too_short": 0,
        "conj_unknown": Counter(),
        "conj_no_expand": Counter(),
    }

    # --- Step 1: 素材読み込み ---
    print("[1/5] 素材を読み込み中…", file=sys.stderr)

    for fname, src in (("gcanna.ctd", "cannadic/gcanna"),
                       ("gtankan.ctd", "cannadic/gtankan"),
                       ("g_fname.ctd", "cannadic/g_fname")):
        path = os.path.join(cannadir, fname)
        parse_cannadic(path, src, col)
        print("      {:<14} {:>9,}".format(fname, col.by_source[src]), file=sys.stderr)

    for fname in sorted(os.listdir(ipadir)):
        if not fname.endswith(".dic"):
            continue
        src = "ipadic/" + fname[:-4]
        parse_ipadic(os.path.join(ipadir, fname), src, col, stats)
    n_ipa = sum(v for k, v in col.by_source.items() if k.startswith("ipadic/"))
    print("      {:<14} {:>9,}".format("ipadic/*.dic", n_ipa), file=sys.stderr)

    # gt_okuri は最後に読む。送りがなの決定に、ここまでに集めた
    # (読み, 表記) を正解データとして使うため。
    ref = set((r, s) for r, s, _ in col.pairs)
    parse_gt_okuri(os.path.join(cannadir, "gt_okuri.ctd"),
                   "cannadic/gt_okuri", col, stats, ref)
    print("      {:<14} {:>9,}".format("gt_okuri.ctd",
          col.by_source["cannadic/gt_okuri"]), file=sys.stderr)

    # --- Step 2: 正規化・重複除去 ---
    print("[2/5] 重複除去中… (raw {:,})".format(len(col.pairs)), file=sys.stderr)
    seen = set()
    entries = []
    for reading, surface, _src in col.pairs:
        key = (reading, surface)
        if key in seen:
            continue
        seen.add(key)
        entries.append(key)
    dup_removed = len(col.pairs) - len(entries)
    print("      {:,} 件（重複 {:,} 件を除去）".format(len(entries), dup_removed),
          file=sys.stderr)

    # --- Step 3: 読みでソート（UTF-8 バイト列の辞書順、同一読みは挿入順を保持）---
    print("[3/5] ソート中…", file=sys.stderr)
    entries.sort(key=lambda e: e[0].encode("utf-8"))

    # --- Step 4: バイナリ出力 ---
    dic_path = os.path.join(args.out, "system.dic")
    print("[4/5] {} を書き出し中…".format(dic_path), file=sys.stderr)
    sizes = write_dic(dic_path, entries)
    print("      {:,} bytes ({:.2f} MB)".format(sizes[0], sizes[0] / 1024 / 1024),
          file=sys.stderr)

    # --- Step 5: 統計 ---
    stats_path = os.path.join(args.out, "stats.txt")
    print("[5/5] {} を書き出し中…".format(stats_path), file=sys.stderr)
    write_stats(stats_path, entries, col.by_source, col.dropped,
                dup_removed, sizes, stats)

    print("完了: {:,} エントリ".format(len(entries)), file=sys.stderr)


if __name__ == "__main__":
    main()
