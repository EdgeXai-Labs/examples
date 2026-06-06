"""
IoT OTA Update Backend - FastAPI
Simulates real OTA firmware update server for ESP32 microcontrollers.
"""
 
import hashlib
from fastapi import FastAPI, HTTPException, Header, Request
from fastapi.middleware.cors import CORSMiddleware
import logging, time, json
from fastapi.responses import StreamingResponse, JSONResponse, PlainTextResponse
from pydantic import BaseModel
from typing import Optional
import time, hashlib, logging

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

app = FastAPI(title="IoT OTA Update Server", version="1.0.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
    expose_headers=["X-Firmware-Version", "X-Firmware-Size"],
)

firmware_catalog = {
    "Grha-v1" :{
        "version":"1.0.0",
        "description": "Basic LED blink every 3 second",
        "target": "esp32",
        "size_bytes" :312,
        "code":"""\
            // Firmware for Grha-v1 
            // Deployed via OTA from IOT platform

            #include <Arduino.h>

            #define LED_PIN 2

            void setup(){
            Serial.begin(115200);
            pinMode(LED_PIN, OUTPUT);
    Serial.println("[OTA] blink_v1 booted successfully");
}
 
void loop() {
    digitalWrite(LED_PIN, HIGH);
    delay(1000);
    digitalWrite(LED_PIN, LOW);
    delay(1000);
}
        """,
    },
    "Grha-v2" :{
        "version":"2.0.0",
        "description": "Fast blink 200ms send Temp and Humidity to cloud",
        "target": "esp32",
        "size_bytes" :428,
        "code":"""\
            // Firmware for Grha-v2 
            // Deployed via OTA from IOT platform
            #include <Arduino.h>
 
            #define LED_PIN 2
            unsigned long lastHeartbeat = 0;
 
            void setup() {
                Serial.begin(115200);
                pinMode(LED_PIN, OUTPUT);
                Serial.println("[OTA] blink_v2 booted — fast blink mode");
            }
 
            void loop() {
                digitalWrite(LED_PIN, HIGH);
                delay(200);
                digitalWrite(LED_PIN, LOW);
                delay(200);
 
                if (millis() - lastHeartbeat > 5000) {
                    Serial.printf("[HEARTBEAT] uptime=%lus\\n", millis() / 1000);
                    lastHeartbeat = millis();
                }
            }
        """,
    },
    
"sensor_v1": {
        "version": "1.0.0",
        "description": "Reads analog pin A0 and reports over Serial",
        "target": "esp32",
        "size_bytes": 541,
        "code": """\
// Firmware: sensor_v1  |  Version: 1.0.0
// Deployed via OTA from IoT Platform
 
#include <Arduino.h>
 
#define SENSOR_PIN 34   // ESP32 ADC1 channel 6
#define SAMPLE_INTERVAL_MS 2000
 
unsigned long lastSample = 0;
 
void setup() {
    Serial.begin(115200);
    analogReadResolution(12);
    Serial.println("[OTA] sensor_v1 booted — ADC sampling mode");
}
 
void loop() {
    if (millis() - lastSample >= SAMPLE_INTERVAL_MS) {
        int raw = analogRead(SENSOR_PIN);
        float voltage = (raw / 4095.0f) * 3.3f;
        Serial.printf("[SENSOR] raw=%d  voltage=%.3fV\\n", raw, voltage);
        lastSample = millis();
    }
}
""",
    },
}

# SImulate the device registry { device_id -> {curernt version and last seen }}
device_registry: dict ={}

# Validation BasemOdels

class DeviceCheckIn(BaseModel):
    device_id: str
    current_version: str
    chip: Optional[str] = "esp32"
    free_heap: Optional[int] = None

class OTAStatus(BaseModel):
    device_id : str
    version : str
    status: str   #sucess or failure


# Helerpe functions 

def comute_checsum(data: str)-> str:
    return  hashlib.md5(data.encode("utf-8")).hexdigest()

def get_latest_firmware():
    """Return the latest firmware  based on version string. (For simulation)"""
    return list(firmware_catalog.keys())[-1]


@app.get("/firmware/list")
def list_firmwares():
    """List of all avialabel firmwre in the catlog"""
    return {
        fid:{
             "version":f["version"],
        "description":f["description"],
        "target":f["target"],
        "size_bytes" :f["size_bytes"],
        }
        for fid, f in firmware_catalog.items()
    }

@app.get("/")
def root():
    return {"message": "IoT OTA Update Server is Running", "catalog": firmware_catalog.keys()}


@app.post("/firmware/checkin")
def device_checkin(payload: DeviceCheckIn, request: Request):
    """Simulates the device 
     Device hits this on boot or periodically.
    Returns: whether an update is available + which firmware_id to fetch.
    
    CORS NOTE: ESP32 never enforces CORS — it's a raw HTTP client.
    Only browsers enforce it. So this endpoint works from ESP32 regardless.
    """

    logger.info(f"[CHEIKIN] device = {payload.device_id} | version = {payload.current_version}")

    # Update last seen timestamp
    device_registry[payload.device_id] = {
        "version": payload.current_version,
        "chip":payload.chip,
        "last_seen": time.time(),
        "ip": request.client.host}

    latest_id = get_latest_firmware()
    latest = firmware_catalog[latest_id]
    update_availbel = payload.current_version != latest["version"]

    return {
        "device_id": payload.device_id,
        "update_available": update_availbel,
        "current_version":payload.current_version,
        "latest_version":latest["version"],
        "firmware_id": latest_id if update_availbel else None,
        "message": f"Update to {latest['version']} available" if update_availbel else "Device is up to date",
    }

@app.get("/firmware/{firmware_id}")
def get_firmware(firmware_id: str, x_device : Optional[str] = Header(default=None)):

    """
    Returns the firmware code as plain text.
    
    In a real system this would stream a binary .bin file.
    Here we return C++ source as text so you can read/log it on the ESP32.
    
    The ESP32 sends X-Device-ID header so we can log who downloaded what.
    """
    
    if  firmware_id not in firmware_catalog:
        raise HTTPException(status_code=404 , detail="Firmware not found")

    fw = firmware_catalog[firmware_id]
    code = fw["code"]

    checksum = comute_checsum(code)
    
    logger.info(f"[FIRMWARE_DOWNLOADED] device ={x_device} |  firmware= {firmware_id}")
    
    from fastapi.responses import Response

    return Response(
        content=code,
        media_type="text/plain",
        headers={
            "X-Firmware-Version": fw["version"],
            "X-Firmware-Size": str(fw["size_bytes"]),
            "X-Firmware-Checksum": checksum,
            "X-Firmware-Description": fw["description"],
        }
    )

@app.post("/device/ota-status")
def resport_ota_status(paylaod: OTAStatus):
    """
    Device calls this after OTA attempt to report success/failure.
    In production: write to time-series DB, trigger alert if failed.
    """

    logger.info(f"[OTA_STATUS] device={paylaod.device_id} | version={paylaod.version} | status={paylaod.status}")
    
    if paylaod.device_id in device_registry:
        if paylaod.status == "success":
            device_registry[paylaod.device_id]["version"] = paylaod.version
            logger.info(f"[FIRMWARE_UPDATED] device ={paylaod.device_id} | status {paylaod.status} | new version = {paylaod.version}")
        else:
            logger.error(f"[OTA_FAILED] device ={paylaod.device_id} | status {paylaod.status} | new version = {paylaod.version}")
            # Here you would trigger an alert

    return {"acknowledged": True, "message": "OTA status received", "device_id": paylaod.device_id}

@app.get("/devices")
def list_devices():
    """Dashboard endpoint — see all connected devices and their versions."""
    now = time.time()
    return {
        did: {
            **info,
            "online": (now - info["last_seen"]) < 60,  # seen in last 60s
        }
        for did, info in device_registry.items()
    }
 
 
# ─── Request logging middleware (shows CORS in action) ───────────────────────
@app.middleware("http")
async def log_requests(request: Request, call_next):
    origin = request.headers.get("origin", "no-origin (embedded/curl)")
    logger.info(f"[REQ] {request.method} {request.url.path}  origin={origin}")
    response = await call_next(request)
    logger.info(f"[RES] {response.status_code}  cors-origin={response.headers.get('access-control-allow-origin', 'not-set')}")
    return response