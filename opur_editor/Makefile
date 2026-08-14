# opur_editor — エディタ curses プロトタイプ (m4max / macOS)
#
#   make          curses 版ビルド
#   make run      実行
#   make test     自動テスト（curses 不要）— エディタ＋変換候補バー
#   make cand     変換候補バーの対話モード
#   make clean    生成物削除
#
# editor.c / candidate_bar.c / conv_utf8.c / utf8_utf16.c は C11 の環境非依存
# ロジック。curses に触るのは view_curses.c / main.c。
#
# 辞書アクセス層は dict_tools/opur_dict.{h,c} を -I../dict_tools で参照する。
# 依存は opur_editor → dict_tools の片方向。
# ncurses の wide-char API を有効にする _XOPEN_SOURCE_EXTENDED は view_curses.c
# 側で定義しているので、ここでは足さない。
#
# Homebrew の ncurses 6.x を使いたい場合:
#   make NCURSES_PREFIX=/opt/homebrew/opt/ncurses LDLIBS=-lncursesw

CC             ?= clang
NCURSES_PREFIX ?=

DICT_DIR := ../dict_tools

CFLAGS ?= -std=c11 -Wall -Wextra -O2
CFLAGS += -I$(DICT_DIR)
LDLIBS ?= -lncurses

ifneq ($(NCURSES_PREFIX),)
CFLAGS  += -I$(NCURSES_PREFIX)/include
LDFLAGS += -L$(NCURSES_PREFIX)/lib
endif

TARGET     := opur_editor
TESTER     := test_editor
CAND_TESTER := test_candidate_bar
DEPS       := editor.h candidate_bar.h conv_utf8.h utf8_utf16.h $(DICT_DIR)/opur_dict.h

APP_OBJS  := main.o view_curses.o editor.o
TEST_OBJS := test_editor.o editor.o
CAND_OBJS := test_candidate_bar.o candidate_bar.o conv_utf8.o utf8_utf16.o \
             $(DICT_DIR)/opur_dict.o

.PHONY: all run test cand clean

all: $(TARGET)

$(TARGET): $(APP_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(APP_OBJS) $(LDLIBS)

# テストは editor.o しかリンクしないので ncurses 不要。
$(TESTER): $(TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(TEST_OBJS)

# 候補バーのテストも ncurses 非リンク（curses 非依存の保証を兼ねる）。
$(CAND_TESTER): $(CAND_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(CAND_OBJS)

$(DICT_DIR)/opur_dict.o: $(DICT_DIR)/opur_dict.c $(DICT_DIR)/opur_dict.h
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

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
