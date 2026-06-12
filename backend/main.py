"""
IoT OTA Update Backend - FastAPI
Simulates real OTA firmware update server for ESP32 microcontrollers.
"""
 
import hashlib, os
from pathlib import Path
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
    expose_headers=["X-Firmware-Version", "X-Firmware-Size", "X-Firmware-SHA256"],
)

myfirmware_dir = Path(__file__).parent /"firmware_sources/bin_files"

firmware_catalog = {
    "Grha-v1" :{
        "version":"1.0.0",
        "description": "Basic LED blink every 3 second",
        "target": "esp32",
        "bin_file": "v1_0_0.ino.bin",
    },
#     "Grha-v2" :{
#         "version":"2.0.0",
#         "description": "Fast blink 200ms send Temp and Humidity to cloud",
#         "target": "esp32",
#         "bin_file": "Grha-v2.bin",
#     },
# "sensor_v1": {
#         "version": "3.0.0",
#         "description": "Reads analog pin A0 and reports over Serial",
#         "target": "esp32",
#         "bin_file": "sensor_v1.bin",
# }
}

# ─── Helper: compute SHA-256 of a file ────────────────────────────────────────
# WHY SHA-256 not MD5?
#   MD5 is 128-bit and has known collision attacks.
#   SHA-256 is 256-bit — practically unbreakable.
#   If an attacker intercepts and modifies your .bin, the hash won't match
#   and the ESP32 will reject it before calling set_boot_partition().
def compute_sha256_file(filepath: Path) -> str:
    sha256 = hashlib.sha256()
    # Read in 64KB chunks — the file could be 2MB+, no need to load all at once
    with open(filepath, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            sha256.update(chunk)        # feed each chunk into the hasher
    return sha256.hexdigest()           # returns 64-char hex string



# SImulate the device registry { device_id -> {curernt version and last seen }}

for fid, fw in firmware_catalog.items():
    bin_path = myfirmware_dir / fw["bin_file"]
    if bin_path.exists():
        fw["size_bytes"] = bin_path.stat().st_size   # actual file size in bytes
        fw["sha256"] = compute_sha256_file(bin_path)
        logger.info(f"[CATALOG] {fid}  size={fw['size_bytes']}  sha256={fw['sha256'][:16]}...")
    else:
        # File missing — mark it so the endpoint can return 503 cleanly
        fw["size_bytes"] = 0
        fw["sha256"]     = ""
        logger.warning(f"[CATALOG] WARNING: {bin_path} NOT FOUND — endpoint will return 503")



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

# def get_latest_firmware():
#     """Return the latest firmware  based on version string. (For simulation)"""
#     return list(firmware_catalog.keys())[-1]

def get_latest_firmware(current_version: str):
    """Return the next firmware in the cycle for sequential OTA testing."""
    if current_version == "1.0.0":
        return "Grha-v2"
    elif current_version == "2.0.0":
        return "sensor_v1"
    elif current_version == "3.0.0":
        return "Grha-v1"
    else:
        return "Grha-v1"



@app.get("/firmware/list")
def list_firmwares():
    return {
        fid: {
            "version":     f["version"],
            "description": f["description"],
            "target":      f["target"],
            "size_bytes":  f["size_bytes"],
            "sha256":      f["sha256"],
        }
        for fid, f in firmware_catalog.items()
    }

@app.get("/")
def root():
    return {"message": "IoT OTA Update Server is Running", "catalog": list(firmware_catalog.keys())}


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

    latest_id = get_latest_firmware(payload.current_version)
    latest = firmware_catalog[latest_id]
    update_availbel = payload.current_version != latest["version"]

    return {
        "device_id": payload.device_id,
        "update_available": update_availbel,
        "current_version":payload.current_version,
        "latest_version":latest["version"],
        "firmware_id": latest_id if update_availbel else None,
        "message": f"Update to {latest['version']} available" if update_availbel else "Device is up to date",
        "sha256": latest["sha256"] if update_availbel else None,
        "size_bytes":latest["size_bytes"] if update_availbel else None,
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

    bin_path = myfirmware_dir / fw["bin_file"]
    if not bin_path.exists():
        raise HTTPException(status_code=404 , detail="Firmware not found on server disk")

    logger.info(f"[DOWNLOAD] device={x_device} | firmware={firmware_id} | size={fw['size_bytes']}")

    def binary_file_generator():
        with open(bin_path, "rb") as f:
            while True:
                chunk = f.read(4096)
                if not chunk:
                    break
                yield chunk
    return  StreamingResponse(
        content = binary_file_generator(),
        media_type="application/octet-stream",
        headers={
            "X-Firmware-Version": fw["version"],
            "X-Firmware-Size": str(fw["size_bytes"]),
            "X-Firmware-Checksum": fw["sha256"],
            "X-Firmware-Description": fw["description"],
        }
    )

    

    # checksum = comute_checsum(code)
    
    # logger.info(f"[FIRMWARE_DOWNLOADED] device ={x_device} |  firmware= {firmware_id}")
    
    # from fastapi.responses import Response

    # return Response(
    #     content=code,
    #     media_type="text/plain",
    #     headers={
    #         "X-Firmware-Version": fw["version"],
    #         "X-Firmware-Size": str(fw["size_bytes"]),
    #         "X-Firmware-Checksum": checksum,
    #         "X-Firmware-Description": fw["description"],
    #     }
    # )

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