# dict_tools — M5OpurSan 辞書ビルドパイプライン
#
#   make build-dict   natume を clone（未取得なら）→ system.dic + stats.txt 生成
#   make test-dict    辞書引きテスト 9 項目
#   make interactive  対話モードで辞書を引く
#   make clean        生成物削除（natume の clone は消さない）
#   make distclean    natume の clone も消す

CC     ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -O2

PYTHON ?= python3

NATUME_URL := https://github.com/takayuki/natume.git
NATUME_DIR := natume
OUT_DIR    := output
DIC        := $(OUT_DIR)/system.dic
STATS      := $(OUT_DIR)/stats.txt
TESTER     := test_dict

.PHONY: all build-dict test-dict interactive clean distclean

all: build-dict

# --- 素材取得 ---
$(NATUME_DIR)/alt-cannadic-110208:
	@echo "natume を clone 中… ($(NATUME_URL))"
	git clone --depth 1 $(NATUME_URL) $(NATUME_DIR)

# --- 辞書ビルド ---
build-dict: $(DIC)

$(DIC) $(STATS): build_dict.py | $(NATUME_DIR)/alt-cannadic-110208
	$(PYTHON) build_dict.py

# --- テスト ---
# 辞書アクセス層は opur_dict.c。opur_editor/ からも -I. で参照される。
$(TESTER): test_dict.o opur_dict.o
	$(CC) $(CFLAGS) -o $@ test_dict.o opur_dict.o

%.o: %.c opur_dict.h
	$(CC) $(CFLAGS) -c -o $@ $<

test-dict: $(TESTER) $(DIC)
	./$(TESTER)

interactive: $(TESTER) $(DIC)
	./$(TESTER) -i

clean:
	rm -f $(TESTER) test_dict.o opur_dict.o $(DIC) $(STATS)
	-rmdir $(OUT_DIR) 2>/dev/null || true

distclean: clean
	rm -rf $(NATUME_DIR)
