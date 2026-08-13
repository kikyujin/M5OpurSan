# opur_editor — エディタ curses プロトタイプ (m4max / macOS)
#
#   make          curses 版ビルド
#   make run      実行
#   make test     自動テスト（curses 不要）
#   make clean    生成物削除
#
# editor.c は C11 の環境非依存ロジック。curses に触るのは view_curses.c / main.c。
# ncurses の wide-char API を有効にする _XOPEN_SOURCE_EXTENDED は view_curses.c
# 側で定義しているので、ここでは足さない。
#
# Homebrew の ncurses 6.x を使いたい場合:
#   make NCURSES_PREFIX=/opt/homebrew/opt/ncurses LDLIBS=-lncursesw

CC             ?= clang
NCURSES_PREFIX ?=

CFLAGS ?= -std=c11 -Wall -Wextra -O2
LDLIBS ?= -lncurses

ifneq ($(NCURSES_PREFIX),)
CFLAGS  += -I$(NCURSES_PREFIX)/include
LDFLAGS += -L$(NCURSES_PREFIX)/lib
endif

TARGET := opur_editor
TESTER := test_editor
DEPS   := editor.h

APP_OBJS  := main.o view_curses.o editor.o
TEST_OBJS := test_editor.o editor.o

.PHONY: all run test clean

all: $(TARGET)

$(TARGET): $(APP_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(APP_OBJS) $(LDLIBS)

# テストは editor.o しかリンクしないので ncurses 不要。
$(TESTER): $(TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(TEST_OBJS)

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

test: $(TESTER)
	./$(TESTER)

clean:
	rm -f $(APP_OBJS) $(TEST_OBJS) $(TARGET) $(TESTER)
