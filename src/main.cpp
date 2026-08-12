#include <M5Cardputer.h>

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg);

    auto &d = M5Cardputer.Display;
    d.setFont(&fonts::efontJA_16);  // 東雲16x16 (efont JA / JIS第二水準まで)
    d.setTextSize(1);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.fillScreen(TFT_BLACK);
    d.setCursor(0, 0);

    d.println("令和のRupo");
    d.println("M5OpurSan");
    d.println("東雲フォント表示テスト");
    d.println("漢字ひらがなカタカナ");
}

void loop() {
}
