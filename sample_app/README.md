# ZMK IPC Sample App

ZMK native_sim プロセスと通信するサンプル Python アプリケーション。

## アーキテクチャ

```
 sample_app                     ZMK native_sim
 ─────────────────────          ─────────────────────────────
 send_keys.py  ──────────────►  /tmp/zmk_kscan_ipc.sock
  (ClientMessage)               kscan_ipc driver
                                  ↓ keymap processing
 watch_events.py  ◄─────────────  /tmp/zmk_ipc.sock
  (ZmkEvent)                    ipc_observer module
```

**Wire format**: `[4-byte big-endian length][protobuf bytes]`（HTTP/2 gRPC ではなく独自フレーミング）

## ファイル構成

| ファイル | 説明 |
|---|---|
| `zmk_ipc_pb2.py` | proto から生成された型定義 |
| `zmk_client.py` | Unix socket + length-prefix フレーミングの transport adapter |
| `watch_events.py` | ZMK イベントを受信して表示するスクリプト |
| `send_keys.py` | キーイベントを送信するスクリプト |
| `demo.py` | 両方を組み合わせたインタラクティブデモ |
| `generate_proto.sh` | `zmk_ipc_pb2.py` を再生成するスクリプト |

## セットアップ

```bash
pip install -r requirements.txt
```

`zmk_ipc_pb2.py` はリポジトリに含まれています。proto を変更した場合は再生成してください：

```bash
./generate_proto.sh
```

## 使い方

### 前提

ZMK native_sim プロセスが起動していること：

```bash
./build/zephyr/zmk.exe &
```

### 1. イベントを監視する

```bash
python watch_events.py
```

出力例：
```
[kscan   ] PRESS    pos=0     source=255  ts=1234 ms
[keyboard] transport=TRANSPORT_USB     modifiers=0x00  keys=[0x04]
[keyboard] transport=TRANSPORT_USB     modifiers=0x00  keys=[-]
[kscan   ] RELEASE  pos=0     source=255  ts=1284 ms
```

### 2. キーを送信する

```bash
# position 0 (row=0, col=0) を1回押す
python send_keys.py 0

# position 0, 1, 2 を順に押す
python send_keys.py 0 1 2
```

### 3. インタラクティブデモ

```bash
python demo.py
```

プロンプトで操作：

```
> 0          # position 0 を press/release
> r 1 3      # row=1, col=3 を press/release
> h          # ヘルプ表示
> q          # 終了
```

## キーマトリクスのアドレス

`native_sim/native/zmk_ipc` ボードは **4行 × 12列（48キー）**：

```
linear position = row * 12 + col

row 0:  0  1  2  3  4  5  6  7  8  9 10 11
row 1: 12 13 14 15 16 17 18 19 20 21 22 23
row 2: 24 25 26 27 28 29 30 31 32 33 34 35
row 3: 36 37 38 39 40 41 42 43 44 45 46 47
```
