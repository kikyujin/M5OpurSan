#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_dict.py — M5OpurSan 用バイナリ辞書ビルダー

cannadic / ipadic（natume 同梱、すべて EUC-JP）から (読み, 表記) を抽出し、
読み順にソートしたバイナリ辞書 system.dic と統計 stats.txt を出力する。

  usage: python3 build_dict.py [--natume DIR] [--out DIR]
                               [--conj {legacy,cforms}] [--aux {none,min,std}]
                               [--name FILE]

用言の展開方式は 2 つある。

  --conj legacy  このファイルに手書きした CONJ テーブルで展開する（既定）。
                 同梱の system.dic はこれで作った。**既定は変えないこと。**
                 system.dic は git 追跡下にあるので、うっかり別方式で
                 上書きすると 14MB の差分が出る。
  --conj cforms  ipadic-2.7.0/cforms.cha（活用型 × 活用形の語尾テーブル）を
                 読んで展開する。legacy より形態が多く、**活用形名が取れる**ので
                 --aux で付属語を繋げられる。

  --aux none     付属語を繋がない（既定）
  --aux min      た / て / ます / ない だけ
  --aux std      助動詞・接続助詞をひととおり（AUX_STD を参照）

別方式で作るときは --name で出力名を変えること:

  python3 build_dict.py --conj cforms --aux std --name system_conj.dic
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
# cforms.cha 方式（--conj cforms）
# ---------------------------------------------------------------------------
#
# ipadic-2.7.0/cforms.cha は活用型 × 活用形の語尾テーブル。
# 表記語尾と読み語尾が別カラムで揃っているので、カ変「来る」のように
# 読みと表記がずれる型もそのまま扱える。
#
#   (五段・カ行イ音便
#       (  (基本形 く ク) (未然形 か カ) (連用形 き キ)
#          (連用タ接続 い イ) (仮定形 け ケ) … ))
#
# 上の手書き CONJ との違いは 2 つ。
#   - 形態が多い（72 活用型 / 478 形態。手書きは 1 型あたり 5〜6 語尾）
#   - **活用形名が取れる**。「言っ」が連用タ接続だと分かるので、
#     そこにだけ「た」を繋げられる。付属語連接はこれが前提。

def parse_sexp(text):
    """cforms.cha 用の最小 S 式パーサ。`;` から行末まではコメント。"""
    text = re.sub(r";[^\n]*", " ", text)
    stack = [[]]
    for tok in re.findall(r"\(|\)|[^\s()]+", text):
        if tok == "(":
            stack.append([])
        elif tok == ")":
            node = stack.pop()
            stack[-1].append(node)
        else:
            stack[-1].append(tok)
    return stack[0]


# cforms.cha は「語尾が無い」ことを `*` で書く（一段の `(未然形 *)` など）。
# そのまま連結すると「食べ*」のようなゴミになるので空文字に潰す。
def _star(x):
    return "" if x == "*" else x


# 語幹そのものが未然形・連用形になる型は、cforms.cha にその行すら無いことがある
# （一段・得ル / 一段・ル など）。空語尾で補う。
_STEM_IS_FORM = ("一段", "下二", "上二")

# cforms.cha の取りこぼしを補う。手書き CONJ にはあった形なので、
# 補わないと legacy より候補が減ってしまう。
#   サ変・−スル(398 語) / サ変・−ズル(122 語) は連用形の行が無い。
#   「察し」「感じ」が作れず、連用タ接続のフォールバック元も消える。
#   サ変・−スル は未然レル接続が「せ」だけで「さ」が無い。286 語ぶんの
#   「愛さ」「察さ」が消え、受身・使役（愛される／愛させる）も作れない。
#   サ変・−ズル と 一段・得ル は命令形の行が無い（感じろ／得よ）。
#
# 逆に、legacy にあって cforms に無くても**補わない**ものもある:
#   五段・ラ行特殊の連用形「り」（13 語）… cforms は「い」。
#     「いらっしゃります」ではなく「いらっしゃいます」が正しいので cforms が正しい
#   四段のオ段「ほ/と/そ/ぼ」（15 語）… 文語の未然ウ接続（思はう）。要らない
_FORM_PATCH = {
    "サ変・−スル": [("連用形", "し", "し"),
                    ("未然レル接続", "さ", "さ")],
    "サ変・−ズル": [("連用形", "じ", "じ"),
                    ("命令ｒｏ", "じろ", "じろ"),
                    ("命令ｙｏ", "じよ", "じよ")],
    "一段・得ル":   [("命令ｙｏ", "よ", "よ"),
                    ("命令ｒｏ", "ろ", "ろ")],
}

# 連用タ接続の行が無い型は連用形で代用する。
#   五段・サ行  し + た  → 話した   ✓
#   一段        （空）+ た → 食べた  ✓
#   サ変・スル  し + た  → した     ✓
# ただし「ゆく」だけは連用形（き）では「ゆきた」になってしまう。
# ipadic 自身が型名を促音便と呼んでいるので、手書き CONJ と同じ「っ」を充てる。
_TA_OVERRIDE = {"五段・カ行促音便ユク": ("っ", "っ")}


def load_cforms(path):
    """cforms.cha を読む。{活用型: [(活用形名, 表記語尾, 読み語尾)]}"""
    with open(path, "rb") as f:
        text = f.read().decode(SRC_ENCODING, errors="replace")

    table = {}
    for node in parse_sexp(text):
        if not isinstance(node, list) or len(node) < 2:
            continue
        rows = []
        for group in node[1:]:
            if not isinstance(group, list):
                continue
            for row in group:
                # 4 カラムの行（特殊・ナイ の音便基本形など）は 4 つ目が発音。使わない。
                if isinstance(row, list) and len(row) >= 3:
                    rows.append((row[0], _star(row[1]), _star(kata_to_hira(row[2]))))
                elif isinstance(row, list) and len(row) == 2:
                    # `(未然形 *)` のように 1 カラムしか無い行。語尾が空という意味。
                    rows.append((row[0], _star(row[1]), _star(row[1])))
        if rows:
            table[node[0]] = rows

    for name, rows in table.items():
        have = set(r[0] for r in rows)
        # 同じ活用形名で別の語尾が既にあることがある（サ変・−スルの未然レル接続は
        # 「せ」がある）ので、名前だけでなく語尾込みで重複を見る。
        for patch in _FORM_PATCH.get(name, ()):
            if patch not in rows:
                rows.append(patch)
        have = set(r[0] for r in rows)
        if name.startswith(_STEM_IS_FORM):
            for f in ("未然形", "連用形"):
                if f not in have:
                    rows.append((f, "", ""))
            have = set(r[0] for r in rows)
        if "連用タ接続" not in have:
            ov = _TA_OVERRIDE.get(name)
            if ov:
                rows.append(("連用タ接続", ov[0], ov[1]))
            else:
                for f, fs, fr in list(rows):
                    if f == "連用形":
                        rows.append(("連用タ接続", fs, fr))

    return table


# ---------------------------------------------------------------------------
# 付属語連接テーブル（--aux）
# ---------------------------------------------------------------------------
#
# 活用形名をキーに、その形に繋がる付属語（助動詞・接続助詞）を並べる。
# 「言った」が引けないのはここが無かったからで、案A の本体はこの表。

# 連用タ接続に「だ/で」が付く型（撥音便・イ音便のうち濁るもの）。
#   読んだ・飛んで・泳いだ。「い」でもガ行だけは濁る。
VOICED_CONJ = ("五段・ガ行", "五段・ナ行", "五段・バ行", "五段・マ行")

# 受身・使役。五段は れる/せる、一段系は られる/させる。
# サ変は未然レル接続（さ）が別にあるので未然形には付けない。
_ICHIDAN_LIKE = ("一段", "下二", "上二", "カ変")

AUX_MIN = {
    "連用タ接続": ["た", "て"],
    "連用形":     ["ます"],
    "未然形":     ["ない"],
}

AUX_STD = {
    "連用タ接続":     ["た", "たら", "たり", "て", "ても", "てる", "ている",
                       "てください"],
    "連用テ接続":     ["て", "ても"],          # 形容詞の「高くて」
    "連用形":         ["ます", "ました", "ません", "まして", "ましょう",
                       "たい", "たく", "たかった", "ながら", "そう"],
    "未然形":         ["ない", "なかった", "なく", "なくて"],
    "未然レル接続":   ["れる", "れた", "せる", "せた"],   # サ変の される/させる
    "未然ヌ接続":     ["ぬ", "ん"],
    "未然ウ接続":     ["う"],
    "仮定形":         ["ば"],
}

# 濁音便のとき「た」→「だ」に振り替える対応。
_VOICE = {"た": "だ", "たら": "だら", "たり": "だり", "て": "で", "ても": "でも",
          "てる": "でる", "ている": "でいる", "てください": "でください"}


def aux_suffixes(form, conj, level):
    """活用形 form（活用型 conj）に繋げる付属語のリスト。"""
    if level == "none":
        return []
    table = AUX_MIN if level == "min" else AUX_STD
    out = list(table.get(form, ()))

    if level == "std" and form == "未然形":
        if conj.startswith(_ICHIDAN_LIKE):
            out += ["られる", "られた", "させる", "させた"]
        elif conj.startswith(("五段", "四段")):
            out += ["ず", "れる", "れた", "せる", "せた"]

    if form == "連用タ接続" and conj in VOICED_CONJ:
        out = [_VOICE.get(a, a) for a in out]
    return out


def expand_cforms(reading, surface, conj, cforms, level, stats):
    """cforms.cha に従って展開し、付属語も繋ぐ。[(読み, 表記)] を返す。"""
    rows = cforms.get(conj)
    if rows is None or is_no_expand(conj):
        if is_no_expand(conj):
            stats["conj_no_expand"][conj] += 1
        elif conj:
            stats["conj_unknown"][conj] += 1
        return [(reading, surface)]

    # 語幹 = 見出し語 － 基本形の語尾。ipadic 16,512 語すべてで剥がせることを確認済み。
    base = [r for r in rows if r[0] == "基本形"]
    if not base:
        stats["cforms_no_base"][conj] += 1
        return [(reading, surface)]
    _, b_s, b_r = base[0]
    if not surface.endswith(b_s) or not reading.endswith(b_r):
        stats["cforms_stem_mismatch"][conj] += 1
        return [(reading, surface)]

    s_stem = surface[:len(surface) - len(b_s)] if b_s else surface
    r_stem = reading[:len(reading) - len(b_r)] if b_r else reading

    out = []
    for fname, f_s, f_r in rows:
        out.append((r_stem + f_r, s_stem + f_s))
        stats["form_counts"][fname] += 1
        for a in aux_suffixes(fname, conj, level):
            out.append((r_stem + f_r + a, s_stem + f_s + a))
            stats["aux_counts"][a] += 1
    stats["conj_expanded"] += 1
    return out

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


def parse_ipadic(path, source, col, stats, expander=None):
    """expander は (読み, 表記, 活用型, stats) -> [(読み, 表記)]。
    省略時は手書き CONJ テーブルの expand_conj。"""
    if expander is None:
        expander = expand_conj
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
            for r, s in expander(reading, surface, conj, stats):
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

def write_stats(path, entries, by_source, dropped, dup_removed, sizes, stats,
                args=None):
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
    if args is not None:
        ap("--- ビルド設定 ---")
        ap("出力ファイル           : {}".format(args.name))
        ap("用言の展開方式         : --conj {}".format(args.conj))
        ap("付属語連接             : --aux {}".format(args.aux))
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
    if stats.get("cforms_stem_mismatch"):
        ap("  ★語幹を剥がせなかった語（基本形の語尾で終わっていない）:")
        for name, n in stats["cforms_stem_mismatch"].most_common():
            ap("    {:<26} {:>9,}".format(name, n))
    if stats.get("cforms_no_base"):
        ap("  ★cforms に基本形の行が無い活用型:")
        for name, n in stats["cforms_no_base"].most_common():
            ap("    {:<26} {:>9,}".format(name, n))
    if stats.get("form_counts"):
        ap("")
        ap("--- 活用形ごとの生成数（cforms 方式・重複除去前）---")
        for name, n in stats["form_counts"].most_common():
            ap("  {:<26} {:>9,}".format(name, n))
    if stats.get("aux_counts"):
        ap("")
        ap("--- 付属語ごとの生成数（重複除去前・計 {:,}）---".format(
            sum(stats["aux_counts"].values())))
        for name, n in stats["aux_counts"].most_common():
            ap("  {:<26} {:>9,}".format(name, n))
        ap("")
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
    ap.add_argument("--conj", choices=("legacy", "cforms"), default="legacy",
                    help="用言の展開方式（既定 legacy = 同梱 system.dic と同じ）")
    ap.add_argument("--aux", choices=("none", "min", "std"), default="none",
                    help="付属語連接の量。--conj cforms のときだけ効く")
    ap.add_argument("--name", default="system.dic",
                    help="出力ファイル名。統計は <ベース名>_stats.txt")
    args = ap.parse_args()

    if args.aux != "none" and args.conj != "cforms":
        sys.exit("--aux は --conj cforms とセットで使うこと"
                 "（legacy は活用形名を持たないので付属語を繋げない）")

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
        "cforms_no_base": Counter(),
        "cforms_stem_mismatch": Counter(),
        "form_counts": Counter(),
        "aux_counts": Counter(),
    }

    expander = None
    if args.conj == "cforms":
        cf_path = os.path.join(ipadir, "cforms.cha")
        if not os.path.isfile(cf_path):
            sys.exit("cforms.cha が見つからない: {}".format(cf_path))
        cforms = load_cforms(cf_path)
        print("      cforms.cha: {} 活用型 / {} 形態".format(
            len(cforms), sum(len(v) for v in cforms.values())), file=sys.stderr)

        def expander(reading, surface, conj, st,
                     _cf=cforms, _lv=args.aux):
            return expand_cforms(reading, surface, conj, _cf, _lv, st)

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
        parse_ipadic(os.path.join(ipadir, fname), src, col, stats, expander)
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
    dic_path = os.path.join(args.out, args.name)
    print("[4/5] {} を書き出し中…".format(dic_path), file=sys.stderr)
    sizes = write_dic(dic_path, entries)
    print("      {:,} bytes ({:.2f} MB)".format(sizes[0], sizes[0] / 1024 / 1024),
          file=sys.stderr)

    # --- Step 5: 統計 ---
    base = os.path.splitext(args.name)[0]
    stats_path = os.path.join(args.out,
                              "stats.txt" if base == "system"
                              else base + "_stats.txt")
    print("[5/5] {} を書き出し中…".format(stats_path), file=sys.stderr)
    write_stats(stats_path, entries, col.by_source, col.dropped,
                dup_removed, sizes, stats, args)

    print("完了: {:,} エントリ".format(len(entries)), file=sys.stderr)


if __name__ == "__main__":
    main()
