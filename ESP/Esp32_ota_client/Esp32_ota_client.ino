#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

const char* wifi_ssid = "FTTH";
const char* wifi_pass = "12345678";

const char* server_url = "https://grha-iot-backend-test.onrender.com";

const char* device_id = "myesp32";
const char* current_version = "1.0.0";

#define LED_PIN 2

HTTPClient http;
String pendingFirmwareId = "";

void blinkLED(int times, int delayMs){
    for (int i =0; i< times; i++){
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
    while(WiFi.status() != WL_CONNECTED &&  retries< 30){
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

bool performChecking(){
    Serial.println("\n[OTA] --- step 1 device checkin");
    
    String myurl = String(server_url) +"/firmware/checkin";
    http.begin(myurl);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<512> doc;
    doc["device_id"] = device_id;
    doc["current_version"] = current_version;
    doc["chip"] = "esp32";
    doc["free_heap"] = ESP.getFreeHeap();
    // doc["max_heap"] = ESP.getHeapSize();
    // doc["wifi_rssi"] = WiFi.RSSI();

    String reqBody;
    serializeJson(doc, reqBody);

    Serial.printf("[HTTP] POST %s\n", myurl.c_str());
    Serial.printf("[HTTP] Body: %s\n", reqBody.c_str());

    int statusCode = http.POST(reqBody);
    Serial.printf("[HTTP] Response: %d\n", statusCode);

    if (statusCode != 200) {
        Serial.printf("[OTA] Check-in failed: HTTP %d\n", statusCode);
        http.end();
        return false;
    }

    String respBody = http.getString();
    Serial.printf("[HTTP] Body: %s\n", respBody.c_str());
    http.end();

    // Parse response
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
    Serial.printf("[OTA] Update available! Firmware ID: %s\n", pendingFirmwareId.c_str());
    return true;
}

bool downlaod_the_firmware(){
    Serial.println("\n[ota] step 2 - downloading firmware ");
    String url = String(server_url) + "/firmware/" + pendingFirmwareId;
    
    http.begin(url);
    http.addHeader("X-Device-ID", device_id);
    Serial.printf("[HTTP] GET %s\n", url.c_str());
    int statusCode = http.GET();

    if (statusCode != 200){
        Serial.printf("[OTA] Download failed HTTP %d\n", statusCode);
        http.end();
        return false;
    }

    String firmwareCode = http.getString();
     // Read response headers (metadata about the firmware)
    String fwVersion  = http.header("X-Firmware-Version");
    String fwSize     = http.header("X-Firmware-Size");
    String fwChecksum = http.header("X-Firmware-Checksum");

    Serial.println("[OTA] ── Firmware Metadata ──");
    Serial.printf("  Version:  %s\n", fwVersion.c_str());
    Serial.printf("  Size:     %s bytes\n", fwSize.c_str());
    Serial.printf("  Checksum: %s\n", fwChecksum.c_str());

    // Read the firmware payload
    // In real OTA: stream this into esp_ota_write() calls
    // Here: print to Serial to show what arrived
    int receivedBytes   = firmwareCode.length();
    http.end();

    Serial.println("\n[OTA] ── Received Firmware Code ──");
    Serial.println("─────────────────────────────────────");
    Serial.println(firmwareCode);
    Serial.println("─────────────────────────────────────");

    // Simulate flash write delay
    Serial.printf("[OTA] Simulating flash write (%d bytes)...\n", receivedBytes);
    for (int i = 0; i <= 100; i += 20) {
        Serial.printf("[OTA] Writing... %d%%\n", i);
        delay(300);
    }

    // In real OTA you would call:
    //   esp_ota_handle_t handle;
    //   esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &handle);
    //   esp_ota_write(handle, (const void*)chunk, chunkLen);
    //   esp_ota_end(handle);
    //   esp_ota_set_boot_partition(update_partition);
    //   esp_restart();

    Serial.println("[OTA] Flash write complete (simulated)");
    return true;
}

// ─── Step 3: Report OTA result back to server ─────────────────────────────────
// POST /device/ota-status
// Body: { device_id, status, new_version }

void reportOTAStatus(bool success, const String& newVersion) {
    Serial.println("\n[OTA] ── Step 3: Report OTA Status ──");

    String url = String(server_url) + "/device/ota-status";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<256> doc;
    doc["device_id"]   = device_id;
    doc["status"]      = success ? "success" : "failed";
    doc["version"]     = newVersion;

    String body;
    serializeJson(doc, body);

    Serial.printf("[HTTP] POST %s\n", url.c_str());
    Serial.printf("[HTTP] Body: %s\n", body.c_str());

    int statusCode = http.POST(body);
    Serial.printf("[HTTP] Response: %d\n", statusCode);
    if (statusCode == 200) {
        Serial.println("[OTA] Status reported successfully");
    }
    http.end();
}

// ─── Full OTA sequence ────────────────────────────────────────────────────────

void runOTAUpdate() {
    Serial.println("\n╔══════════════════════════════╗");
    Serial.println("║      OTA UPDATE STARTED      ║");
    Serial.println("╚══════════════════════════════╝");

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[OTA] WiFi not connected, aborting");
        return;
    }

    blinkLED(1, 500);

    // Step 1: Check-in
    bool updateNeeded = performChecking();
    if (!updateNeeded) {
        blinkLED(2, 200);
        return;
    }

    // Step 2: Download
    bool downloaded = downlaod_the_firmware();
    if (!downloaded) {
        Serial.println("[OTA] Download failed");
        reportOTAStatus(false, current_version);
        blinkLED(5, 100);
        return;
    }

    // Step 3: Report
    // In real OTA, version comes from the downloaded binary header
    // Here we hardcode the new version from the firmware_id
    reportOTAStatus(true, "2.0.0");

    Serial.println("\n[OTA] ✓ OTA Complete! In real system, esp_restart() would run here.");
    blinkLED(10, 80);
}

// ─── Arduino entry points ────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(1000);
    pinMode(LED_PIN, OUTPUT);

    Serial.println("\n[BOOT] ESP32 OTA Demo Client");
    Serial.printf("[BOOT] Device ID:      %s\n", device_id);
    Serial.printf("[BOOT] Current Version: %s\n", current_version);
    Serial.printf("[BOOT] Server:         %s\n", server_url);

    connectwifi();

    Serial.println("\n[READY] Type 'run' in Serial Monitor to trigger OTA check");
}

void loop() {
    // Wait for "run" command from Serial Monitor
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        cmd.toLowerCase();

        if (cmd == "run") {
            runOTAUpdate();
        } else if (cmd == "status") {
            Serial.printf("[STATUS] Device: %s  Version: %s  WiFi: %s\n",
                device_id,
                current_version,
                WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "DISCONNECTED"
            );
        } else if (cmd == "help") {
            Serial.println("[HELP] Commands: run | status | help");
        } else {
            Serial.printf("[CMD] Unknown command: '%s'  (try 'help')\n", cmd.c_str());
        }
    }
    delay(50);
}
