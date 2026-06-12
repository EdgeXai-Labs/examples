#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include "mbedtls/sha256.h"

const char* wifi_ssid = "FTTH";
const char* wifi_pass = "12345678";

// Make sure this points to your deployed backend URL on Render (or local IP for local testing)
const char* server_url = "https://examples-0sgv.onrender.com";

const char* device_id = "myesp32";
const char* current_version = "3.0.0"; // Version 3.0.0

#define LED_PIN 2
#define OTA_BUFFER_SIZE 4096

HTTPClient http;
String pendingFirmwareId = "";
String pendingFirmwareSHA = "";
int pendingFirmwareSize = 0;

void blinkLED(int times, int delayMs){
    for (int i = 0; i < times; i++){
        digitalWrite(LED_PIN, HIGH);
        delay(delayMs);
        digitalWrite(LED_PIN, LOW);
        delay(delayMs);
    }
}

void connectwifi(){
    Serial.print("Connecting to " + String(wifi_ssid));
    WiFi.begin(wifi_ssid, wifi_pass);
    int retries = 0;
    while(WiFi.status() != WL_CONNECTED && retries < 30){
        delay(500);
        Serial.print(".");
        retries++;
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED){
        Serial.println("Connected to Wifi, IP " + WiFi.localIP().toString());
    }else{
        Serial.println("Failed to connect");
    }
}

bool performCheckin(){
    Serial.println("\n[OTA] --- step 1 device checkin");
    
    String myurl = String(server_url) + "/firmware/checkin";
    http.begin(myurl);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<512> doc;
    doc["device_id"] = device_id;
    doc["current_version"] = current_version;
    doc["chip"] = "esp32";
    doc["free_heap"] = ESP.getFreeHeap();

    String reqBody;
    serializeJson(doc, reqBody);

    int code = http.POST(reqBody);
    if(code != 200){
        Serial.printf("[OTA] Check-in failed: HTTP %d\n", code);
        http.end();
        return false;
    }
    String respBody = http.getString();
    Serial.printf("[HTTP] Body: %s\n", respBody.c_str());
    http.end();

    StaticJsonDocument<512> resDoc;
    DeserializationError err = deserializeJson(resDoc, respBody);
    if (err) {
        Serial.println("[OTA] JSON parse error");
        return false;
    }

    bool updateAvailable = resDoc["update_available"];
    const char* message  = resDoc["message"];
    Serial.printf("[OTA] Server says: %s\n", message);

    if (!updateAvailable) {
        Serial.println("[OTA] Device is up to date. No action needed.");
        return false;
    }

    pendingFirmwareId = resDoc["firmware_id"].as<String>();
    pendingFirmwareSHA = resDoc["sha256"].as<String>();
    pendingFirmwareSize = resDoc["size_bytes"].as<int>();
    
    Serial.printf("[OTA] Update available! Firmware ID: %s\n", pendingFirmwareId.c_str());
    return true;
}

String bytesToHex(const uint8_t* bytes, size_t len) {
    String result = "";
    result.reserve(len * 2);
    char buf[3];
    for (size_t i = 0; i < len; i++) {
        snprintf(buf, sizeof(buf), "%02x", bytes[i]);
        result += buf;
    }
    return result;
}

bool download_and_flash_firmware(){
    Serial.println("\n[OTA] --- Step 2: Download & Flash Firmware");
    String url = String(server_url) + "/firmware/" + pendingFirmwareId;
    
    const char* headerKeys[] = {"X-Firmware-Version", "X-Firmware-Size", "X-Firmware-SHA256"};
    http.collectHeaders(headerKeys, 3);

    http.begin(url);
    http.addHeader("X-Device-ID", device_id);
    Serial.printf("[HTTP] GET %s\n", url.c_str());
    int statusCode = http.GET();

    if (statusCode != 200){
        Serial.printf("[OTA] Download failed HTTP %d\n", statusCode);
        http.end();
        return false;
    }

    String headerSHA  = http.header("X-Firmware-SHA256");
    String headerSize = http.header("X-Firmware-Size");
    int    totalBytes = http.getSize();

    Serial.println("[OTA] ── Response Headers ──");
    Serial.printf("  SHA256:  %s\n", headerSHA.c_str());
    Serial.printf("  Size:    %s bytes\n", headerSize.c_str());
    Serial.printf("  Content-Length: %d\n", totalBytes);

    if (totalBytes <= 0){
        Serial.println("[OTA] Error: Server did not send Content-Length");
        http.end();
        return false;
    }

    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL){
        Serial.println("[OTA] Error: Could not find update partition");
        http.end();
        return false;
    }

    Serial.printf("[OTA] Update partition: %s\n", update_partition->label);
    Serial.printf("[OTA] Update partition address: %d\n", update_partition->address);
    Serial.printf("[OTA] Update partition size: %d\n", update_partition->size);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        Serial.printf("[OTA] Error: Failed to begin OTA update: %d\n", err);
        http.end();
        return false;
    }
    Serial.println("[OTA] Partition erased for writing firmware OTA handle opened....");

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts_ret(&sha_ctx, 0);

    uint8_t buffer[OTA_BUFFER_SIZE];
    int bytesReceived = 0;
    int bytesRead = 0;
    bool streamError = false;

    WiFiClient* stream = http.getStreamPtr();
    Serial.println("[OTA] Starting to write firmware chunk by chunk...");
    Serial.println("--------------------------------------------------");

    while (bytesReceived < totalBytes){
        int remainingBytes = totalBytes - bytesReceived;
        int toRead = min(remainingBytes, OTA_BUFFER_SIZE);

        int waited = 0;
        while (stream->available() < toRead && waited < 5000){
            delay(1);
            waited++;
        }
        bytesRead = stream->readBytes(buffer, toRead);
        if (bytesRead <= 0){
            Serial.println("[OTA] Stream read returned 0. Network error.");
            streamError = true;
            break;
        }

        err = esp_ota_write(ota_handle, (const void*)buffer, bytesRead);
        if (err != ESP_OK) {
            Serial.printf("[OTA] esp_ota_write failed: %s\n", esp_err_to_name(err));
            streamError = true;
            break;
        }
    
        mbedtls_sha256_update_ret(&sha_ctx, buffer, bytesRead);
        bytesReceived += bytesRead;
    
        if (bytesReceived % 65536 == 0 || bytesReceived == totalBytes){
            Serial.printf("[OTA] Progress: %d / %d bytes (%.1f%%)\n",
                              bytesReceived, totalBytes,
                              (float)bytesReceived / totalBytes * 100.0f);
        }
    }

    http.end();

    if (streamError){
        esp_ota_abort(ota_handle);
        Serial.println("[OTA] Aborted due to stream error");
        mbedtls_sha256_free(&sha_ctx);
        return false;
    }

    if (bytesReceived != totalBytes){
        Serial.printf("[OTA] Size mismatch: got %d, expected %d. Aborting.\n",
                      bytesReceived, totalBytes);
        esp_ota_abort(ota_handle);
        mbedtls_sha256_free(&sha_ctx);
        return false;
    }

    uint8_t hash[32];
    mbedtls_sha256_finish_ret(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);

    String computedSHA = bytesToHex(hash, 32);
    Serial.printf("[OTA] Computed SHA256:  %s\n", computedSHA.c_str());
    Serial.printf("[OTA] Expected SHA256:  %s\n", headerSHA.c_str());

    if (computedSHA != headerSHA || computedSHA != pendingFirmwareSHA) {
        Serial.println("[OTA] ❌ SHA-256 MISMATCH. Firmware is corrupt or tampered.");
        esp_ota_abort(ota_handle);
        return false;
    }
    Serial.println("[OTA] ✅ SHA-256 verified. Firmware is authentic.");
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        Serial.printf("[OTA] esp_ota_end failed: %s (bad binary header?)\n", esp_err_to_name(err));
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        Serial.printf("[OTA] esp_ota_set_boot_partition failed: %s\n", esp_err_to_name(err));
        return false;
    }

    Serial.println("[OTA] ✅ Boot partition updated. Ready to restart.");
    return true;
}

void reportOTAStatus(bool success, const String& version) {
    Serial.println("\n[OTA] --- Step 3: Reporting Status");
    String url = String(server_url) + "/device/ota-status";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<256> doc;
    doc["device_id"] = device_id;
    doc["status"]    = success ? "success" : "failed";
    doc["version"]   = version;

    String body;
    serializeJson(doc, body);
    int code = http.POST(body);
    Serial.printf("[OTA] Status report: HTTP %d\n", code);
    http.end();
}

void runOTAUpdate() {
    Serial.println("\n╔══════════════════════════════╗");
    Serial.println("║   PRODUCTION OTA STARTED     ║");
    Serial.println("╚══════════════════════════════╝");
    Serial.printf("[HEAP] Free before OTA: %d bytes\n", ESP.getFreeHeap());

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[OTA] WiFi not connected. Aborting."); return;
    }

    blinkLED(1, 500);

    if (!performCheckin()) {
        blinkLED(2, 200); return;
    }

    bool success = download_and_flash_firmware();
    reportOTAStatus(success, success ? pendingFirmwareSHA.substring(0, 8) : current_version);

    if (success) {
        Serial.println("\n[OTA] ✅ OTA Complete! Restarting in 3 seconds...");
        blinkLED(10, 80);
        delay(3000);
        esp_restart();
    } else {
        Serial.println("[OTA] ❌ OTA Failed. Current firmware unchanged.");
        blinkLED(5, 100);
    }
}

unsigned long lastBlinkTime = 0;
unsigned long lastSensorTime = 0;
bool ledState = false;

void setup() {
    Serial.begin(115200);
    delay(1000);
    pinMode(LED_PIN, OUTPUT);

    // Get current boot partition info
    const esp_partition_t* running_partition = esp_ota_get_running_partition();

    Serial.println("\n[BOOT] ========================================");
    Serial.println("[BOOT] ESP32 Production OTA Client: Version 3.0.0");
    Serial.printf("[BOOT] Device ID:  %s\n", device_id);
    Serial.printf("[BOOT] Running on Partition: %s (Address: 0x%08X)\n", 
                  running_partition ? running_partition->label : "Unknown",
                  running_partition ? running_partition->address : 0);
    Serial.printf("[BOOT] Free Heap: %d bytes\n", ESP.getFreeHeap());
    Serial.println("[BOOT] Behaviour: Sensor monitor mode - Mock reading GPIO 36");
    Serial.println("[BOOT] ========================================");

    connectwifi();
    Serial.println("\n[READY] Commands: run | status | help");
}

void loop() {
    // Check for Serial commands
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        cmd.toLowerCase();

        if (cmd == "run") {
            runOTAUpdate();
        } else if (cmd == "status") {
            const esp_partition_t* run_part = esp_ota_get_running_partition();
            Serial.printf("[STATUS] Version=%s  Partition=%s  Heap=%d  WiFi=%s\n",
                current_version, run_part ? run_part->label : "Unknown", ESP.getFreeHeap(),
                WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "DISCONNECTED");
        } else if (cmd == "help") {
            Serial.println("[HELP] run | status | help");
        } else {
            Serial.printf("[CMD] Unknown: '%s'\n", cmd.c_str());
        }
    }

    // Version 3.0.0 behavior: Medium blink (every 1000 ms)
    if (millis() - lastBlinkTime >= 1000) {
        lastBlinkTime = millis();
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    }

    // Sensor monitor print: every 4 seconds
    if (millis() - lastSensorTime >= 4000) {
        lastSensorTime = millis();
        int sensorVal = analogRead(36); // GPIO 36 (ADC1_CH0 / VP)
        Serial.printf("[v3.0.0] Sensor Reading: GPIO36_ADC = %d\n", sensorVal);
    }

    delay(50);
}
