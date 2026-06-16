#include <Arduino.h>
#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEUtils.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "secrets.h"

namespace {

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t SCAN_SECONDS = 5;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 30000;
constexpr uint32_t NOTIFICATION_DEBOUNCE_MS = 120000;
constexpr uint32_t HTTP_TIMEOUT_MS = 15000;
constexpr int SCAN_INTERVAL = 160;
constexpr int SCAN_WINDOW = 80;
constexpr bool DEBUG_BLE_PACKETS = false;
constexpr bool DEBUG_HTTP = false;

// Temporary calibration from observed samples:
// 74.8 kg -> raw 11652, 77.7 kg -> raw 11663.
// This is intentionally kept as a calibration, not a protocol constant, until
// more measurements confirm the scale.
constexpr uint16_t CALIBRATION_RAW = 11652;
constexpr float CALIBRATION_WEIGHT_KG = 74.8f;
constexpr float CALIBRATION_KG_PER_RAW_STEP = 2.9f / 11.0f;

BLEScan *scanner = nullptr;
String lastManufacturerDataHex;
uint32_t lastRepeatedLogMillis = 0;
String lastLivePayloadHex;
uint16_t lastLiveRawBe = 0;
String lastAnnouncedStablePayloadHex;
uint16_t lastNotifiedRawBe = 0;
uint32_t lastNotificationMillis = 0;
uint32_t lastWifiAttemptMillis = 0;

struct Measurement {
  bool pending = false;
  float weightKg = 0.0f;
  uint16_t rawBe = 0;
  String stablePayloadHex;
  String livePayloadHex;
};

Measurement pendingMeasurement;

String bytesToHex(const String &bytes) {
  String out;
  out.reserve(bytes.length() * 3);

  for (size_t i = 0; i < bytes.length(); i++) {
    if (i > 0) {
      out += ' ';
    }

    char buf[3];
    snprintf(buf, sizeof(buf), "%02X", static_cast<uint8_t>(bytes[i]));
    out += buf;
  }

  return out;
}

bool hasChocozapScaleSignal(BLEAdvertisedDevice &device) {
  if (device.haveName() && device.getName() == "CM3-HM") {
    return true;
  }

  if (device.haveServiceUUID() &&
      device.isAdvertisingService(BLEUUID(static_cast<uint16_t>(0xFFB0)))) {
    return true;
  }

  if (device.haveManufacturerData()) {
    const String manufacturerData = device.getManufacturerData();
    if (manufacturerData.length() >= 2) {
      const uint8_t byte0 = static_cast<uint8_t>(manufacturerData[0]);
      const uint8_t byte1 = static_cast<uint8_t>(manufacturerData[1]);
      if ((byte0 == 0xA0 && byte1 == 0xAC) ||
          (byte0 == 0xAC && byte1 == 0xA0)) {
        return true;
      }
    }
  }

  return false;
}

bool isPlaceholder(const char *value) {
  return value == nullptr || strlen(value) == 0 ||
         strncmp(value, "YOUR_", 5) == 0 ||
         strncmp(value, "CHANGE_ME", 9) == 0 ||
         strncmp(value, "https://discord.com/api/webhooks/xxxxxxxx/", 42) ==
             0 ||
         strncmp(value, "https://script.google.com/macros/s/xxxxxxxx/", 44) ==
             0;
}

bool wifiConfigured() {
  return !isPlaceholder(WIFI_SSID) && !isPlaceholder(WIFI_PASSWORD);
}

bool discordConfigured() {
  return wifiConfigured() && !isPlaceholder(DISCORD_WEBHOOK_URL);
}

bool sheetsConfigured() {
  return wifiConfigured() && !isPlaceholder(SHEETS_WEBHOOK_URL) &&
         !isPlaceholder(SHEETS_TOKEN);
}

void printSheetsConfigStatus() {
  if (!DEBUG_HTTP) {
    return;
  }

  Serial.printf(
      "Sheets config: wifi=%s url=%s token=%s url_len=%u token_len=%u\n",
      wifiConfigured() ? "ok" : "missing",
      !isPlaceholder(SHEETS_WEBHOOK_URL) ? "ok" : "missing",
      !isPlaceholder(SHEETS_TOKEN) ? "ok" : "missing",
      static_cast<unsigned>(strlen(SHEETS_WEBHOOK_URL)),
      static_cast<unsigned>(strlen(SHEETS_TOKEN)));
}

String bytesSliceToHex(const String &bytes, size_t start) {
  if (start >= bytes.length()) {
    return "(none)";
  }

  String out;
  out.reserve((bytes.length() - start) * 3);

  for (size_t i = start; i < bytes.length(); i++) {
    if (i > start) {
      out += ' ';
    }

    char buf[3];
    snprintf(buf, sizeof(buf), "%02X", static_cast<uint8_t>(bytes[i]));
    out += buf;
  }

  return out;
}

bool isLikelyStableMeasurement(const String &manufacturerData) {
  return manufacturerData.length() == 14 &&
         static_cast<uint8_t>(manufacturerData[8]) == 0xA2;
}

bool isLikelyLiveMeasurement(const String &manufacturerData) {
  if (manufacturerData.length() != 14) {
    return false;
  }

  const uint8_t status = static_cast<uint8_t>(manufacturerData[8]);
  return status == 0xA0 || status == 0x20;
}

uint16_t readPayloadRawBe(const String &manufacturerData) {
  if (manufacturerData.length() < 11) {
    return 0;
  }

  return (static_cast<uint16_t>(static_cast<uint8_t>(manufacturerData[9]))
          << 8) |
         static_cast<uint8_t>(manufacturerData[10]);
}

float estimateWeightKg(uint16_t raw) {
  return CALIBRATION_WEIGHT_KG +
         (static_cast<int32_t>(raw) - CALIBRATION_RAW) *
             CALIBRATION_KG_PER_RAW_STEP;
}

void rememberMeasurementState(const String &manufacturerData) {
  if (isLikelyLiveMeasurement(manufacturerData)) {
    lastLivePayloadHex = bytesSliceToHex(manufacturerData, 8);
    lastLiveRawBe = readPayloadRawBe(manufacturerData);
  }
}

bool shouldAnnounceMeasurement(const String &stablePayloadHex, uint16_t rawBe) {
  const uint32_t now = millis();
  if (stablePayloadHex != lastAnnouncedStablePayloadHex) {
    return true;
  }

  if (rawBe != lastNotifiedRawBe) {
    return true;
  }

  return now - lastNotificationMillis >= NOTIFICATION_DEBOUNCE_MS;
}

void queueMeasurement(float weightKg, uint16_t rawBe,
                      const String &stablePayloadHex,
                      const String &livePayloadHex) {
  pendingMeasurement.pending = true;
  pendingMeasurement.weightKg = weightKg;
  pendingMeasurement.rawBe = rawBe;
  pendingMeasurement.stablePayloadHex = stablePayloadHex;
  pendingMeasurement.livePayloadHex = livePayloadHex;

  lastAnnouncedStablePayloadHex = stablePayloadHex;
  lastNotifiedRawBe = rawBe;
  lastNotificationMillis = millis();
}

void handleStableMeasurement(const String &manufacturerData) {
  if (!isLikelyStableMeasurement(manufacturerData)) {
    return;
  }

  const String stablePayloadHex = bytesSliceToHex(manufacturerData, 8);
  if (!shouldAnnounceMeasurement(stablePayloadHex, lastLiveRawBe)) {
    return;
  }

  const float weightKg = estimateWeightKg(lastLiveRawBe);
  queueMeasurement(weightKg, lastLiveRawBe, stablePayloadHex, lastLivePayloadHex);

  Serial.printf("MEASUREMENT_COMPLETE estimated_weight_kg=%.1f\n", weightKg);

  if (DEBUG_HTTP) {
    Serial.printf(
        "MEASUREMENT_DETAIL stable_payload=%s last_live_payload=%s "
        "last_live_raw_be=%u\n",
        stablePayloadHex.c_str(),
        lastLivePayloadHex.length() > 0 ? lastLivePayloadHex.c_str() : "(none)",
        lastLiveRawBe);
  }
}

void printPacketSummary(BLEAdvertisedDevice &device,
                        const String &manufacturerData) {
  rememberMeasurementState(manufacturerData);
  handleStableMeasurement(manufacturerData);

  if (!DEBUG_BLE_PACKETS) {
    return;
  }

  const String dataHex = bytesToHex(manufacturerData);
  const bool isRepeat = dataHex == lastManufacturerDataHex;
  const uint32_t now = millis();

  if (isRepeat && now - lastRepeatedLogMillis < 2000) {
    return;
  }

  if (!isRepeat) {
    lastManufacturerDataHex = dataHex;
  }
  lastRepeatedLogMillis = now;

  Serial.printf("[%lu] rssi=%d", now, device.getRSSI());

  if (device.haveName()) {
    Serial.printf(" name=%s", device.getName().c_str());
  }

  Serial.printf(" data=%s", dataHex.c_str());

  // CM3-HM packets observed so far:
  // [0..1] manufacturer id, [2..7] device address reversed, [8..13] payload.
  Serial.printf(" payload=%s", bytesSliceToHex(manufacturerData, 8).c_str());

  if (isLikelyStableMeasurement(manufacturerData)) {
    Serial.print(" stable_candidate=yes");
  }

  if (isRepeat) {
    Serial.print(" repeat=yes");
  }

  Serial.println();
}

void printAdvertisedDevice(BLEAdvertisedDevice &device) {
  if (device.haveManufacturerData()) {
    printPacketSummary(device, device.getManufacturerData());
    return;
  }

  if (!DEBUG_BLE_PACKETS) {
    return;
  }

  static uint32_t lastNoManufacturerLogMillis = 0;
  const uint32_t now = millis();
  if (now - lastNoManufacturerLogMillis > 2000) {
    lastNoManufacturerLogMillis = now;
    Serial.printf("[%lu] rssi=%d name=%s manufacturer_data=(none)\n", now,
                  device.getRSSI(),
                  device.haveName() ? device.getName().c_str() : "(none)");
  }
}

class ScaleAdvertisedDeviceCallbacks final : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice device) override {
    if (!hasChocozapScaleSignal(device)) {
      return;
    }

    printAdvertisedDevice(device);
  }
};

void setupScanner() {
  BLEDevice::init("");

  scanner = BLEDevice::getScan();
  scanner->setAdvertisedDeviceCallbacks(new ScaleAdvertisedDeviceCallbacks(),
                                        true);

  // Passive scan is enough for non-connectable advertisements and avoids
  // changing the scale's behavior while we are reverse-engineering packets.
  scanner->setActiveScan(false);
  scanner->setInterval(SCAN_INTERVAL);
  scanner->setWindow(SCAN_WINDOW);
}

void connectWiFiIfNeeded() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  const uint32_t now = millis();
  if (lastWifiAttemptMillis != 0 &&
      now - lastWifiAttemptMillis < WIFI_RETRY_INTERVAL_MS) {
    return;
  }
  lastWifiAttemptMillis = now;

  Serial.printf("WiFi connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected, ip=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi connection failed");
  }
}

String buildDiscordJson(const Measurement &measurement) {
  String payload =
      "{\"content\":\"もっくんが体重を測りました！\\n体重: ";
  payload += String(measurement.weightKg, 1);
  payload += "kg\"}";
  return payload;
}

bool postJson(const char *label, const char *url, const String &json,
              String *responseBody = nullptr) {
  connectWiFiIfNeeded();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("%s skipped: WiFi is not connected\n", label);
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    Serial.printf("%s failed: could not begin HTTP request\n", label);
    return false;
  }

  if (DEBUG_HTTP) {
    Serial.printf("%s POST start: payload_bytes=%u\n", label,
                  static_cast<unsigned>(json.length()));
  }

  http.addHeader("Content-Type", "application/json");
  const int statusCode = http.POST(json);
  const String body = http.getString();
  if (DEBUG_HTTP) {
    Serial.printf("%s POST finished: status=%d\n", label, statusCode);
  }
  if (responseBody != nullptr) {
    *responseBody = body;
  }

  if (statusCode >= 200 && statusCode < 300) {
    if (DEBUG_HTTP) {
      Serial.printf("%s HTTP ok: status=%d response_bytes=%u\n", label,
                    statusCode, static_cast<unsigned>(body.length()));
    }
    http.end();
    return true;
  }

  Serial.printf("%s failed: status=%d body=%s\n", label, statusCode,
                body.c_str());
  http.end();
  return false;
}

String urlEncode(const String &value) {
  String encoded;
  encoded.reserve(value.length() * 3);

  for (size_t i = 0; i < value.length(); i++) {
    const char c = value[i];
    if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      encoded += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", static_cast<uint8_t>(c));
      encoded += buf;
    }
  }

  return encoded;
}

bool getUrl(const char *label, const String &url, String *responseBody = nullptr) {
  connectWiFiIfNeeded();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("%s skipped: WiFi is not connected\n", label);
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    Serial.printf("%s failed: could not begin HTTP GET\n", label);
    return false;
  }

  if (DEBUG_HTTP) {
    Serial.printf("%s GET start: url_bytes=%u\n", label,
                  static_cast<unsigned>(url.length()));
  }

  const int statusCode = http.GET();
  const String body = http.getString();
  if (DEBUG_HTTP) {
    Serial.printf("%s GET finished: status=%d response_bytes=%u\n", label,
                  statusCode, static_cast<unsigned>(body.length()));
  }

  if (responseBody != nullptr) {
    *responseBody = body;
  }

  if (statusCode >= 200 && statusCode < 300) {
    http.end();
    return true;
  }

  Serial.printf("%s failed: status=%d body=%s\n", label, statusCode,
                body.c_str());
  http.end();
  return false;
}

bool sendDiscordWebhook(const Measurement &measurement) {
  if (!discordConfigured()) {
    Serial.println("Discord skipped: src/secrets.h is not configured");
    return false;
  }

  if (!postJson("Discord", DISCORD_WEBHOOK_URL, buildDiscordJson(measurement))) {
    return false;
  }

  Serial.println("Discord sent");
  return true;
}

String buildSheetsJson(const Measurement &measurement) {
  String payload = "{\"token\":\"";
  payload += SHEETS_TOKEN;
  payload += "\",\"weightKg\":";
  payload += String(measurement.weightKg, 1);
  payload += ",\"rawBe\":";
  payload += String(measurement.rawBe);
  payload += ",\"stablePayload\":\"";
  payload += measurement.stablePayloadHex;
  payload += "\",\"livePayload\":\"";
  payload += measurement.livePayloadHex;
  payload += "\",\"device\":\"CM3-HM\"}";
  return payload;
}

String buildSheetsUrl(const Measurement &measurement) {
  String url = SHEETS_WEBHOOK_URL;
  url += "?token=";
  url += urlEncode(SHEETS_TOKEN);
  url += "&weightKg=";
  url += urlEncode(String(measurement.weightKg, 1));
  url += "&rawBe=";
  url += urlEncode(String(measurement.rawBe));
  url += "&stablePayload=";
  url += urlEncode(measurement.stablePayloadHex);
  url += "&livePayload=";
  url += urlEncode(measurement.livePayloadHex);
  url += "&device=CM3-HM";
  return url;
}

bool sendSheetsWebhook(const Measurement &measurement) {
  if (!sheetsConfigured()) {
    Serial.println("Sheets skipped: src/secrets.h is not configured");
    printSheetsConfigStatus();
    return false;
  }

  if (DEBUG_HTTP) {
    Serial.printf(
        "Sheets request: weight=%.1f raw=%u stable=\"%s\" live=\"%s\"\n",
        measurement.weightKg, measurement.rawBe,
        measurement.stablePayloadHex.c_str(),
        measurement.livePayloadHex.c_str());
  }

  String body;
  const String url = buildSheetsUrl(measurement);
  if (!getUrl("Sheets", url, &body)) {
    return false;
  }

  if (body.indexOf("\"ok\":true") < 0) {
    Serial.printf("Sheets failed: body=%s\n", body.c_str());
    return false;
  }

  const int rowIndex = body.indexOf("\"row\":");
  if (rowIndex >= 0) {
    const int rowStart = rowIndex + 6;
    const int rowEndComma = body.indexOf(',', rowStart);
    const int rowEndBrace = body.indexOf('}', rowStart);
    int rowEnd = body.length();
    if (rowEndComma >= 0) {
      rowEnd = rowEndComma;
    } else if (rowEndBrace >= 0) {
      rowEnd = rowEndBrace;
    }
    Serial.printf("Sheets sent: row=%s\n",
                  body.substring(rowStart, rowEnd).c_str());
  } else {
    Serial.println("Sheets sent");
  }

  if (DEBUG_HTTP) {
    Serial.printf("Sheets response: %s\n", body.c_str());
  }
  return true;
}

void processPendingMeasurement() {
  if (!pendingMeasurement.pending) {
    return;
  }

  Measurement measurement = pendingMeasurement;
  pendingMeasurement.pending = false;
  sendSheetsWebhook(measurement);
  sendDiscordWebhook(measurement);
}

}  // namespace

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1500);

  Serial.println();
  Serial.println("CM3-HM auto weight logger");
  Serial.println("Serial: 115200 baud");
  Serial.println("Step on the scale. Measurement summaries are printed.");
  if (DEBUG_BLE_PACKETS) {
    Serial.println(
        "BLE packet debug is enabled: data=<full manufacturer data>, "
        "payload=<last 6 bytes>");
  }
  Serial.println("Weight estimate is provisional and based on 3 samples.");
  printSheetsConfigStatus();
  if (discordConfigured() || sheetsConfigured()) {
    connectWiFiIfNeeded();
  } else {
    Serial.println("Discord disabled until src/secrets.h is configured.");
    Serial.println("Sheets disabled until src/secrets.h is configured.");
  }

  setupScanner();
}

void loop() {
  scanner->start(SCAN_SECONDS, false);
  scanner->clearResults();
  processPendingMeasurement();
  delay(500);
}
