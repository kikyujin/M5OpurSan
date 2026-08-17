// opur_sleep.h — ライトスリープ / ディープスリープ
//
// **実機専用。** ESP-IDF のスリープ API とボード固有のキーウェイク配線を
// ここに閉じ込める。呼び出し側（src/main.cpp）は「いつ寝るか」だけを決め、
// M5Unified や esp_sleep.h を持ち込まずに済む。
//
// 実装は C++（M5.getBoard() を見るため）。他と揃えて extern "C" で囲ってある。

#ifndef OPUR_SLEEP_H_INCLUDED
#define OPUR_SLEEP_H_INCLUDED

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 起きた理由。
#define OPUR_WAKE_OTHER 0   // 上記以外（寝られなかった場合も含む）
#define OPUR_WAKE_KEY   1   // キーが押された
#define OPUR_WAKE_TIMER 2   // タイマーが切れた

// この機体でキーウェイクが使えるか（1/0）。
//
// **使えるのは Cardputer ADV だけ。** ADV のキーボードは TCA8418 という
// I2C のキースキャナで、キーを押すと INT（GPIO11）が LOW になる。
// TCA8418 は自分で走査するので、ESP32 が寝ていても取りこぼさない。
//
// 無印 / v1.1 は GPIO マトリクスを CPU 自身が走査する方式で、
// 出力 3 本（GPIO 8/9/11）で 8 行のうち **1 行だけ**を選ぶ形になっている。
// 寝ている間は 1 行しか有効にできないため、その行のキーでしか起きられない。
// 「押しても起きないキーがある」のは壊れて見えるので、
// **この関数は無印で 0 を返し、呼び出し側にスリープごと諦めさせる。**
// 実機を持っていないので検証もできていない（README に注記あり）。
int opur_sleep_key_wake_supported(void);

// 今回の起動が何で起こされたか。OPUR_WAKE_*。
//
// ディープスリープから戻ると setup() からやり直しになるので、
// 「タイマーで起きただけ（誰も待っていない）」のか
// 「キーで起こされた（人が使いにきた）」のかをここで見分ける。
// 電源投入や通常のリセットでは OPUR_WAKE_OTHER。
int opur_sleep_wake_cause(void);

// ライトスリープが有効か（1/0）。
//
// **いまは 1（有効）。** 実体は opur_sleep.cpp の LIGHT_SLEEP_ENABLED。
// 「充電されない」疑いで一度 0 にしたが再現せず戻した経緯も、そちらに書いてある。
//
// **呼び出し側（src/main.cpp の idle_check）はこれを見て、寝る手前で引き返す。**
// opur_sleep_light() 自身も 0 のときは何もせず返すが、それだけだと
// 画面 OFF / WiFi 切断（どちらも呼び出し側の担当）を毎回やって即座に戻す
// ことになり、無操作中ずっと画面のチラつきと再接続を繰り返してしまう。
int opur_sleep_light_enabled(void);

// ライトスリープに入る。RAM は保持されるので、戻ってきたら続きから動く。
//
//   timer_ms  0 より大きければタイマーでも起きる。0 ならキーだけ
//
// 戻り値は OPUR_WAKE_*。キーウェイクが使えない機体、または寝る時点で
// 既にキーが押されている場合は、寝ずに OPUR_WAKE_KEY / OPUR_WAKE_OTHER を返す。
// opur_sleep_light_enabled() が 0 のときも寝ずに OPUR_WAKE_OTHER を返す。
//
// **画面と WiFi はここでは触らない。**呼び出し側の責任
// （どちらもこの層より上の関心事で、復帰後の描き直しと対で扱いたいため）。
int opur_sleep_light(uint32_t timer_ms);

// ディープスリープに入る。**戻らない。** 復帰は setup() から。
//
//   timer_ms  0 より大きければタイマーで起きる
//
// キーウェイクが使える機体なら、タイマーとは別にキーでも起きる。
void opur_sleep_deep(uint32_t timer_ms);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPUR_SLEEP_H_INCLUDED
