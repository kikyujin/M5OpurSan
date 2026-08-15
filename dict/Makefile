# dict — M5OpurSan 辞書ビルドパイプライン
#
#   make build-dict   natume を clone（未取得なら）→ system.dic + stats.txt 生成
#   make clean        生成物削除（natume の clone は消さない）
#   make distclean    natume の clone も消す
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

.PHONY: all build-dict clean distclean

all: build-dict

# --- 素材取得 ---
$(NATUME_DIR)/alt-cannadic-110208:
	@echo "natume を clone 中… ($(NATUME_URL))"
	git clone --depth 1 $(NATUME_URL) $(NATUME_DIR)

# --- 辞書ビルド ---
build-dict: $(DIC)

$(DIC) $(STATS): build_dict.py | $(NATUME_DIR)/alt-cannadic-110208
	$(PYTHON) build_dict.py

clean:
	rm -f $(DIC) $(STATS)
	-rmdir $(OUT_DIR) 2>/dev/null || true

distclean: clean
	rm -rf $(NATUME_DIR)
