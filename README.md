# M5OpurSan

> 令和のワープロ — M5Stack Cardputer で蘇るワープロ専用機

## これは何？

M5Stack Cardputer ADV の上で動く、スタンドアロン日本語入力環境。

WiFi 不要。サーバー不要。辞書もかな漢字変換エンジンも、全部この手のひらサイズのデバイスの中に入っている。電源を入れたら日本語が打てる。ワープロ専用機の体験を、ESP32-S3 で再現する。

書いたメモは microSD に保存される。WiFi があれば EQIDEN（https://eqiden.com）経由で外に送り出すこともできる（なくても困らない）。

## 特徴

- **完全オフライン日本語入力** — 44万語の辞書をmicroSDに搭載。ローマ字→かな→漢字変換がデバイス単体で完結
- **curses 互換アーキテクチャ** — エディタのコードは標準 curses API で書かれている。Mac/Linux のターミナルでそのまま動く。M5 では薄い互換レイヤー（m5curses）が LCD に描画する。リンク先を変えるだけ
- **保存と送信は別の操作** — 保存は SD に書く。送信はメニューから明示的に。何を送るか、いつ送るかはあなたが決める
- **郵便受けモデルの送信** — EQIDEN にテキストを PUT するだけ。受け側が AI でもサーバーでも何でもいい。M5OpurSan は送信先を関知しない

## 必要なもの

| | |
|--|--|
| M5Stack Cardputer | ESP32-S3, 1.14" LCD, 56キーキーボード。ADV で開発・検証。無印 / v1.1 でも動く可能性あり |
| microSD カード | 32GB 以下（FAT32）。辞書に約 14MB |
| USB-C ケーブル | ファームウェア書き込み用 |

## ビルド

### PC ビルド（macOS / Linux）

Mac や Linux のターミナル上で curses アプリとして動く。実機なしで開発・テストできる。

```bash
cd opur_editor
make
./opur_editor
```

### M5 ビルド（PlatformIO）

```bash
pio run -t upload
```

PlatformIO が ESP32-S3 向けにビルドし、USB 経由で書き込む。

## SD カードの準備

```
/
├── config.txt          ← WiFi 設定（任意）
├── opur/               ← メモの保存先（自動作成）
└── dict/
    └── system.dic      ← システム辞書（同梱）
```

`system.dic` はリポジトリの `dict/` に同梱されている。SD カードの `/dict/` にコピーする。

## 使い方

電源を入れるとエディタが起動する。ローマ字を打つとかなに変換され、スペースキーで漢字変換候補が出る。

| キー | 動作 |
|--|--|
| 文字キー | ローマ字入力 → かな変換 |
| Space | 漢字変換（候補表示） |
| ← → | 候補選択 |
| Enter | 確定 |
| ESC | メニュー / 変換キャンセル |
| BS | 削除 |
| Fn+S | 保存（ESC→1.保存 のショートカット） |

### ESC メニュー

```
[1保存 2新規 3読込 4送信]
ESC:戻る
```

- **1.保存** — 現在のメモを SD に保存。読み込んだファイルは上書き、新規なら新番号
- **2.新規** — バッファをクリアして新しいメモを開始
- **3.読込** — 保存済みメモを ← → で選んでプレビューしながら読み込む
- **4.送信** — 今のバッファの内容を ENDPOINT_URL に送信

## 送信（EQIDEN 連携）

M5OpurSan は config.txt に書かれた URL にテキストを PUT する。推奨は EQIDEN（https://eqiden.com）。

保存と送信は別の操作。保存は SD に書くだけ。送信するかどうかはあなたが決める。

### セットアップ

1. https://eqiden.com でトークンを発行する
2. microSD の `config.txt` にこう書く:

```
WIFI_SSID=あなたのSSID
WIFI_PASS=あなたのパスワード
ENDPOINT_URL=https://eqiden.com/あなたのトークン
```

### 仕組み（郵便受けモデル）

```
M5OpurSan:   ESC→4.送信 → GET → 箱が空？ → PUT テキスト
受け取り側:   GET → テキストある → 処理 → PUT ""（箱を空にする）
```

箱が空なら入れる。空じゃなければ確認が出る（強制送信もできる）。PUT/GET と空文字だけで成立する。

送信先は EQIDEN に限らない。`ENDPOINT_URL` を自前の API に向ければ何にでも繋がる。

## WiFi について

WiFi はオプション。オフラインでも日本語入力・編集・保存の全機能が動く。

**繋がらない場合はルーターの暗号化設定を確認してください。** ESP32-S3 は WPA2/WPA3 混在モード（transition mode）で認証ハングする既知問題があります。ルーターを WPA2-Personal AES 専用に設定することで解決します。

その他の注意:
- 2.4GHz のみ（5GHz 非対応、ESP32-S3 の制約）
- 繋がらないときは ESC → ログ で起動ログを確認できる
- タイムゾーンは JST 固定（ソース修正で変更可）

## 辞書

システム辞書は alt-cannadic + ipadic から生成した 44 万語・14.2MB のバイナリ。リポジトリに同梱。

自分でビルドする場合:

```bash
cd dict
make build-dict
```

ユーザー辞書（`user.dic`）は将来実装予定。現時点ではシステム辞書のみ対応。

## 日本語変換

- **文節変換ではありません**（熟語変換）。
  ❌️`私は` `日本語が` `得意です。`

  ⭕️`私` `は` `日本語` `が` ` 得意` `です` `。`

- ローマ字入力して、スペースで変換。

- 変換候補はスペース or 左右で送り、Enter で決定。

- 半角カナには対応してません（仕様）。

- 全角スペースの入力はできません（困ったら対応するかも）。

## ライセンス

本リポジトリは2つのライセンスで構成されています。

| 対象 | ライセンス |
|--|--|
| `dict/` 以外のすべて（エディタ・FEP・m5curses 等） | MIT |
| `dict/` 以下（辞書ビルドツール・辞書バイナリ） | GPL-2.0 |

辞書バイナリは [alt-cannadic](https://github.com/takayuki/natume)（GPL-2.0）および ipadic（NAIST License）から生成した派生物です。

## 謝辞

cannadic の開発と普及に携わられた全ての方に。

M5Stack Cardputer Adv の開発に携わられた全ての方に。

Rupo を生み出した東芝の技術者の皆さんに。

## リンク

- [EQIDEN — AIの襷を繋ぐ](https://eqiden.com)
- [M5Stack Cardputer ADV](https://shop.m5stack.com/products/m5stack-cardputer-adv)
- https://x.com/777kdm
- [M5Stackワープロ M5OpurSan 爆誕💥](https://note.com/kikyujin/n/ndca2a3bfbf7a)

