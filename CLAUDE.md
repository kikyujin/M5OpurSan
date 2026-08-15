# M5OpurSan

M5Cardputer で動く日本語入力・編集ツール。オフラインで完結し、WiFi があれば
書いたものを EQIDEN に送る。

コードの構造・分担は `platformio.ini` と `opur_editor/Makefile` の冒頭コメントに
書いてある。ここには**実機を触らないと分からないこと**だけを置く。

---

## 実機のハード構成（実測）

**思い込みで型番を決めないこと。** 必ず実機から読む。

```
$ pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 \
    --port /dev/cu.usbmodem* flash_id

Chip is ESP32-S3 (revision v0.2)
Features: WiFi, BLE          ← PSRAM の記載が無い
Detected flash size: 8MB
```

つまり **ESP32-S3FN8（Flash 8MB 内蔵・PSRAM なし）**。

- **PSRAM は積んでいない。** 8MB PSRAM を持つのは **Cardputer ADV** のほう
- `board_build.arduino.memory_type = qio_opi` + `-DBOARD_HAS_PSRAM` を入れても
  実行時の `MALLOC_CAP_SPIRAM` は **0K** のまま。試して確認済みなので繰り返さない
- 内部 DRAM 約 320KB がすべて。静的に約 151KB 使っている

2026-08-15 に、型番を N16R8（＝ADV 相当）と思い込んで PSRAM 有効化を試し、
1 往復ぶん無駄にした。**ハード前提は最初に esptool で確認する。**

## メモリの余裕が無い（HTTPS を触るとき必読）

WiFi スタックが 50KB 前後を持っていく（起動ログの `heap 129>78K`）。
**mbedTLS のハンドシェイクには 45〜50KB** 要るので余裕はほとんど無い。
Canvas が 8bpp だった頃は接続後 44KB しか残らず、**HTTPS が張れなかった**
（実機で `GET -1`）。1bpp にして 28KB 空けた今が **78KB**。

いま枠を作っているのは 1 箇所だけ:

- **描画 Canvas は 1bpp**（`m5curses.cpp` の `canvas_alloc`）。
  240x135 で 4KB。8bpp なら 32KB なので **28KB を常時空けている**。
  画面は前景・背景の 2 色しか使わないので階調は要らない。
  `kFg`/`kBg` は**色ではなくパレット番号**（1/0）。

**Canvas の色深度を上げると HTTPS が壊れる。** 変えるときは送信も実機で試すこと。

過去に「送信の数秒だけ Canvas 32KB を解放する」方式を試して**捨てた**。
解放はできても、TLS 解放後に**連続した** 32KB を取り戻せず失敗することがある
（実機でリセットに至った）。解放して作る枠は当てにしない。

これ以上メモリが要るときの候補: `CandBar`（静的 84KB）の縮小。

## ビルド・書き込み・実機テスト

```bash
# PC 側（実機とは完全に独立）
#
# **`make && ./test_editor` としないこと。** 既定ターゲットは本体だけを作るので、
# テストは前回の古いバイナリが走る（気づかず 1 往復無駄にした）。
# `make test` ならビルドしてから実行してくれる。
cd opur_editor && make test && make test-dict
make -C fep selftest          # 'test' ターゲットは無い

# 実機
pio run
pio run -t upload
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 \
  --port /dev/cu.usbmodem* verify_flash 0x10000 \
  .pio/build/m5stack-cardputer/firmware.bin   # 載っているものの照合
```

`pio` は PATH に無い。`~/.platformio/penv/bin/pio` を使う。
**`pio run` はプロジェクトルートで実行する**（`opur_editor/` にいると失敗する）。

## ログの読み方

ESC メニューの「4 ログ」。32 行のリングバッファ。

- **開いた直後は最新 7 行**しか見えない。起動時の行（`PSRAM` など）は
  ↑ で戻らないと出てこない
- `esp_restart()` はリセット理由が「ソフトリセット」になり、
  `setup()` のパンくず表示条件（`!= 1 && != 3`）から外れて**ログに何も残らない**。
  意図的に再起動するなら、別に痕跡を残すこと

### ホストから全文を読む（画面を読み上げてもらうより速い）

`opur_log_add()` は `printf` でシリアルにも吐く。`CONFIG_ESP_CONSOLE_UART_DEFAULT`
で UART0 に出るが、`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG` も立っているので
**USB 側にも出る**。パニックのバックトレースも同じ経路に出る。

**`cat /dev/cu.usbmodem*` は使えない。** データが無い間の read 0 を EOF と解釈して
即終了し、「何も出ていない」ように見える（これで一度誤診した）。
`pio device monitor` も stdin が TTY でないと `termios.error` で落ちる。

読み続ける側を自分で書く:

```python
import sys, serial
s = serial.Serial()
s.port = '/dev/cu.usbmodem83201'; s.baudrate = 115200; s.timeout = 0.5
s.dtr = False; s.rts = False      # 触るとリセット／ブートローダ突入がありうる
s.open()
out = open(sys.argv[1], 'wb', buffering=0)
while True:
    d = s.read(4096)
    if d: out.write(d)            # 0 バイトでも EOF 扱いにしない
```

`~/.platformio/penv/bin/python` に pyserial が入っている。
読むときは `tr -d '\r'` を通すと日本語がそのまま読める。

`s.setRTS(True)` → `False` で EN を叩けば、繋いだまま起動しなおさせられる
（esptool と同じ classic reset）。起動ログを頭から取りたいときに使う。

### パニックしたらコアダンプを読む（パンくずより速くて正確）

sdkconfig でコアダンプが Flash に保存される設定になっている
（`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` / `..._DATA_FORMAT_ELF`、
パーティションは `0x7f0000` に 64K）。**関数名と行番号まで分かる。**

`esp-coredump` は ESP-IDF 環境を要求してくるので使えない。先頭 20 バイトが
メタデータで、その後ろがそのまま ELF なので自分で切り出す:

```bash
# シリアル監視は止めてから（ポートが競合する）
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 \
  --port /dev/cu.usbmodem83201 read_flash 0x7f0000 0x8000 core_full.bin

python3 -c "
import struct
d=open('core_full.bin','rb').read()
size=struct.unpack('<I', d[0:4])[0]
open('core.elf','wb').write(d[20:size])
"

~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-gdb \
  -batch -q .pio/build/m5stack-cardputer/firmware.elf core.elf -ex "bt"
```

`firmware.elf` は**そのとき焼いたもの**でないとアドレスが合わない。

## NVS には自分から書かない（021 で退避を撤去した）

書きかけを NVS に自動退避していたが、**その書き込み自体がパニックの発生源**
だったので 021 でまるごとやめた。いまアプリから NVS を読み書きする箇所は無い。

落ちていたのは `nvs_open()` が内部ミューテックスを取る時点:

```
assert failed: xQueueSemaphoreTake queue.c:1549 (pxQueue->uxItemSize == 0)
  nvs::Lock::Lock  →  nvs_open_from_partition  →  nvs_open  →  nvs_save
```

**データの破損ではない。** `esptool erase_region 0x9000 0x5000` で
NVS を全消去しても再発した。壊れているのはロックオブジェクトのほう。
復旧のために置いていた `m5c_nvs_reset()`（`nvs_flash_deinit()` を呼ぶ）を
疑っているが、確証は取っていない。詳細は
`~/claude-store/opur/2026-08-15_nvs_panic_investigation.md`。

**予防的に NVS を作り直すのは避けること。** `nvs_flash_erase()` はパーティション
全体を消すので、ESP-IDF が持つ **WiFi のキャリブレーション情報も一緒に消える**。
実際に接続時間が 650ms → 2500ms に伸びた（`OPUR_WIFI_TIMEOUT_MS` は 3000）。
数回の接続で戻る。

`nvs_flash_init()` を自前で呼ぶ必要も無い。Arduino の `initArduino()` が
`setup()` より前に同じ手順（壊れていたら消して再 init）を済ませている。

## 送信（opur_net）の設計

EQIDEN は 1 つの URL に対するセマフォ。GET で覗いて、**空なら**置ける。

- GET は**消費しない**。受け手が空 PUT するまで塞がったまま（実機で確認）
- 空のときの応答は **200 + Content-Length: 0**（404 でも 204 でもない）
- キューは `/opur/` そのもの。送信済みは `/opur/sent/` へ move。
  `OPUR_%04d.txt` は 0 埋めなので **名前順 = 番号順 = 書いた順**（FIFO）
- 1 回の呼び出しで 1 件だけ。まとめ送りはセマフォ方式では原理的に無理

**送信のトリガは「保存したとき」だけ**。溜まった未送信を流す手段が他に無い。
設計論点として未解決（019 の報告書に案 A/B/C を整理してある）。

手元での確認:

```bash
curl -s URL              # 中身を見る（空なら送信可）
curl -X PUT -d "" URL    # 引き取って空に戻す
```

## 踏んだ罠

- **`CONFIG_ARDUINO_LOOP_STACK_SIZE` は効かない。** `sdkconfig.h` が無条件に
  8192 で再定義するため。`cores/esp32/main.cpp` は `CONFIG_` 無しの
  **`ARDUINO_LOOP_STACK_SIZE`** を先に見るので、そちらを `-D` で渡す
- **半角カタカナは使わない。** efont での実描画幅（8px か 16px か）が読めず、
  桁勘定が狂う。画面は 30 半角桁
- **`ENDPOINT_URL` は 83 文字**（`https://eqiden.com/` + 64 桁トークン）。
  `OPUR_CFG_VAL_MAX`（64）には入らないので `OPUR_CFG_URL_MAX`（160）を使う。
  `set_str()` は黙って切り詰めるだけなので、超えても気づけない
- HTTPClient の `GET()` が返す **`-1` は mbedTLS のエラーではない**。
  `ssl_client.cpp` がソケット層で返す独自の値で、`mbedtls_strerror(-1)` が
  "Generic error" になるため紛らわしい。経路の問題と TLS の問題を分けるには、
  暗号化なしで :443 を叩く（`opur_net.cpp` の `log_why()` に実装済み）

## 作業の進め方

- 大きめの変更はブランチを切る（`git checkout -b <name>` → main に ff マージ → 削除）
- 実機の挙動を推測で語らない。**ELF を読む・esptool で実機から読む・
  失敗時のログを足して焼き直す**のどれかで確定させる
- 実機を操作するのはマスター。ちびエルマー側は m4max から curl や esptool で
  検証する。マスターに小さい画面のログを読み上げさせるより、
  **失敗時だけ材料を残す診断を足して焼き直す**ほうが速い
