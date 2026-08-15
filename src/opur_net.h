// opur_net.h — 書いたものを EQIDEN へ送る
//
// EQIDEN は「セマフォ方式」の受け口。1 つの URL に対して
//   GET  … 受け口の中身を読む。空なら「置いてよい」、非空なら「まだ塞がっている」
//   PUT  … 中身を置く
// という約束になっている。相手が引き取るまで上書きしないので、
// 送る側は「空いていたら 1 件だけ置く」を繰り返せばよい。
//
// キューは SD の /opur/ そのもの。送信済みは /opur/sent/ へ移す。
// 別途キューファイルを持たないので、電源が落ちても状態が食い違わない。
//
// **実機専用**。PC ビルドでは何もしない空実装になる（.cpp 側の #else）。
// 設定は持たない。ENDPOINT_URL は opur_config.h から呼び出し側が渡す。

#ifndef OPUR_NET_H_INCLUDED
#define OPUR_NET_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

// 送信先 URL の最大長。opur_config.h の OPUR_CFG_URL_MAX と同じ長さ。
// 同じ値を使うためだけに opur_config.h を include すると、送信が設定の
// 都合に縛られる（opur_wifi.h の注記と同じ理由）ので、こちらで持つ。
#define OPUR_NET_URL_MAX 160

// 送信先 URL を設定する。空文字列 or NULL で送信無効。
// 起動時に 1 度だけ呼ぶ。長すぎる URL は切り詰める。
void opur_net_init(const char *endpoint_url);

// /opur/ の未送信ファイル先頭 1 件を EQIDEN セマフォ方式で送信試行する。
// ブロックする（TLS ハンドシェイク + 往復 2 回で数秒かかりうる）。
//
// 戻り値:
//    1  送信成功（sent/ へ移動済み）
//    0  何もしなかった（URL 未設定 / WiFi 未接続 / セマフォ塞がり / キュー空）
//   -1  エラー（HTTP エラー・ファイルが読めない等）
int opur_net_try_send(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPUR_NET_H_INCLUDED
