# toio-pen-plotter

XIAO ESP32C3 から BLE で最大 2 台の toio Core Cube に接続し、ボタン操作でペンプロッタ用の移動を行う Arduino スケッチです。

OLED に接続状態と操作ガイドを表示し、NeoPixel と toio 本体のインジケータで接続状態を確認できます。

## 構成

- `toio-pen-plotter.ino`: メインの Arduino スケッチ
- `config.h`: XIAO ESP32C3 のピン設定
- `diagram.json`: Wokwi 用の配線図
- `wokwi.toml`: Wokwi 実行用設定
- `libraries.txt`: Wokwi 用ライブラリ一覧
- `stl/`: ペンホルダー、キューブホルダー、ストッパーの 3D モデル

## 必要なもの

- Seeed Studio XIAO ESP32C3
- toio Core Cube 1 台または 2 台
- SSD1306 OLED ディスプレイ 128x64、I2C、アドレス `0x3C`
- NeoPixel 3 LED
- ブザー
- プッシュボタン 4 個
- Arduino IDE または arduino-cli
- ESP32 Arduino core
- ライブラリ
  - NimBLE-Arduino
  - Adafruit GFX Library
  - Adafruit SSD1306
  - Adafruit NeoPixel

## ピン設定

`config.h` の定義は以下の通りです。

| 機能 | ピン | XIAO 表記 |
| --- | --- | --- |
| OLED SDA | GPIO 6 | D4 |
| OLED SCL | GPIO 7 | D5 |
| UP ボタン | GPIO 20 | D7 |
| DOWN ボタン | GPIO 8 | D8 |
| LEFT ボタン | GPIO 9 | D9 |
| RIGHT ボタン | GPIO 10 | D10 |
| ブザー | GPIO 5 | D3 |
| NeoPixel | GPIO 2 | D0 |

ボタンは `INPUT_PULLUP` で使われ、押下時に GND に落ちる構成です。

## ビルド

Arduino IDE で開く場合は、`toio-pen-plotter.ino` を開き、ボードに XIAO ESP32C3 を選択して書き込んでください。

arduino-cli を使う場合の例:

```sh
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C3 .
arduino-cli upload --fqbn esp32:esp32:XIAO_ESP32C3 -p <PORT> .
```

`<PORT>` は接続しているシリアルポートに置き換えてください。

## 使い方

起動すると OLED に `Mode: IDLE` と表示されます。

### 接続

1. toio Core Cube の電源を入れます。
2. `UP` ボタンを短く押すと、近くの toio を 5 秒間スキャンします。
3. 見つかった未接続の toio のうち、RSSI が最も強いものへ接続します。
4. 1 台目は `toio1`、2 台目は `toio2` として扱われます。
5. 2 台接続できると自動で Play mode に入ります。

1 台だけ接続した状態でも、`DOWN` ボタンで Play mode に入れます。

## 操作

### Idle / Connected mode

| 操作 | 動作 |
| --- | --- |
| UP 短押し | 次の toio に接続 |
| DOWN 短押し | 1 台以上接続済みなら Play mode に移行 |

### Play mode

`toio1` は長押しで連続操作します。ボタンを離すと停止します。

| 操作 | toio1 の動作 |
| --- | --- |
| UP 長押し | 前進、速度 `50` |
| DOWN 長押し | 後退、速度 `-20` |
| LEFT 長押し | 左旋回、左右速度 `-30 / 30` |
| RIGHT 長押し | 右旋回、左右速度 `30 / -30` |

`toio2` は短押しで 150ms だけ動きます。

| 操作 | toio2 の動作 |
| --- | --- |
| UP 短押し | 後退、速度 `-30` |
| DOWN 短押し | 前進、速度 `30` |

## 表示

OLED には以下が表示されます。

- 現在のモード
  - `IDLE`
  - `CONNECTING`
  - `CONNECTED`
  - `PLAY`
- `toio1` / `toio2` の接続名
- 操作ガイド
- 接続エラーやステータス

NeoPixel は接続状態を表します。

| 状態 | 表示 |
| --- | --- |
| 未接続 | 消灯 |
| toio1 接続済み | 1 個目の LED が緑 |
| toio2 接続済み | 2 個目の LED が青 |

toio 本体のインジケータも、`toio1` は緑、`toio2` は青に設定されます。

## シリアルコマンド

シリアルモニタから以下のコマンドを送れます。

| コマンド | 動作 |
| --- | --- |
| `c` | 次の toio に接続 |
| `p` | Play mode に移行 |
| `s` | 全 toio のモーター停止 |
| `i` | Idle mode に戻る |

起動時のシリアル速度は `115200` bps です。

## BLE 接続仕様

スケッチは toio の BLE Service UUID またはデバイス名で toio を検出します。

- Service UUID: `10B20100-5B3B-4571-9508-CF3EFCD7BBAE`
- Motor Characteristic: `10B20102-5B3B-4571-9508-CF3EFCD7BBAE`
- Indicator Characteristic: `10B20103-5B3B-4571-9508-CF3EFCD7BBAE`
- Sound Characteristic: `10B20104-5B3B-4571-9508-CF3EFCD7BBAE`

接続時は最大 3 回リトライし、接続済みの toio は再接続候補から除外します。

## Wokwi

`diagram.json` と `wokwi.toml` が含まれているため、Wokwi で UI 周辺の配線や表示を確認できます。

ただし、toio との BLE 接続や実機のモーター動作は実機環境で確認してください。

## 3D モデル

`stl/` には以下のモデルがあります。

- `cube-holder.stl`
- `pen-holder.stl`
- `stopper.stl`

ペンプロッタとして使うための toio 固定具やペン保持部品として利用できます。
