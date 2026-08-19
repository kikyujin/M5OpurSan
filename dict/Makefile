# dict — M5OpurSan 辞書ビルドパイプライン
#
#   make build-dict       natume を clone（未取得なら）→ system.dic + stats.txt 生成
#   make build-dict-conj  活用展開版 system_conj.dic を作る（本番辞書は触らない）
#   make clean            生成物削除（natume の clone は消さない）
#   make distclean        natume の clone も消す
#
# system.dic は git 追跡下にある。**build-dict の中身を変えないこと。**
# 別方式の辞書は build-dict-conj のように必ず別名で出すこと。
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
CONJ_DIC   := $(OUT_DIR)/system_conj.dic

.PHONY: all build-dict build-dict-conj clean distclean

all: build-dict

# --- 素材取得 ---
$(NATUME_DIR)/alt-cannadic-110208:
	@echo "natume を clone 中… ($(NATUME_URL))"
	git clone --depth 1 $(NATUME_URL) $(NATUME_DIR)

# --- 辞書ビルド ---
build-dict: $(DIC)

$(DIC) $(STATS): build_dict.py | $(NATUME_DIR)/alt-cannadic-110208
	$(PYTHON) build_dict.py

# 活用展開版。ipadic の cforms.cha で展開し、付属語（た・て・ます・ない…）まで
# 繋いだもの。「いった → 言った」が引ける代わりに 33MB になる。
# 本番と入れ替えるかはマスター判断なので、ここでは別名で出すだけ。
build-dict-conj: $(CONJ_DIC)

$(CONJ_DIC): build_dict.py | $(NATUME_DIR)/alt-cannadic-110208
	$(PYTHON) build_dict.py --conj cforms --aux std --name system_conj.dic

clean:
	rm -f $(DIC) $(STATS) $(CONJ_DIC) $(OUT_DIR)/system_conj_stats.txt
	-rmdir $(OUT_DIR) 2>/dev/null || true

distclean: clean
	rm -rf $(NATUME_DIR)
