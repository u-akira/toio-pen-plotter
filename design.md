# Design

## 目的

ESP32 から BLE で最大 2 台の `toio` に直接接続し、接続状態を画面と LED で確認しながら操作する。

## 画面遷移

### 1. 初期状態

- `toio` には未接続
- 画面に `UP: Connect` を表示する
- `UP` 短押しで近くの未接続 `toio` を探索して接続する

### 2. 1 台接続済み

- `toio1: xxx` を表示する
  - `xxx` は toio の short name
- 画面に `UP: Connect` と `DOWN: Play` を表示する
- `UP` 短押しで 2 台目の `toio` に接続する
- `DOWN` 短押しで Play mode に移行する
- 2 台目接続に失敗した場合は、1 台接続済み画面に戻る

### 3. 2 台接続済み

- `toio2: xxx` を表示する
- 確認画面は挟まず、自動で Play mode に移行する
- 2 台接続済みの場合、それ以上の接続は受け付けない

### 4. Play mode

- 接続済みの `toio1` / `toio2` を操作する
- Play mode 中の追加接続は行わない

## 操作

### 接続前/接続済み画面

- `UP` 短押し: Connect
- `DOWN` 短押し: Play mode へ移行
  - 1 台以上接続済みの場合のみ有効

### Play mode: `toio1`

- `UP` 長押し: 前進速度 `50`
- `DOWN` 長押し: 後退速度 `-50`
- `LEFT` 長押し: 左旋回
- `RIGHT` 長押し: 右旋回
- 長押し解除: 停止

### Play mode: `toio2`

- `UP` 短押し: 短時間後退速度 `-30`
- `DOWN` 短押し: 短時間前進速度 `30`

## LED 表示

- 未接続: 全 LED 消灯
- `toio1` 接続済み: 一番左の LED を緑
- `toio2` 接続済み: 真ん中の LED を青
- Play mode 中も同じ接続状態表示を維持する

## 実装メモ

- BLE 接続には `NimBLE-Arduino` を使用する
- スキャンでは toio Service UUID または name が `toio` で始まるデバイスを候補にする
- 複数候補がある場合は RSSI が最も強い未接続 toio を選ぶ
- toio 接続時は MTU 交換を行わずに接続する
- 接続先は未接続スロットに割り当てる
- `toio1` は 1 台目、`toio2` は 2 台目として表示する
- シリアルログには scan/connect/gatt の主要な調査ログを出力する
