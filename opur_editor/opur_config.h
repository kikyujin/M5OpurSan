// opur_config.h — microSD の /config.txt から読む設定
//
// **実機専用**。SD の存在が前提なので、環境非依存層（editor.c / candidate_bar.c /
// conv_utf8.c / utf8_utf16.c）には入れない。PC ビルドはこのファイルを含まない。
//
// WiFi だけでなく送信先ホストも持つ。「設定」という 1 つの責務にまとめてあるので、
// opur_wifi も将来の送信処理も、どちらもここを参照する。
// 逆向き（送信処理が opur_wifi.h 経由で設定を引く）にはしない。
//
// フォーマット（実機の SD にあるものと同じ）:
//   WIFI_SSID=MyWiFi
//   WIFI_PASS=xxxxx
//   ENDPOINT_URL=https://eqiden.com/<64 桁の16進トークン>
//   OPUR_HOST=192.168.1.100
//   OPUR_PORT=9500
//   DANTONG_HOST=192.168.1.101
//   DANTONG_PORT=9201

#ifndef OPUR_CONFIG_H_INCLUDED
#define OPUR_CONFIG_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

// 1 つの値の最大長。SSID・パスフレーズとも WPA2 の上限が 63 文字なので +1。
#define OPUR_CFG_VAL_MAX 64

// URL 1 本の最大長。ENDPOINT_URL は "https://eqiden.com/" + 64 桁トークンで
// 83 文字あり、OPUR_CFG_VAL_MAX（64）には**入らない**。
// set_str() は黙って切り詰めるだけなので、URL は別枠にしてある。
#define OPUR_CFG_URL_MAX 160

// 1 行の最大長。key= と値、改行、終端で足りる長さ。
// これを超える行は「超えたぶんを次の行として読まない」で捨てる。
#define OPUR_CFG_LINE_MAX 224

typedef struct {
    char wifi_ssid[OPUR_CFG_VAL_MAX];
    char wifi_pass[OPUR_CFG_VAL_MAX];

    // 書いたものの送信先（EQIDEN のトークン URL）。空なら送信しない。
    char endpoint_url[OPUR_CFG_URL_MAX];

    char opur_host[OPUR_CFG_VAL_MAX];
    int  opur_port;

    char dantong_host[OPUR_CFG_VAL_MAX];
    int  dantong_port;
} OpurConfig;

// 全フィールドを「未設定」にする。文字列は "" 、ポートは 0。
void opur_config_clear(OpurConfig *cfg);

// path から読む。読めたキーだけ上書きし、無いキーは触らない。
// 呼ぶ前に opur_config_clear() 済みであることを前提にする。
//
// 戻り値: 取り込んだキーの数。ファイルが開けなければ -1。
//         SD が無い・config.txt が無いのは異常ではないので、
//         -1 でも呼び出し側は普通に起動を続けてよい。
int opur_config_load(OpurConfig *cfg, const char *path);

// WiFi に繋ぎにいけるだけの設定があるか（SSID が空でない）。
// パスフレーズは空でもよい（オープンな AP）。
int opur_config_has_wifi(const OpurConfig *cfg);

// 送信先 URL。未設定なら ""（NULL は返さない）。
// cfg が NULL のときも "" を返すので、呼び出し側は長さだけ見ればよい。
const char *opur_config_endpoint(const OpurConfig *cfg);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPUR_CONFIG_H_INCLUDED
