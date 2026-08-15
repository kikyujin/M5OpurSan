// opur_net.h — 書いたものを EQIDEN へ送る
//
// EQIDEN は「セマフォ方式」の受け口。1 つの URL に対して
//   GET  … 受け口の中身を読む。空なら「置いてよい」、非空なら「まだ塞がっている」
//   PUT  … 中身を置く
// という約束になっている。GET では消費されない（受け手が空 PUT するまで
// 塞がったまま）ので、送る側は「空いていたら置く」を守ればよい。
//
// **送信済みの管理はしない。** 何を送ったか・送っていないかは持たず、
// ユーザーがメニューから明示的に送ったものだけが飛ぶ。
// キューも sent/ も無い（019 では持っていたが 020 で撤去した）。
//
// 送るのは「いま編集中のバッファ」であってファイルではない。
// ファイルを読むのは呼び出し側の仕事で、ここは渡された文字列を置くだけ。
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

// 受け口が空いているか見る（GET）。ブロックする（TLS 込みで数秒）。
//
// 戻り値:
//    1  空いている（送ってよい）
//    2  塞がっている（相手がまだ引き取っていない）
//    0  送る条件が揃っていない（URL 未設定 / WiFi 未接続）
//   -1  通信エラー。詳しい理由はログに残る
int opur_net_check(void);

// テキストを置く（PUT）。ブロックする。
// text は UTF-8。len はバイト数（NUL 終端は見ない）。
//
// **塞がっているかどうかは見ない。** 上書きしてよいかの判断は呼び出し側。
// opur_net_check() で 2 が返ったあとに、それでも送ると決めたなら
// そのまま呼んでよい。
//
// 戻り値:
//    1  成功
//    0  送る条件が揃っていない（URL 未設定 / WiFi 未接続）
//   -1  通信エラー。詳しい理由はログに残る
int opur_net_put(const char *text, int len);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPUR_NET_H_INCLUDED
