# fep テストハーネス (m4max / macOS)
#
#   make            ビルド
#   make run        対話モードで起動
#   make selftest   curses を使わないキー列テスト
#   make clean      生成物削除
#
# macOS 標準の libncurses は -D_XOPEN_SOURCE_EXTENDED を付けると
# wide-char API (cchar_t / addwstr) が使える。
# Homebrew の ncurses 6.x を使いたい場合は:
#   make NCURSES_PREFIX=/opt/homebrew/opt/ncurses LDLIBS=-lncursesw

CXX             ?= clang++
NCURSES_PREFIX  ?=

CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
CXXFLAGS += -D_XOPEN_SOURCE_EXTENDED
LDLIBS   ?= -lncurses

ifneq ($(NCURSES_PREFIX),)
CXXFLAGS += -I$(NCURSES_PREFIX)/include
LDFLAGS  += -L$(NCURSES_PREFIX)/lib
endif

TARGET := test_fep
SRCS   := fep.cpp fep_util.cpp roma_table.cpp convert.cpp test_fep.cpp
OBJS   := $(SRCS:.cpp=.o)
DEPS   := fep.h

.PHONY: all run selftest clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

selftest: $(TARGET)
	./$(TARGET) --selftest

clean:
	rm -f $(OBJS) $(TARGET)
