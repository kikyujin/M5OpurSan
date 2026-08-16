// opur_wifi.h — WiFi 接続と NTP 同期
//
// **実機専用**。責務はこの 2 つだけで、設定は持たない（opur_config.h が持つ）。
// 送信処理を書くときは opur_config.h を直接参照すること。
// このヘッダ経由でホスト名を引く形にすると、送信が WiFi に依存してしまう。
//
// 実装は C++（WiFi.h / esp32-hal-time.h が C++ のため）。
// 呼び出し側の main.cpp も C++ だが、他と揃えて extern "C" で囲ってある。

#ifndef OPUR_WIFI_H_INCLUDED
#define OPUR_WIFI_H_INCLUDED

#include "opur_config.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 接続を待つ上限（ms）。指示 2-2 の「最大3秒」。
// 実機の接続所要は 650ms（WPA2-PSK / RSSI 良好）。4 倍以上の余裕がある。
#define OPUR_WIFI_TIMEOUT_MS 3000

// NTP の応答を待つ上限（ms）。
// 3 秒では足りなかった（実機で NTP NG）。接続直後は DHCP が終わったばかりで
// DNS がまだ引けないことがあり、名前解決 + SNTP 初回応答で数秒かかる。
//
// ここで待つのは「時刻がまだ入っていないとき」だけ。ESP32 の時刻は
// リセットをまたいで保持されるので、2 回目以降の起動は 0ms で返る
// （実機で確認済み）。つまりこの 8 秒が効くのは電源を完全に切ったあとの
// 初回だけで、常用時の起動を遅くはしない。
#define OPUR_NTP_TIMEOUT_MS 8000

// WiFi に繋ぐ。ブロックする（最大 OPUR_WIFI_TIMEOUT_MS）。
//   - 既に繋がっていれば即 1
//   - cfg に SSID が無ければ何もせず 0（設定なしは異常ではない）
//   - タイムアウトしたら 0。WiFi は落とさず、自動再接続に任せる
// 戻り値: 1=接続済み, 0=未接続
int opur_wifi_connect(const OpurConfig *cfg);

// 今つながっているか。ブロックしない。
int opur_wifi_is_connected(void);

// WiFi を落とす。スリープの前に呼ぶ。
//
// **自動再接続も切る。**切らないと、こちらが落としたそばから
// 裏で繋ぎ直しにいってしまう。opur_wifi_connect() が呼ばれたときに
// また立てるので、対で使うぶんには意識しなくていい。
void opur_wifi_disconnect(void);

// NTP 同期。未接続なら何もせず 0、同期済みなら即 1。
// 二重同期しないようフラグを内部に持つ。
//
// timeout_ms は応答を待つ上限。起動時は何も届いていない前提で長めに取るが、
// あとから拾い直すときは画面が止まるので短くする。
int opur_wifi_ntp_sync(uint32_t timeout_ms);

// 一度でも同期できていれば 1。
int opur_wifi_ntp_synced(void);

// 自分の IP を "192.168.1.23" の形で返す。未接続なら "0.0.0.0"。
// 戻り値は内部の静的バッファ。次の呼び出しで上書きされる。
const char *opur_wifi_ip(void);

// 切り分け用。直前の opur_wifi_connect() の結果を細かく見る。
//
//   status  wl_status_t の生値。1=SSID 見つからず / 4=接続失敗（鍵違い等）
//           / 6=切断状態のまま / 3=接続済み
//   elapsed 接続待ちに実際かかった ms。タイムアウト値の見直しに使う
int      opur_wifi_last_status(void);
uint32_t opur_wifi_elapsed_ms(void);

// 直前の接続が「失敗」ではなく「待ち時間内に間に合わなかっただけ」なら 1。
//
// **WiFi.begin() は非同期。** こちらが待つのをやめても裏では試行が続くので、
// 時間切れで抜けた直後に繋がることがある。SSID が無い・鍵が違うといった
// はっきりした失敗と、これを混ぜて「NG」と言わないための判定。
int opur_wifi_last_pending(void);

// 直近の切断理由（wifi_err_reason_t の生値）。status() は WL_DISCONNECTED に
// 丸めてしまい「AP が無い」と「鍵が違う」を区別できないので、こちらを見る。
//   201 AP見つからず / 202 認証失敗 / 15 鍵違い / 205 接続失敗
int opur_wifi_last_reason(void);

// 理由コードの短い日本語。知らないコードなら ""。
const char *opur_wifi_reason_text(int reason);

// 診断用。opur_wifi_connect() の前後で測った空きヒープ（バイト）。
// 呼ぶ前は両方 0。WiFi スタックが実際に何 KB 食うかを実機で見るためのもの。
uint32_t opur_wifi_heap_before(void);
uint32_t opur_wifi_heap_after(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPUR_WIFI_H_INCLUDED
