"""
Signal Logger — Advanced SIGINT Platform
FastAPI Backend  v2.0
─────────────────────────────────────────
New in v2.0:
  • WebSocket /ws  — real-time push every second (no polling needed)
  • /api/stats     — comprehensive statistics
  • /api/alerts    — new_contact / lost_contact / emergency events
  • /api/history/{icao} — per-aircraft position trail
  • /api/military  — filtered military contacts only
  • /api/search    — query by callsign, ICAO prefix, altitude range
  • /api/export/geojson — GeoJSON export for external GIS tools
  • Auto CSV rotation (> 100 MB renames to .bak)
  • Contact lifecycle tracking (first_seen, total_messages)
"""
from __future__ import annotations

import asyncio
import csv
import json
import math
import os
import shutil
import time
from collections import deque
from datetime import datetime
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, Query, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse

app = FastAPI(title="Signal Logger SIGINT API", version="2.0")
app.add_middleware(CORSMiddleware, allow_origins=["*"],
                   allow_methods=["*"], allow_headers=["*"])

# ── Config ───────────────────────────────────────────────────────────────────
DATA_LAKE_PATH  = "../../data_lake.csv"
SIGNAL_TIMEOUT  = 90      # seconds
MAX_HISTORY     = 500     # positions per aircraft
MAX_ALERTS      = 500     # alert queue depth
CSV_ROTATE_MB   = 100

START_TIME      = time.time()

# ── In-memory state ───────────────────────────────────────────────────────────
# contact_store: {icao → {first_seen_ts, last_seen_ts, total_msgs,
#                          positions: deque[(lat,lon,ts,alt)], ...}}
contact_store: dict[str, dict] = {}

# alert_queue: [{type, icao, ts, detail}]
alert_queue: deque = deque(maxlen=MAX_ALERTS)

# known ICAOs from last read (for new-contact detection)
_prev_icaos: set[str] = set()

# WebSocket clients
_ws_clients: list[WebSocket] = []

# ── Military ICAO ranges ──────────────────────────────────────────────────────
MIL_RANGES = [
    (0xAE0000, 0xAEFFFF, "US DoD"),
    (0x43C000, 0x43CFFF, "UK RAF"),
    (0x3A4000, 0x3A4FFF, "French AF"),
    (0x686000, 0x6863FF, "Turkish THK"),
    (0x350000, 0x37FFFF, "Russian"),
    (0x140000, 0x157FFF, "Chinese PLA"),
    (0x710000, 0x71FFFF, "Japan JASDF"),
]

def is_military(icao: str) -> str:
    """Return country string if military, '' otherwise."""
    try:
        h = int(icao, 16)
        for lo, hi, label in MIL_RANGES:
            if lo <= h <= hi:
                return label
    except Exception:
        pass
    return ""

# ── Helpers ───────────────────────────────────────────────────────────────────
def safe_float(v, d=0.0):
    try:
        x = float(v)
        return d if (math.isnan(x) or math.isinf(x)) else x
    except Exception:
        return d

def safe_int(v, d=0):
    try:
        return int(float(v))
    except Exception:
        return d

def parse_ts(ts_str: str) -> Optional[datetime]:
    try:
        return datetime.fromisoformat(ts_str[:19])
    except Exception:
        return None

def ts_now() -> str:
    return datetime.now().isoformat(timespec="seconds")

# ── CSV rotation ──────────────────────────────────────────────────────────────
def maybe_rotate_csv():
    p = Path(DATA_LAKE_PATH)
    if not p.exists():
        return
    size_mb = p.stat().st_size / (1024 * 1024)
    if size_mb >= CSV_ROTATE_MB:
        bak = str(p) + ".bak"
        shutil.move(str(p), bak)
        # re-create with header
        with open(DATA_LAKE_PATH, "w", newline="", encoding="utf-8") as f:
            f.write("Timestamp,Protocol,Identifier,Callsign,Squawk,Category,"
                    "Altitude,Speed,Heading,VertRate,"
                    "Latitude,Longitude,Frequency,SNR_dB,Emergency\n")

# ── Core flight reader ────────────────────────────────────────────────────────
def read_flights() -> dict:
    """Read CSV → build {icao: flight_dict}, apply timeout filter."""
    if not os.path.exists(DATA_LAKE_PATH):
        return {}

    flights: dict[str, dict] = {}
    try:
        with open(DATA_LAKE_PATH, "r", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            for row in reader:
                icao = row.get("Identifier", "").strip()
                if not icao:
                    continue
                lat  = safe_float(row.get("Latitude",  "0"))
                lon  = safe_float(row.get("Longitude", "0"))
                prev = flights.get(icao, {})

                if lat == 0.0 and lon == 0.0:
                    lat = prev.get("lat", 0.0)
                    lon = prev.get("lon", 0.0)

                alt      = safe_int(row.get("Altitude", "0")) or prev.get("altitude", 0)
                speed    = safe_float(row.get("Speed",   "0")) or prev.get("speed",   0)
                heading  = safe_float(row.get("Heading", "0")) or prev.get("heading", 0)
                vrate    = safe_int(row.get("VertRate",  "0"))
                callsign = (row.get("Callsign", "").strip()
                            or prev.get("callsign", ""))
                squawk   = row.get("Squawk",    "").strip()
                category = row.get("Category",  "").strip()
                snr      = safe_float(row.get("SNR_dB", "0"))
                emergency= row.get("Emergency", "").strip()
                ts_str   = row.get("Timestamp", "")
                protocol = row.get("Protocol",  "ADS-B")
                freq     = row.get("Frequency", "1090")

                flights[icao] = {
                    "icao":      icao,
                    "callsign":  callsign or icao,
                    "squawk":    squawk,
                    "category":  category,
                    "altitude":  alt,
                    "speed":     round(speed, 1),
                    "heading":   round(heading, 1),
                    "vert_rate": vrate,
                    "lat":       lat,
                    "lon":       lon,
                    "timestamp": ts_str,
                    "protocol":  protocol,
                    "frequency": freq,
                    "snr_db":    round(snr, 1),
                    "emergency": emergency,
                }
    except Exception as e:
        return {}

    # ── Signal timeout filter ───────────────────────────────────────────────
    now_local = datetime.now()
    fresh: dict[str, dict] = {}
    for icao, f in flights.items():
        ts = parse_ts(f["timestamp"])
        if ts:
            age_s = (now_local - ts).total_seconds()
            if age_s <= SIGNAL_TIMEOUT:
                f["age_seconds"] = int(age_s)
                fresh[icao] = f
        else:
            f["age_seconds"] = -1
            fresh[icao] = f

    return fresh

# ── Contact lifecycle + alert generation ──────────────────────────────────────
def update_contact_store(fresh: dict):
    global _prev_icaos
    now_ts = ts_now()
    curr_icaos = set(fresh.keys())

    # New contacts
    for icao in curr_icaos - _prev_icaos:
        f = fresh[icao]
        mil = is_military(icao)
        detail = {"protocol": f["protocol"], "callsign": f["callsign"]}
        if mil:
            detail["military_unit"] = mil
        alert_queue.append({
            "type": "new_military" if mil else "new_contact",
            "icao": icao,
            "ts":   now_ts,
            "detail": detail,
        })
        contact_store[icao] = {
            "first_seen": now_ts,
            "last_seen":  now_ts,
            "total_msgs": 0,
            "positions":  deque(maxlen=MAX_HISTORY),
            "protocol":   f["protocol"],
            "military":   mil,
        }

    # Lost contacts
    for icao in _prev_icaos - curr_icaos:
        if icao in contact_store:
            contact_store[icao]["last_seen"] = now_ts
        alert_queue.append({
            "type":   "lost_contact",
            "icao":   icao,
            "ts":     now_ts,
            "detail": {},
        })

    # Update existing
    for icao, f in fresh.items():
        cs = contact_store.setdefault(icao, {
            "first_seen": now_ts, "last_seen": now_ts,
            "total_msgs": 0, "positions": deque(maxlen=MAX_HISTORY),
            "protocol": f["protocol"], "military": is_military(icao),
        })
        cs["last_seen"]  = now_ts
        cs["total_msgs"] += 1
        cs["protocol"]   = f["protocol"]

        # Position trail update
        if f["lat"] != 0.0 and f["lon"] != 0.0:
            trail = cs["positions"]
            if not trail or trail[-1][:2] != (f["lat"], f["lon"]):
                trail.append((f["lat"], f["lon"], f["timestamp"], f["altitude"]))

        # Emergency alert
        if f.get("emergency") and f["emergency"] not in ("", "None"):
            alert_queue.append({
                "type":   "emergency",
                "icao":   icao,
                "ts":     now_ts,
                "detail": {"status": f["emergency"], "callsign": f["callsign"]},
            })

    _prev_icaos = curr_icaos


# ── WebSocket broadcaster ─────────────────────────────────────────────────────
async def broadcast_loop():
    """Background task: push fresh data to all WS clients every second."""
    while True:
        await asyncio.sleep(1)
        if not _ws_clients:
            continue
        fresh = read_flights()
        update_contact_store(fresh)
        payload = json.dumps({
            "type":    "update",
            "flights": list(fresh.values()),
            "count":   len(fresh),
            "ts":      ts_now(),
        })
        dead = []
        for ws in _ws_clients:
            try:
                await ws.send_text(payload)
            except Exception:
                dead.append(ws)
        for d in dead:
            _ws_clients.remove(d)

@app.on_event("startup")
async def startup():
    asyncio.create_task(broadcast_loop())


# ═════════════════════════════════════════════════════════════════════════════
# ENDPOINTS
# ═════════════════════════════════════════════════════════════════════════════

@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await ws.accept()
    _ws_clients.append(ws)
    try:
        while True:
            await ws.receive_text()  # keep-alive ping handler
    except WebSocketDisconnect:
        pass
    finally:
        if ws in _ws_clients:
            _ws_clients.remove(ws)


@app.get("/api/flights")
def get_flights():
    maybe_rotate_csv()
    fresh = read_flights()
    update_contact_store(fresh)

    # Merge lifecycle info
    result = []
    for icao, f in fresh.items():
        cs = contact_store.get(icao, {})
        f["first_seen"]   = cs.get("first_seen", "")
        f["total_msgs"]   = cs.get("total_msgs",  0)
        f["military_unit"]= cs.get("military",    "")
        result.append(f)

    return {"status": "ok", "count": len(result), "flights": result}


@app.get("/api/status")
def get_status():
    uptime_s = int(time.time() - START_TIME)
    h, rem = divmod(uptime_s, 3600); m, s = divmod(rem, 60)
    line_count = 0
    if os.path.exists(DATA_LAKE_PATH):
        try:
            with open(DATA_LAKE_PATH) as f:
                line_count = max(0, sum(1 for _ in f) - 1)
        except Exception:
            pass
    size_mb = 0.0
    if os.path.exists(DATA_LAKE_PATH):
        size_mb = round(os.path.getsize(DATA_LAKE_PATH) / (1024*1024), 2)

    return {
        "hardware":       "RTL-SDR",
        "dsp_engine":     "C++ Native (CRC24 validated)",
        "decoder_core":   "ADS-B DF17/18/11 + AIS",
        "packets_total":  line_count,
        "csv_size_mb":    size_mb,
        "uptime":         f"{h:02d}:{m:02d}:{s:02d}",
        "signal_timeout": SIGNAL_TIMEOUT,
        "ws_clients":     len(_ws_clients),
        "contacts_seen":  len(contact_store),
    }


@app.get("/api/stats")
def get_stats():
    fresh = read_flights()
    if not fresh:
        return {"error": "no_data"}

    flights = list(fresh.values())
    adsb    = [f for f in flights if f["protocol"] == "ADS-B"]
    ais     = [f for f in flights if f["protocol"] == "AIS"]
    mil     = [f for f in flights if is_military(f["icao"])]
    emerg   = [f for f in flights if f.get("emergency", "")]
    with_pos= [f for f in flights if f["lat"] != 0 and f["lon"] != 0]

    def avg(lst, key):
        vals = [x[key] for x in lst if x.get(key)]
        return round(sum(vals)/len(vals), 1) if vals else 0

    # Altitude buckets
    alt_buckets = {
        "GND":    sum(1 for f in adsb if f["altitude"] <= 1000),
        "<10k":   sum(1 for f in adsb if 1000 < f["altitude"] <= 10000),
        "10-25k": sum(1 for f in adsb if 10000 < f["altitude"] <= 25000),
        "25-35k": sum(1 for f in adsb if 25000 < f["altitude"] <= 35000),
        "35k+":   sum(1 for f in adsb if f["altitude"] > 35000),
    }
    # Category breakdown
    categories: dict[str, int] = {}
    for f in flights:
        c = f.get("category", "") or "Unknown"
        categories[c] = categories.get(c, 0) + 1

    # Squawk breakdown (top 10)
    squawks: dict[str, int] = {}
    for f in flights:
        sq = f.get("squawk", "")
        if sq:
            squawks[sq] = squawks.get(sq, 0) + 1
    top_squawks = sorted(squawks.items(), key=lambda x: -x[1])[:10]

    return {
        "total":          len(flights),
        "adsb":           len(adsb),
        "ais":            len(ais),
        "military":       len(mil),
        "emergency":      len(emerg),
        "with_position":  len(with_pos),
        "avg_altitude_ft":avg(adsb, "altitude"),
        "avg_speed_kt":   avg(adsb, "speed"),
        "avg_snr_db":     avg(flights, "snr_db"),
        "altitude_buckets": alt_buckets,
        "categories":     categories,
        "top_squawks":    dict(top_squawks),
        "military_units": {f["icao"]: is_military(f["icao"]) for f in mil},
        "active_ws_clients": len(_ws_clients),
    }


@app.get("/api/alerts")
def get_alerts(limit: int = Query(50, le=500)):
    alerts = list(alert_queue)[-limit:]
    return {"count": len(alerts), "alerts": list(reversed(alerts))}


@app.get("/api/history/{icao}")
def get_history(icao: str):
    icao = icao.upper()
    cs = contact_store.get(icao)
    if not cs:
        return {"error": "not_found", "icao": icao}
    trail = list(cs["positions"])
    return {
        "icao":       icao,
        "first_seen": cs.get("first_seen"),
        "last_seen":  cs.get("last_seen"),
        "total_msgs": cs.get("total_msgs", 0),
        "military":   cs.get("military", ""),
        "positions":  [{"lat": p[0], "lon": p[1], "ts": p[2], "alt": p[3]}
                       for p in trail],
    }


@app.get("/api/military")
def get_military():
    fresh = read_flights()
    mil = {}
    for icao, f in fresh.items():
        unit = is_military(icao)
        if unit:
            f["military_unit"] = unit
            mil[icao] = f
    return {"count": len(mil), "contacts": list(mil.values())}


@app.get("/api/search")
def search(
    callsign:  Optional[str] = None,
    icao:      Optional[str] = None,
    alt_min:   int = Query(0,   ge=0),
    alt_max:   int = Query(99999, le=200000),
    speed_min: float = Query(0.0, ge=0),
    protocol:  Optional[str] = None,
    military:  bool = False,
):
    fresh = read_flights()
    results = []
    for f in fresh.values():
        if callsign and callsign.upper() not in f["callsign"].upper():
            continue
        if icao and not f["icao"].upper().startswith(icao.upper()):
            continue
        if not (alt_min <= f["altitude"] <= alt_max):
            continue
        if f["speed"] < speed_min:
            continue
        if protocol and f["protocol"] != protocol:
            continue
        if military and not is_military(f["icao"]):
            continue
        results.append(f)
    return {"count": len(results), "results": results}


@app.get("/api/intercepts")
def get_intercepts(limit: int = Query(120, le=2000)):
    if not os.path.exists(DATA_LAKE_PATH):
        return {"intercepts": []}
    rows = []
    try:
        with open(DATA_LAKE_PATH, "r", encoding="utf-8") as f:
            rows = list(csv.DictReader(f))
    except Exception:
        pass
    return {"intercepts": rows[-limit:]}


@app.get("/api/export/geojson")
def export_geojson():
    """Export current live contacts as GeoJSON FeatureCollection."""
    fresh = read_flights()
    features = []
    for f in fresh.values():
        if f["lat"] == 0 and f["lon"] == 0:
            continue
        props = {k: v for k, v in f.items() if k not in ("lat", "lon")}
        props["military"] = is_military(f["icao"])
        features.append({
            "type": "Feature",
            "geometry": {"type": "Point", "coordinates": [f["lon"], f["lat"]]},
            "properties": props,
        })
    return JSONResponse({
        "type":     "FeatureCollection",
        "features": features,
        "generated": ts_now(),
    })


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000, reload=False)
