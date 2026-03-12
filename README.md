# Device-development

XIAO ESP32C3 + MAX485 で、PLC / インバータのRS485通信を行う開発リポジトリです。

## 主な機能

- AP Web UI（`RS485COM_*`）
- 通信対象切替: PLC / INV
- PLC:
  - 通信方式: MCプロトコル / Modbus RTU
  - デバイス読取: D/X/Y/M/L/F/V/B/W/R/ZR/S/T/C/TN/CN
  - word/bit 表示、16/32bit表示
- INV:
  - 三菱系（計算機リンク / Modbus RTU）
  - Hz / I / V / Status / Alarm表示
- RTC時刻同期、SD CSVログ、SPIFFS設定保存

## 主要ファイル

- `xiao_c3_rs485_mc/xiao_c3_rs485_mc.ino` : メインスケッチ

## 使い方（最短）

1. スケッチを書き込み
2. Wi-Fi AP `RS485COM_*` に接続
3. `http://192.168.4.1` を開く
4. 通信対象・通信方式・シリアル設定を合わせて `Save & Apply`
5. 読み取り画面で状態確認

## 推奨初期設定

- PLC (MC):
  - 9600 / 7O1 / station 0（内部固定）
- PLC (Modbus):
  - 9600 / 8E1 / station 1（内部運用）
- INV (Mitsubishi):
  - 計算機リンク: 19200 / 8E2
  - Modbus RTU: 8bit固定

## 配線例（最小）

```text
XIAO ESP32C3          MAX485                PLC/INV RS485
-------------------------------------------------------------
5V  ----------------> VCC
GND ----------------> GND -----------------> SG/GND
GPIO21 (TX) -------> DI
GPIO20 (RX) <------- RO
GPIO4  (DE) -------> DE
GPIO3  (RE) -------> /RE
A/B --------------- A/B -------------------> A/B
```

- A/Bの極性が逆だと通信しません（無応答になります）。
- 終端抵抗(120Ω)はネットワーク端点のみ有効にしてください。

## 画面イメージ

### PLC/INV UIデモ

![UI frame 01](results/ui_demo_frames/frame_01.png)
![UI frame 02](results/ui_demo_frames/frame_02.png)
![UI frame 03](results/ui_demo_frames/frame_03.png)

### PLCデータ例

![FX5UC D2000 sample](fx5uc_d2000_10s.png)


## すぐ試す（出荷前確認）

- `RELEASE_CHECKLIST.md` に沿って最終確認してください。
- 特に「MC/Modbus 切替」「再起動後の設定保持」「CSV追記」を確認してから出荷推奨です。

## メモ

- Modbusは機種・設定でレジスタマップが異なるため、実機に合わせた調整が必要です。
- 高速ポーリング時は通信線品質・終端・A/B極性の影響を強く受けます。
