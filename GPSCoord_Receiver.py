import socket
import ast
host = '10.14.2.136'  # Écoute sur toutes les interfaces réseau
port = 12345

try:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, port))
    s.listen(1)
    print(f"Serveur démarré sur {host}:{port}")

    try:
        while True:
            c, addr = s.accept()
            print(f"Connexion établie depuis {addr}")

            try:
                while True:
                    data = c.recv(2048)
                    if not data:
                        break  # Le client a fermé la connexion
                    print(f"Reçu depuis {addr}: {ast.literal_eval(data.decode('utf-8'))['fused']['latitude']}")

                    # Envoie une réponse au client
                    response = b"Position recue avec succes"
                    c.sendall(response)
                    print(f"Reponse envoyee a {addr}")

            except Exception as e:
                print(f"Erreur avec le client {addr}: {e}")
            finally:
                c.close()  # Ferme la connexion avec le client
                print(f"Connexion fermee avec {addr}")

    except KeyboardInterrupt:
        print("Arret du serveur...")
    finally:
        s.close()  # Ferme le socket du serveur
        print("Serveur arrete")

except Exception as e:
    print(f"Erreur de demarrage du serveur: {e}")
