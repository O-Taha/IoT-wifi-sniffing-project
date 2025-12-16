from fastapi import FastAPI, Request, HTTPException
from fastapi.responses import JSONResponse, HTMLResponse, FileResponse
from fastapi.middleware.cors import CORSMiddleware
import sqlite3
from datetime import datetime
from contextlib import asynccontextmanager
from fastapi.staticfiles import StaticFiles

DB_PATH = "wifi_scans.db"

@asynccontextmanager
async def lifespan(app: FastAPI):
    # initialisation de la DB au démarrage
    init_db()
    yield
    # pas de code à l'arrêt

app = FastAPI(title="WiFi Scan Collector", lifespan=lifespan)
app.mount("/static", StaticFiles(directory="static"), name="static")

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
            bssid TEXT NOT NULL UNIQUE,
            ssid TEXT,
            latitude REAL NOT NULL,
            longitude REAL NOT NULL,
            last_seen DATETIME DEFAULT CURRENT_TIMESTAMP,
            rssi INTEGER,
            channel INTEGER,
            encryption TEXT
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
async def add_access_point(data: dict):
    """
    Endpoint pour que le téléphone enregistre un point d'accès avec ses coordonnées GPS.
    Exemple de payload :
    {
        "bssid": "A1:B2:C3:D4:E5:F6",
        "ssid": "MonReseau",
        "latitude": 48.8566,
        "longitude": 2.3522
    }
    """
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute('''
        INSERT OR REPLACE INTO access_points (bssid, ssid, latitude, longitude, last_seen)
        VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP)
    ''', (
        data['bssid'],
        data.get('ssid', ''),
        data['latitude'],
        data['longitude']
    ))
    conn.commit()
    conn.close()
    return {"status": "success", "bssid": data['bssid']}

@app.post("/update_access_point")
async def update_access_point(data: dict):
    """
    Endpoint pour que l'ESP32 met à jour les infos WiFi (RSSI, channel, etc.) d'un point d'accès existant.
    Exemple de payload :
    {
        "bssid": "A1:B2:C3:D4:E5:F6",
        "rssi": -65,
        "channel": 6,
        "encryption": "WPA2-PSK"
    }
    """
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute('''
        UPDATE access_points
        SET rssi = ?, channel = ?, encryption = ?, last_seen = CURRENT_TIMESTAMP
        WHERE bssid = ?
    ''', (
        data['rssi'],
        data['channel'],
        data['encryption'],
        data['bssid']
    ))
    conn.commit()
    conn.close()
    return {"status": "success", "bssid": data['bssid']}

@app.get("/access_points")
async def get_access_points():
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute('SELECT bssid, ssid, latitude, longitude, rssi, channel, encryption FROM access_points')
    aps = c.fetchall()
    conn.close()
    result = []
    for ap in aps:
        result.append({
            "bssid": ap[0],
            "ssid": ap[1],
            "latitude": ap[2],
            "longitude": ap[3],
            "rssi": ap[4],
            "channel": ap[5],
            "encryption": ap[6]
        })
    return {"data": result}

@app.get("/download_access_points")
async def download_access_points(): 
    #curl -o access_points.db http://<IP_DU_SERVEUR>:8000/download_access_points
    """
    Endpoint pour télécharger la base de données access_points.db.
    """
    return FileResponse(DB_PATH, filename="access_points.db")

@app.get("/", response_class=HTMLResponse)
async def root():
    return FileResponse("static/index.html")

if __name__ == "__main__":
    import uvicorn
    init_db()
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=False)
