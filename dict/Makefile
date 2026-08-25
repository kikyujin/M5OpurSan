# dict — M5OpurSan 辞書ビルドパイプライン
#
#   make build-dict         natume を clone（未取得なら）→ system.dic + stats.txt 生成
#   make build-dict-legacy  021 までの旧方式で system_legacy.dic を作る（比較用）
#   make clean              生成物削除（natume の clone は消さない）
#   make distclean          natume の clone も消す
#
# system.dic は git 追跡下にある。**build-dict の中身を変えないこと。**
# 別方式の辞書は build-dict-legacy のように必ず別名で出すこと。
#
# **ここは GPL 素材（natume / alt-cannadic）を使う側**。辞書を「引く」コード
# （opur_dict.{h,c}）と、そのテスト（test_dict.c）は MIT 側の ../opur_editor/
# にある。辞書引きテストは `make -C ../opur_editor test-dict`。

PYTHON ?= python3

NATUME_URL := https://github.com/takayuki/natume.git
NATUME_DIR := natume
OUT_DIR    := output
DIC        := $(OUT_DIR)/system.dic
STATS      := $(OUT_DIR)/stats.txt
LEGACY_DIC := $(OUT_DIR)/system_legacy.dic

.PHONY: all build-dict build-dict-legacy clean distclean

all: build-dict

# --- 素材取得 ---
$(NATUME_DIR)/alt-cannadic-110208:
	@echo "natume を clone 中… ($(NATUME_URL))"
	git clone --depth 1 $(NATUME_URL) $(NATUME_DIR)

# --- 辞書ビルド ---
build-dict: $(DIC)

$(DIC) $(STATS): build_dict.py | $(NATUME_DIR)/alt-cannadic-110208
	$(PYTHON) build_dict.py

# 021 までの同梱辞書と同じ作り方（手書き CONJ テーブル・付属語なし）。
# 活用語尾までしか作れないので「いった → 言った」は引けない。13.5MB。
# 新旧を突き合わせたいときだけ使う。同梱するのは system.dic のほう。
build-dict-legacy: $(LEGACY_DIC)

$(LEGACY_DIC): build_dict.py | $(NATUME_DIR)/alt-cannadic-110208
	$(PYTHON) build_dict.py --conj legacy --name system_legacy.dic

clean:
	rm -f $(DIC) $(STATS) $(LEGACY_DIC) $(OUT_DIR)/system_legacy_stats.txt
	-rmdir $(OUT_DIR) 2>/dev/null || true

distclean: clean
	rm -rf $(NATUME_DIR)
