import socket
import json
import requests

HOST = "10.227.217.136"
PORT = 12345
FASTAPI_URL = "http://127.0.0.1:8000/access_points"  # adapte si besoin

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((HOST, PORT))
s.listen(1)

print(f"Serveur TCP démarré sur {HOST}:{PORT}")

try:
    while True:
        c, addr = s.accept()
        print(f"Connexion depuis {addr}")

        try:
            while True:
                data = c.recv(2048)
                if not data:
                    break

                # 1️⃣ Parse JSON
                payload = json.loads(data.decode("utf-8"))

                lat = payload["latitude"]
                lon = payload["longitude"]

                print(f"Coordonnées reçues : {lat}, {lon}")

                # 2️⃣ Forward vers FastAPI
                response = requests.post(
                    FASTAPI_URL,
                    json={
                        "latitude": lat,
                        "longitude": lon
                    },
                    timeout=3
                )

                if response.ok:
                    c.sendall(b"Position recue et envoyee au serveur")
                else:
                    c.sendall(b"Erreur FastAPI")

        except Exception as e:
            print(f"Erreur client {addr}: {e}")

        finally:
            c.close()
            print(f"Connexion fermee avec {addr}")

except KeyboardInterrupt:
    print("Arret serveur")

finally:
    s.close()

