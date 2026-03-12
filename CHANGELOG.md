# Changelog

## v0.1.0 (2026-03-12)

### Added
- PLC/INV デュアルモードAP Web UI
- PLC: MC/Modbus 通信方式切替
- INV: 計算機リンク/Modbus RTU 対応
- PLC デバイス選択ドロップダウン（D/X/Y/M/L/F/V/B/W/R/ZR/S/T/C/TN/CN）
- PLC ダッシュボード（word数値表示、bitグリッド表示）
- RTC同期、SD CSVログ、SPIFFS保存

### Changed
- UIのモード別表示最適化
- ポーリングと通信待ち時間の高速化チューニング
- Modbus診断表示（mbRc/mbOp）追加
- Save & Apply でPLC項目含めた一括保存

### Fixed
- PLC/INVの読取ボタン表示不整合
- PLC bit 32bit表示不足
- bitグリッドの画面はみ出し
- SPIFFS保存の信頼性改善

### Removed
- Modbus局番スキャンボタン/エンドポイント（運用版整理）
