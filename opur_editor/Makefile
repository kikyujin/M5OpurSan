# opur_editor — エディタ + FEP + 変換候補バー 統合デモ (m4max / macOS)
#
#   make          curses 統合デモをビルド
#   make run      実行
#   make test     自動テスト（curses 不要）— エディタ＋変換候補バー
#   make cand     変換候補バーの対話モード
#   make clean    生成物削除
#
# editor.c / candidate_bar.c / conv_utf8.c / utf8_utf16.c は C11 の環境非依存
# ロジック。curses に触るのは view_curses.c、C++ なのは main.cpp と fep/ だけ。
#
# 参照する隣のディレクトリ:
#   dict_tools/  辞書アクセス層（opur_dict.{h,c}）
#   fep/         ローマ字→かな変換（C++17）
# 依存はどちらも片方向（opur_editor → 隣）。
#
# リンクは C++ で行う（fep/ が C++17 のため）。C 側のヘッダはすべて
# extern "C" で囲ってあるので、混在は Makefile のリンカ指定だけで済む。
#
# Homebrew の ncurses 6.x を使いたい場合:
#   make NCURSES_PREFIX=/opt/homebrew/opt/ncurses LDLIBS=-lncursesw

CC             ?= clang
CXX            ?= clang++
NCURSES_PREFIX ?=

DICT_DIR := ../dict_tools
FEP_DIR  := ../fep

INCLUDES := -I$(DICT_DIR) -I$(FEP_DIR)

CFLAGS   ?= -std=c11 -Wall -Wextra -O2
CFLAGS   += $(INCLUDES)

CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
CXXFLAGS += $(INCLUDES) -D_XOPEN_SOURCE_EXTENDED

LDLIBS ?= -lncurses

ifneq ($(NCURSES_PREFIX),)
CFLAGS   += -I$(NCURSES_PREFIX)/include
CXXFLAGS += -I$(NCURSES_PREFIX)/include
LDFLAGS  += -L$(NCURSES_PREFIX)/lib
endif

TARGET      := opur_editor
TESTER      := test_editor
CAND_TESTER := test_candidate_bar

DEPS := editor.h candidate_bar.h conv_utf8.h utf8_utf16.h view.h \
        $(DICT_DIR)/opur_dict.h

DICT_OBJS := $(DICT_DIR)/opur_dict.o
FEP_OBJS  := $(FEP_DIR)/fep.o $(FEP_DIR)/fep_util.o \
             $(FEP_DIR)/roma_table.o $(FEP_DIR)/convert.o

APP_OBJS  := main.o view_curses.o editor.o candidate_bar.o conv_utf8.o \
             utf8_utf16.o $(DICT_OBJS) $(FEP_OBJS)
TEST_OBJS := test_editor.o editor.o
CAND_OBJS := test_candidate_bar.o candidate_bar.o conv_utf8.o utf8_utf16.o \
             $(DICT_OBJS)

.PHONY: all run test cand clean

all: $(TARGET)

# 統合デモだけ C++ でリンクする
$(TARGET): $(APP_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(APP_OBJS) $(LDLIBS)

# テストは C だけ。ncurses もリンクしないので、通ること自体が
# editor.c / candidate_bar.c の環境非依存の保証になる。
$(TESTER): $(TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(TEST_OBJS)

$(CAND_TESTER): $(CAND_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(CAND_OBJS)

$(DICT_DIR)/opur_dict.o: $(DICT_DIR)/opur_dict.c $(DICT_DIR)/opur_dict.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(FEP_DIR)/%.o: $(FEP_DIR)/%.cpp $(FEP_DIR)/fep.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.cpp $(DEPS) $(FEP_DIR)/fep.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

test: $(TESTER) $(CAND_TESTER)
	./$(TESTER)
	./$(CAND_TESTER)

cand: $(CAND_TESTER)
	./$(CAND_TESTER) -i

clean:
	rm -f $(APP_OBJS) $(TEST_OBJS) $(CAND_OBJS) \
	      $(TARGET) $(TESTER) $(CAND_TESTER)
