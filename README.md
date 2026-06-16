# CM3-HM BLE Scanner

チョコザップ体重計 `CM3-HM` のBLE advertisementをESP32-C3で拾い、シリアルモニタへ出すための診断用ファームウェアです。

## 目的

まず体重値をDiscordへ送る前に、体重計が送っているBLEデータをESP32で取得できるか確認します。

## 開発環境

- Board: Seeed Studio XIAO ESP32C3
- Framework: Arduino
- Build tool: PlatformIO
- Serial monitor: 115200 baud

このPCではArduino CLIでもビルド・アップロードできます。

```sh
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C3:PartitionScheme=huge_app .
arduino-cli upload -p /dev/cu.usbmodem11301 --fqbn esp32:esp32:XIAO_ESP32C3:PartitionScheme=huge_app .
arduino-cli monitor -p /dev/cu.usbmodem11301 -c baudrate=115200
```

## 使い方

1. VS CodeにPlatformIO拡張を入れる
2. このフォルダをVS Codeで開く
3. XIAO ESP32C3をUSB接続する
4. PlatformIOからUploadする
5. Serial Monitorを `115200` baudで開く
6. 体重計に乗る

`CM3-HM`、Service UUID `FFB0`、Manufacturer Data先頭 `A0 AC` / `AC A0` のいずれかに一致したBLE広告だけを処理します。
通常時は測定中のBLE広告ログを表示しません。
測定完了候補を検出すると、直前の測定中ペイロードとセットで `MEASUREMENT_COMPLETE` 行を表示します。
同じ測定完了ペイロードが連続する場合、`MEASUREMENT_COMPLETE` は最初の1回だけ表示します。

BLE広告の詳細ログを見たい場合は、[src/main.cpp](src/main.cpp) の以下を `true` に変更して再アップロードします。

```cpp
constexpr bool DEBUG_BLE_PACKETS = false;
```

HTTPやSheets連携の詳細ログを見たい場合は、[src/main.cpp](src/main.cpp) の以下を `true` に変更して再アップロードします。

```cpp
constexpr bool DEBUG_HTTP = false;
```

## 次に欲しいログ

シリアルモニタに出た以下の値を、測定時の実際の体重とセットで残してください。

- `rssi`
- `name`
- `service uuid`
- `manufacturer data`
- `full decoded`

例:

```text
実測: 68.4 kg
[44305] rssi=-58 name=CM3-HM data=AC A0 A8 81 48 6A 58 A0 A2 B3 A0 A2 06 BD payload=A2 B3 A0 A2 06 BD stable_candidate=yes
MEASUREMENT_COMPLETE stable_payload=A2 B3 A0 A2 06 BD last_live_payload=A0 2D 84 EE 0D AC last_live_raw_be=11652 estimated_weight_kg=74.8
```

通常体重と、何かを持って少し重くした状態の2パターンがあると、どのバイトが体重か解析しやすくなります。

`estimated_weight_kg` は、現時点では以下2点から作った暫定キャリブレーションです。

- 74.8 kg -> `last_live_raw_be=11652`
- 77.7 kg -> `last_live_raw_be=11663`

追加サンプルでズレる場合は、キャリブレーション式を更新します。

## Discord通知

`src/secrets.example.h` を参考に `src/secrets.h` を編集します。
`src/secrets.h` は `.gitignore` 対象です。

```cpp
constexpr const char *WIFI_SSID = "YOUR_WIFI_SSID";
constexpr const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
constexpr const char *DISCORD_WEBHOOK_URL =
    "https://discord.com/api/webhooks/xxxxxxxx/yyyyyyyy";
constexpr const char *SHEETS_WEBHOOK_URL =
    "https://script.google.com/macros/s/xxxxxxxx/exec";
constexpr const char *SHEETS_TOKEN = "CHANGE_ME_LONG_RANDOM_TOKEN";
```

`src/secrets.h` がダミー値のままの場合、シリアルには `Discord disabled until src/secrets.h is configured.` と表示され、Discord送信は行いません。

Webhook送信まで有効化した後は、測定完了時に以下のような通知をDiscordへ送ります。

```text
もっくんが体重を測りました！
体重: 76.9kg
```

## Google Sheets連携

Google Sheetsは、ESP32から直接Sheets APIを叩かず、Google Apps ScriptのWeb Appを中継します。
ESP32からApps Scriptへは、Google側のPOSTリダイレクトとの相性を避けるため、GET + query parametersで送信します。

```text
ESP32 -> Apps Script Web App -> Google Sheets
```

手順:

1. Google Sheetsで記録用スプレッドシートを作る
2. `拡張機能 > Apps Script` を開く
3. [apps-script/Code.gs](apps-script/Code.gs) の内容を貼り付ける
4. `SCRIPT_TOKEN` を任意の長い文字列に変える
5. `デプロイ > 新しいデプロイ > ウェブアプリ` を選ぶ
6. 実行ユーザーは `自分`
7. アクセスできるユーザーは `全員`
8. デプロイURLをコピーする
9. `src/secrets.h` の `SHEETS_WEBHOOK_URL` にデプロイURLを入れる
10. `src/secrets.h` の `SHEETS_TOKEN` に `SCRIPT_TOKEN` と同じ値を入れる
11. ファームウェアを再アップロードする

Sheets連携が未設定の場合、測定時にシリアルへ以下のように出て、Discord通知だけ行います。

```text
Sheets skipped: src/secrets.h is not configured
```

Sheets連携が成功すると、シリアルへ以下のように出ます。

```text
Sheets sent: weight=76.9 kg response={"ok":true,"sheet":"measurements","row":2,"weightKg":76.9}
```

スプレッドシートには `measurements` シートが作られ、以下の列で追記されます。

```text
timestamp, weight_kg, raw_be, stable_payload, live_payload, device
```

問題調査時は、Apps Script画面の左メニュー `実行数` から `doPost` の実行ログを確認します。
[apps-script/Code.gs](apps-script/Code.gs) は以下をログ出力します。

- POST本文を受け取れたか
- tokenが届いているか
- `weightKg` / `rawBe` / payload値
- token不一致
- append成功時のシート名と行番号
- 例外内容
