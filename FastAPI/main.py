from fastapi import FastAPI, Request, HTTPException
from fastapi.responses import JSONResponse, HTMLResponse, FileResponse
from fastapi.middleware.cors import CORSMiddleware
import sqlite3
from datetime import datetime
from contextlib import asynccontextmanager
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel
import json
import math
from fastapi.templating import Jinja2Templates
templates = Jinja2Templates(directory="WiFiSniffing/FastAPI/templates")

def distance_m(lat1, lon1, lat2, lon2):
    R = 6371000  # rayon Terre en mètres
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lon2 - lon1)

    a = math.sin(dphi / 2)**2 + \
        math.cos(phi1) * math.cos(phi2) * math.sin(dlambda / 2)**2
    return 2 * R * math.atan2(math.sqrt(a), math.sqrt(1 - a))

def compute_position_from_last_scan():
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()

    # Dernier scan reçu
    c.execute("""
        SELECT id FROM scans
        ORDER BY created_at DESC
        LIMIT 1
    """)
    row = c.fetchone()
    if not row:
        conn.close()
        return None, []

    scan_id = row[0]

    c.execute("""
        SELECT bssid, rssi
        FROM networks
        WHERE scan_id = ?
    """, (scan_id,))
    detected = c.fetchall()

    if not detected:
        conn.close()
        return None, []

    scan_dict = {b: r for b, r in detected}

    # Tous les fingerprints
    c.execute("""
        SELECT bssid_list, rssi_list, latitude, longitude
        FROM access_points
    """)
    rows = c.fetchall()
    conn.close()

    best_score = float("inf")
    best_pos = None

    for bssid_json, rssi_json, lat, lon in rows:
        bssids = json.loads(bssid_json or "[]")
        rssis = json.loads(rssi_json or "[]")
        fp = dict(zip(bssids, rssis))

        score = 0
        for b, r in scan_dict.items():
            score += abs(r - fp.get(b, -100))

        if score < best_score:
            best_score = score
            best_pos = {"latitude": lat, "longitude": lon}

    return best_pos, detected


""" access_points.db-related classes (to create my own DB)"""
class AccessPointCreate(BaseModel):
    latitude: float
    longitude: float

class AccessPointUpdate(BaseModel):
    bssid_list: list[str]
    rssi_list: list[int]


DB_PATH = "wifi_scans.db"

@asynccontextmanager
async def lifespan(app: FastAPI):
    # initialisation de la DB au démarrage
    init_db()
    yield
    # pas de code à l'arrêt

app = FastAPI(title="WiFi Scan Collector", lifespan=lifespan)
app.mount("/static", StaticFiles(directory="WiFiSniffing/FastAPI/static"), name="static")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

def init_db():
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute('''
        CREATE TABLE IF NOT EXISTS scans (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT,
            timestamp_ms INTEGER,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    ''')
    c.execute('''
        CREATE TABLE IF NOT EXISTS networks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            scan_id INTEGER,
            group_ssid TEXT,
            ssid TEXT,
            bssid TEXT,
            rssi INTEGER,
            channel INTEGER,
            encryption TEXT,
            FOREIGN KEY(scan_id) REFERENCES scans(id)
        )
    ''')
    # Table pour la base de données des points d'accès
    c.execute('''
        CREATE TABLE IF NOT EXISTS access_points (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            bssid_list TEXT,
            rssi_list TEXT,
            latitude REAL NOT NULL,
            longitude REAL NOT NULL,
            last_seen DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    ''')
    conn.commit()
    conn.close()

# Helper to insert scan + networks
def store_scan(device_id: str, timestamp_ms: int, groups: any):
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute('INSERT INTO scans (device_id, timestamp_ms) VALUES (?, ?)', (device_id, timestamp_ms))
    scan_id = c.lastrowid
    for g in groups:
        group_ssid = g.get("ssid", "")
        items = g.get("items", [])
        for it in items:
            ssid = it.get("ssid", "")
            bssid = it.get("bssid", "")
            rssi = it.get("rssi", None)
            channel = it.get("channel", None)
            enc = it.get("enc", "")
            c.execute('''
                INSERT INTO networks (scan_id, group_ssid, ssid, bssid, rssi, channel, encryption)
                VALUES (?, ?, ?, ?, ?, ?, ?)
            ''', (scan_id, group_ssid, ssid, bssid, rssi, channel, enc))
    conn.commit()
    conn.close()
    return scan_id

@app.post("/ttn")
async def receive_ttn(request: Request):
    """
    Reçoit le JSON envoyé par l'ESP32 (schema approximatif):
    {
        "device_id": "...",
        "timestamp_ms": 123456,
        "groups": [
            {"ssid": "...", "bestRssi": -40, "items":[{...}, ...]},
        ...
        ]
    }
    """
    try:
        payload = await request.json()
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"JSON parse error: {e}")

    device_id = payload.get("device_id", "unknown")
    timestamp_ms = payload.get("timestamp_ms", None)
    groups = payload.get("groups", [])

    if not isinstance(groups, list):
        raise HTTPException(status_code=400, detail="`groups` must be a list")

    scan_id = store_scan(device_id, timestamp_ms, groups)
    return JSONResponse({"status": "success", "scan_id": scan_id})

@app.get("/wifi_scans")
async def get_wifi_scans(limit: int = 200):
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    # Get recent scans
    c.execute('SELECT id, device_id, timestamp_ms, created_at FROM scans ORDER BY created_at DESC LIMIT ?', (limit,))
    scans = c.fetchall()
    result = []
    for s in scans:
        scan_id, device_id, timestamp_ms, created_at = s
        c.execute('SELECT group_ssid, ssid, bssid, rssi, channel, encryption FROM networks WHERE scan_id = ?', (scan_id,))
        nets = c.fetchall()
        nets_list = []
        for n in nets:
            nets_list.append({
                "group_ssid": n[0],
                "ssid": n[1],
                "bssid": n[2],
                "rssi": n[3],
                "channel": n[4],
                "encryption": n[5]
            })
        result.append({
            "scan_id": scan_id,
            "device_id": device_id,
            "timestamp_ms": timestamp_ms,
            "created_at": created_at,
            "networks": nets_list
        })
    conn.close()
    return JSONResponse({"data": result})

@app.get("/wifi_scan", response_class=HTMLResponse)
async def wifi_scan_page(limit: int = 50):
    # Simple HTML frontend (table)
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute('SELECT id, device_id, timestamp_ms, created_at FROM scans ORDER BY created_at DESC LIMIT ?', (limit,))
    scans = c.fetchall()
    html = "<html><head><meta charset='utf-8'><title>WiFi Scans</title></head><body>"
    html += "<h2>Recent WiFi Scans</h2>"
    for s in scans:
        scan_id, device_id, timestamp_ms, created_at = s
        html += f"<h3>Scan {scan_id} — device: {device_id} — at {created_at} — ts_ms: {timestamp_ms}</h3>"
        html += "<table border='1' cellpadding='4' cellspacing='0'><tr><th>Group SSID</th><th>SSID</th><th>BSSID</th><th>RSSI</th><th>CH</th><th>Enc</th></tr>"
        c.execute('SELECT group_ssid, ssid, bssid, rssi, channel, encryption FROM networks WHERE scan_id = ?', (scan_id,))
        nets = c.fetchall()
        for n in nets:
            html += "<tr>"
            for col in n:
                html += f"<td>{col}</td>"
            html += "</tr>"
        html += "</table><br/>"
    conn.close()
    html += "</body></html>"
    return HTMLResponse(content=html)

@app.post("/access_points")
async def add_access_point(data: AccessPointCreate):
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()

    c.execute("""
        SELECT latitude, longitude
        FROM access_points
        ORDER BY last_seen DESC
        LIMIT 1
    """)
    last = c.fetchone()

    if last:
        dist = distance_m(last[0], last[1], data.latitude, data.longitude)
        if dist < 5:
            conn.close()
            return {
                "status": "ignored",
                "reason": f"Déplacement insuffisant ({dist:.2f} m)"
            }

    c.execute("""
        INSERT INTO access_points (latitude, longitude)
        VALUES (?, ?)
    """, (data.latitude, data.longitude))

    conn.commit()
    conn.close()
    return {"status": "success"}



@app.post("/update_access_point")
async def update_access_point(data: AccessPointUpdate):
    if len(data.bssid_list) != len(data.rssi_list):
        raise HTTPException(400, "bssid_list and rssi_list must have same length")

    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()

    c.execute("""
        UPDATE access_points
        SET bssid_list = ?, rssi_list = ?, last_seen = CURRENT_TIMESTAMP
        WHERE id = (
            SELECT id FROM access_points
            ORDER BY last_seen DESC
            LIMIT 1
        )
    """, (
        json.dumps(data.bssid_list),
        json.dumps(data.rssi_list)
    ))

    if c.rowcount == 0:
        conn.close()
        raise HTTPException(404, "No access point to update")

    conn.commit()
    conn.close()
    return {"status": "success"}


@app.get("/access_points")
async def get_access_points():
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()

    # 1️⃣ Nettoyage des entrées invalides
    c.execute("""
        DELETE FROM access_points
        WHERE latitude IS NOT NULL
        AND longitude IS NOT NULL
        AND (
                bssid_list IS NULL
            OR rssi_list IS NULL
            OR bssid_list = '[]'
            OR rssi_list = '[]'
        )
    """)
    conn.commit()

    # 2️⃣ Lecture
    c.execute("""
        SELECT id, bssid_list, rssi_list, latitude, longitude, last_seen
        FROM access_points
        ORDER BY last_seen DESC
    """)
    rows = c.fetchall()
    conn.close()

    # 3️⃣ Aplatissement
    flat = []

    for row in rows:
        ap_id, bssid_json, rssi_json, lat, lon, last_seen = row

        bssids = json.loads(bssid_json) if bssid_json else []
        rssis = json.loads(rssi_json) if rssi_json else []

        for bssid, rssi in zip(bssids, rssis):
            flat.append({
                "access_point_id": ap_id,
                "bssid": bssid,
                "rssi": rssi,
                "latitude": float(lat),
                "longitude": float(lon),
                "last_seen": last_seen
            })

    # 4️⃣ Dé-doublonnage spatial
    deduped = []

    for ap in flat:
        found = False
        for kept in deduped:
            if ap["bssid"] == kept["bssid"]:

                # Cas 1 : coordonnées strictement identiques
                if ap["latitude"] == kept["latitude"] and ap["longitude"] == kept["longitude"]:
                    if ap["last_seen"] > kept["last_seen"]:
                        kept.update(ap)
                    found = True
                    break

                # Cas 2 : proximité GPS
                d = distance_m(
                    ap["latitude"], ap["longitude"],
                    kept["latitude"], kept["longitude"]
                )
                print(f"d={d}\nlatitude={ap['latitude']}, longitude={ap['longitude']}, kept_latitude={kept['latitude']}, kept_longitude={kept['longitude']}")
                if d < 50:
                    if ap["last_seen"] > kept["last_seen"]:
                        kept.update(ap)
                    found = True
                    break

        if not found:
            deduped.append(ap)

    return {"data": deduped}

@app.get("/reset_access_points")
async def reset_access_points():
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute("DELETE FROM access_points")
    c.execute("DELETE FROM sqlite_sequence WHERE name='access_points'")
    c.execute("SELECT COUNT(*) FROM access_points")
    print("NB ROWS:", c.fetchone()[0])
    conn.commit()
    conn.close()
    return {"status": "reset done"}


@app.get("/download_access_points")
async def download_access_points():
    """
    Télécharger la base access_points.db
    """
    return FileResponse(
        path=DB_PATH,
        filename="access_points.db",
        media_type="application/octet-stream"
    )

@app.get("/", response_class=HTMLResponse)
async def root(request: Request):
    position, aps = compute_position_from_last_scan()

    return templates.TemplateResponse(
        "index.html",
        {
            "request": request,
            "position": position,
            "aps": aps
        }
    )


if __name__ == "__main__":
    import uvicorn
    init_db()
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=False)
