// hello_m5.cpp — Phase E-0: m5curses 経由で東雲フォントの日本語表示を確認する
//
// 実機（M5Cardputer / ADV）に焼いて、豆腐（□）が出ないことを目視で確かめるための
// 最小プログラム。入力は扱わない。
//
// ここで確認したいこと:
//   1. initscr() で Canvas と efontJA_16 が正しく用意できるか
//   2. mvaddstr() の座標（半角セル単位）が意図どおりか
//   3. 全角・半角の混在行が崩れないか
//   4. attron(A_REVERSE) で前景・背景が入れ替わるか
//   5. attron(A_UNDERLINE) で下線が文字幅ぶん引かれるか

#include "m5curses.h"

void setup() {
    initscr();
    clear();

    mvaddstr(0, 0, "M5OpurSan Phase E");
    mvaddstr(1, 0, "令和のRupo");
    mvaddstr(2, 0, "東雲フォント表示テスト");
    mvaddstr(3, 0, "OPUR_0001.txt");

    // 以下は属性の目視確認用。実装したのに実機で確かめられないものを残したくない。
    attron(A_REVERSE);
    mvaddstr(5, 0, " 反転表示 A_REVERSE ");
    attroff(A_REVERSE);

    // 全角・半角が混ざっても下線が文字幅ぶん引かれることを見る。
    attron(A_UNDERLINE);
    mvaddstr(6, 0, "みかくてい henkan");
    attroff(A_UNDERLINE);

    refresh();
}

void loop() {
    // 表示を保つだけ。Phase E-1 でここが getch() ループになる。
    getch();
}
