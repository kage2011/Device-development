# ESP32S3/XIAO 人物検出 開発成果アーカイブ

- 保存日: 2026-03-08
- 主な成果:
  - XIAO ESP32S3 Senseで `shot` コマンド撮影フロー確立
  - 画質/色補正適用
  - ESP-IDF + ESP-WHO object_detect を XIAOピンへ移植
  - シリアルCSV出力 (`ts_ms,filtered_person,raw_score,raw_person`)
  - LED連動 (active-low補正)
  - model切替: pedestrian -> human_face_detect

## 含まれるもの
- `camera_probe/` : Arduinoスケッチと撮影サンプル
- `espwho_logs/` : 起動/検証ログ
- `notes/espwho_git_log.txt` : 実装コミット履歴
