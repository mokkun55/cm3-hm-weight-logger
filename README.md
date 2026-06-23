# CM3-HM Auto Weight Logger

チョコザップ体重計 `CM3-HM` のBLE広告を Seeed Studio XIAO ESP32C3 で受け取り、体重をDiscordとGoogle Sheetsへ自動記録するファームウェアです。

## 概要

体重計に乗ると、ESP32-C3が `CM3-HM` のBLE広告を検出します。manufacturer dataから測定完了を推定し、測定値が取れたらWi-Fiを起動してDiscord Webhookへ通知し、Google Apps Script経由でGoogle Sheetsへ追記します。送信が終わるとWi-Fiはすぐに停止します。

```text
CM3-HM scale
  -> BLE advertisement
  -> XIAO ESP32C3
  -> Discord Webhook
  -> Google Apps Script Web App
  -> Google Sheets
```

## 対応デバイス

- Scale: チョコザップ `CM3-HM`
- MCU: Seeed Studio XIAO ESP32C3
- Framework: Arduino
- Build tools: Arduino CLI or PlatformIO
- Serial monitor: `115200` baud

## 現状の注意

- `CM3-HM` の公式BLE仕様は未公開のため、この実装は実測ログから推定したプロトコルに基づきます。
- 体重変換式は現時点で 74.8kg / 76.9kg / 77.7kg 付近のサンプルから作った暫定キャリブレーションです。
- 待機時の発熱を抑えるため、BLEは passive scan、Wi-Fi は送信時だけ有効にしています。
- `src/secrets.h` にはWi-FiパスワードやWebhook URLを入れるため、Git管理対象外です。
- Appleヘルスケア連携は未実装です。HealthKitへ書くにはiPhoneアプリやショートカットなど、iOS側の処理が必要です。

## セットアップ

### 1. 秘密情報を設定する

`src/secrets.example.h` をコピーして `src/secrets.h` を作ります。

```sh
cp src/secrets.example.h src/secrets.h
```

`src/secrets.h` を編集します。

```cpp
#pragma once

constexpr const char *WIFI_SSID = "YOUR_WIFI_SSID";
constexpr const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
constexpr const char *DISCORD_WEBHOOK_URL =
    "https://discord.com/api/webhooks/xxxxxxxx/yyyyyyyy";
constexpr const char *SHEETS_WEBHOOK_URL =
    "https://script.google.com/macros/s/xxxxxxxx/exec";
constexpr const char *SHEETS_TOKEN = "CHANGE_ME_LONG_RANDOM_TOKEN";
```

`SHEETS_WEBHOOK_URL` と `SHEETS_TOKEN` はGoogle Sheets連携を使う場合だけ必要です。未設定でもDiscord通知は動きます。

### 2. ビルドとアップロード

Arduino CLIを使う場合:

```sh
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C3:PartitionScheme=huge_app .
arduino-cli upload -p /dev/cu.usbmodem11301 --fqbn esp32:esp32:XIAO_ESP32C3:PartitionScheme=huge_app .
arduino-cli monitor -p /dev/cu.usbmodem11301 -c baudrate=115200
```

`/dev/cu.usbmodem11301` は環境によって変わります。確認するには:

```sh
arduino-cli board list
```

PlatformIOを使う場合は、このリポジトリをVS Codeで開いてUploadします。`platformio.ini` では `huge_app.csv` パーティションを使います。

### 3. BLEスキャン設定

`src/main.cpp` では待機時の発熱を抑えるため、BLEスキャンを軽めに設定しています。

```cpp
constexpr int SCAN_INTERVAL = 320;
constexpr int SCAN_WINDOW = 40;
constexpr bool ACTIVE_SCAN = false;
```

- `ACTIVE_SCAN = false`: passive scan で追加応答を取りにいかない
- `SCAN_INTERVAL = 320`
- `SCAN_WINDOW = 40`

検出率を優先したくなったら、まず `SCAN_WINDOW` を少し戻して調整してください。

## Discord通知

Discord側でWebhook URLを作り、`DISCORD_WEBHOOK_URL` に設定します。

測定完了時の通知例:

```text
もっくんが体重を測りました！
体重: 76.9kg
```

必要なら [src/main.cpp](src/main.cpp) の `buildDiscordJson()` 内の文面を変更してください。

## Google Sheets連携

Google Sheetsへは、ESP32からSheets APIを直接叩かず、Google Apps ScriptのWeb Appを中継します。ESP32からApps Scriptへは、Google側のPOSTリダイレクトとの相性を避けるため、GET + query parametersで送っています。

### Apps Script設定

1. Google Sheetsで記録用スプレッドシートを作る
2. `拡張機能 > Apps Script` を開く
3. [apps-script/Code.gs](apps-script/Code.gs) の内容を貼り付ける
4. `SCRIPT_TOKEN` を長いランダム文字列へ変更する
5. `デプロイ > 新しいデプロイ > ウェブアプリ` を選ぶ
6. `次のユーザーとして実行`: `自分`
7. `アクセスできるユーザー`: `全員`
8. デプロイURLを `src/secrets.h` の `SHEETS_WEBHOOK_URL` に入れる
9. `SCRIPT_TOKEN` と同じ値を `src/secrets.h` の `SHEETS_TOKEN` に入れる
10. ファームウェアを再アップロードする

`アクセスできるユーザー` を `全員` にするため、URLを知っている人はアクセスできます。`SHEETS_TOKEN` を長くランダムな値にして、簡易認証として使います。

### Sheetsの列

`measurements` シートが自動作成され、以下の列で追記されます。

```text
timestamp, weight_kg, raw_be, stable_payload, live_payload, device
```

`device` 列には `CM3-HM/ADV` のように取得元が入ります。

## 通常ログ

通常時のシリアルログは必要最小限です。

```text
CM3-HM auto weight logger
Serial: 115200 baud
BLE transport mode: advertisement-only
BLE scan mode: passive interval=320 window=40
WiFi stays off until a measurement is ready to upload.
Step on the scale. Measurement summaries are printed.
MEASUREMENT_COMPLETE source=advertisement estimated_weight_kg=76.9 raw=11660
WiFi connecting to YOUR_WIFI_SSID
WiFi connected, ip=192.168.2.188
Sheets sent: row=4
Discord sent
WiFi disconnecting
```

## デバッグ

BLE広告の詳細ログを見たい場合:

```cpp
constexpr bool DEBUG_BLE_PACKETS = true;
```

HTTPやSheets連携の詳細ログを見たい場合:

```cpp
constexpr bool DEBUG_HTTP = true;
```

どちらも [src/main.cpp](src/main.cpp) にあります。変更後は再アップロードしてください。

Apps Script側のログは、Apps Script画面の左メニュー `実行数` から確認できます。`Code.gs` は受信パラメータ、token不一致、append成功行、例外内容をログ出力します。

## BLE解析メモ

観測した `CM3-HM` のBLE広告は以下のような形です。

```text
AC A0 | A8 81 48 6A 58 A0 | A2 B2 A0 A2 06 BC
```

おおまかな解釈:

- 先頭2 bytes: manufacturer idらしき値
- 次の6 bytes: 体重計MACアドレスの逆順らしき値
- 最後の6 bytes: 測定ペイロード

測定中は `A0` / `20` で始まるpayloadが出て、測定完了後は `A2` で始まるpayloadが繰り返されます。体重値は完了payloadではなく、直前のlive payloadから `last_live_raw_be` を作って推定しています。

現在の暫定キャリブレーション:

```text
74.8 kg -> last_live_raw_be=11652
76.9 kg -> last_live_raw_be=11660
77.7 kg -> last_live_raw_be=11663
```

この式は [src/main.cpp](src/main.cpp) の以下で調整できます。

```cpp
constexpr uint16_t CALIBRATION_RAW = 11652;
constexpr float CALIBRATION_WEIGHT_KG = 74.8f;
constexpr float CALIBRATION_KG_PER_RAW_STEP = 2.9f / 11.0f;
```

## トラブルシュート

### `Discord disabled` / `Sheets skipped` と出る

`src/secrets.h` が未設定か、ダミー値のままです。`src/secrets.example.h` をコピーして実値を入れてください。

### アップロード時に `port is busy`

シリアルモニタがポートを掴んでいます。Arduino IDE / VS Code / `arduino-cli monitor` を閉じてから再実行してください。

### Sheetsが記録されない

`DEBUG_HTTP = true` にして再アップロードし、HTTPステータスとレスポンスを確認してください。Apps Script側の `実行数` ログも確認します。

### Apps Scriptのデプロイを更新したのに反映されない

Apps Scriptはコード保存だけではWeb Appに反映されません。`デプロイを管理` から新しいバージョンとして再デプロイしてください。

## セキュリティ

- `src/secrets.h` は `.gitignore` 済みです。
- Webhook URL、Wi-Fiパスワード、Apps Script URL、tokenは公開しないでください。
- Discord Webhook URLを誤って公開した場合は、Discord側でWebhookを再生成してください。
