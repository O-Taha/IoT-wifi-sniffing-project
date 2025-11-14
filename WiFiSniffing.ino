/*
ESP32 WiFi Scan (mode passif) -> Group by SSID -> Sort by RSSI

Usage:
 - Le sketch effectue un scan WiFi passif au démarrage, groupe les APs par SSID,
   trie chaque groupe par RSSI décroissant, puis trie les groupes par le meilleur RSSI.
 - Résultat affiché dans le moniteur série.

Notes:
 - Appelle WiFi.scanDelete() pour libérer la mémoire des résultats.
 - Le mode passif n’envoie pas de requêtes probe, il écoute uniquement les balises (beacons).
*/

#include <HTTPClient.h>
#include <WiFi.h>
#include <vector>
#include <map>
#include <algorithm>

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#define LORAE5TX_RX 16   // ESP32 RX2 <- LoRa TX
#define LORAE5RX_TX 17   // ESP32 TX2 -> LoRa RX
HardwareSerial LoRaSerial(2);

struct APEntry {
  String ssid;
  String bssid;
  int rssi;
  int channel;
  String enc;
};

bool isPhoneHotspot(const String &bssid, const String &ssid) {
  // Normalisation MAC
  String mac = bssid; mac.replace(":", ""); mac.toUpperCase();
  if (mac.length() < 6) return false;
  String oui = mac.substring(0,6);

  static const char* PHONE_OUI[] = {
    // Apple
    "F8FFC2","D0034B","28E14C","B8634D","A4B1C1","F0D1A9","F4F15A","A0EDCD","A8B1D4","18AF61",
    // Samsung
    "FC48EF","CC79CF","842519","D47B75","8C501E","4C66A6","D0C1B1","A4A72B","28BA18","78D6F0",
    // Xiaomi
    "A47733","64CC2E","94E979","B0E235","38E7D8","50EED6","7CB95C",
    // Huawei
    "E47614","009ACD","60A8FE","F4C247","D4E6B7",
    // Google Pixel
    "3C286D","D85D4C","F4F5D8","8C2DAA",
    // OnePlus
    "44D884","F0BF97","38A4ED","D463C6"
  };

  for (const char* known : PHONE_OUI) {
    if (oui.equalsIgnoreCase(known)) return true;
  }

  // Filtrage via SSID connus
  String s = ssid; s.toUpperCase();
  if (s.indexOf("IPHONE") >= 0) return true;
  if (s.startsWith("ANDROID")) return true;
  if (s.startsWith("GALAXY")) return true;
  if (s.indexOf("HUAWEI") >= 0) return true;
  if (s.startsWith("XIAOMI")) return true;
  if (s.startsWith("REDMI")) return true;
  if (s.indexOf("HOTSPOT") >= 0) return true;

  return false;
}

// --------------------------------------------------------------

String encTypeToString(wifi_auth_mode_t mode) {
  switch(mode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK: return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3-PSK";
    default: return "UNKNOWN";
  }
}

void printHeader() {
  Serial.println("Nr | SSID | BSSID | RSSI(dBm) | CH | Encryption");
}

void SSIDGroup_RSSISort_Scan() {
  // Mode station (nécessaire pour scanner)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(); // nettoie l'état précédent
  delay(100);

  Serial.println("[INFO] Starting passive WiFi scan...");
  // mode passif activé (3ème paramètre = true)
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true, /*passive=*/true);
  Serial.printf("[INFO] End of scan, %d networks found\n", n);

  std::vector<APEntry> aps;
  for (int i = 0; i < n; ++i) {
    APEntry a;
    a.ssid = WiFi.SSID(i);
    if (a.ssid.length() == 0) a.ssid = "<hidden>";
    a.bssid = WiFi.BSSIDstr(i);
    a.rssi = WiFi.RSSI(i);
    a.channel = WiFi.channel(i);
    a.enc = encTypeToString(WiFi.encryptionType(i));
    if (!isPhoneHotspot(a.bssid, a.ssid)) {
      aps.push_back(a);
    }(a);
  }

  // Groupement par SSID
  std::map<std::string, std::vector<APEntry>> groups;
  for (auto &ap : aps) {
    std::string key = std::string(ap.ssid.c_str());
    groups[key].push_back(ap);
  }

  struct Group {
    std::string ssid;
    std::vector<APEntry> items;
    int bestRssi;
  };

  std::vector<Group> groupList;
  groupList.reserve(groups.size());

  for (auto &kv : groups) {
    Group g;
    g.ssid = kv.first;
    g.items = kv.second;
    // Tri interne (du plus fort au plus faible RSSI)
    std::sort(g.items.begin(), g.items.end(), [](const APEntry &a, const APEntry &b){
      return a.rssi > b.rssi;
    });
    g.bestRssi = g.items.size() ? g.items.front().rssi : -9999;
    groupList.push_back(g);
  }

  // Tri des groupes selon le meilleur RSSI
  std::sort(groupList.begin(), groupList.end(), [](const Group &a, const Group &b){
    return a.bestRssi > b.bestRssi;
  });

  // Affichage
  printHeader();
  int idx = 1;
  for (auto &g : groupList) {
    Serial.printf("-- SSID: %s (RSSI max=%d) --\n", g.ssid.c_str(), g.bestRssi);
    for (auto &it : g.items) {
      Serial.printf("%2d | %s | %s | %4d | %2d | %s\n",
                    idx++,
                    it.ssid.c_str(),
                    it.bssid.c_str(),
                    it.rssi,
                    it.channel,
                    it.enc.c_str());
      mySendLoRaMessage(it);
    }
  }

  WiFi.scanDelete();
}

void myInitiateLoraJoin() {
  LoRaSerial.begin(9600, SERIAL_8N1, LORAE5TX_RX, LORAE5RX_TX); // UART to LoRa-E5
  delay(1000);
  Serial.println("Connecting LoRa-E5 Modem to TTN");
  // Vérification du module
  LoRaSerial.println("AT");
  delay(1000);
  while (LoRaSerial.available()) {
    Serial.println("LoRa-E5: " + LoRaSerial.readString());
  }

  LoRaSerial.println("AT+DR=EU868");
  delay(1000);

  LoRaSerial.println("AT+ID=DevEUI,70B3D57ED0074082");
  delay(1000);

  LoRaSerial.println("AT+ID=AppEUI,0000000000000000");
  delay(1000);

  LoRaSerial.println("AT+KEY=APPKEY,1F0CA11AB49FF4DEB923ACA8B69D3DE9");
  delay(1000);

  LoRaSerial.println("AT+MODE=LWOTAA");
  delay(1000);

  // Join réseau
  LoRaSerial.println("AT+JOIN");
  delay(5000);

  // Attente du join
  bool joined = false;
  while (!joined) {
    Serial.println("Verifying Network Join");
    delay(5000);
    if (LoRaSerial.available()) {
      String resp = LoRaSerial.readString();
      Serial.println("LoRa-E5: " + resp);
      if (resp.indexOf("failed") == -1) { //If the resp doesn't contain the string "failed"
        joined = true;
        Serial.println("Module joined the network!");
      }
      else {
        Serial.println("Join failed, retrying...");
        LoRaSerial.println("AT+JOIN");  // relancer le join
      }
    }
  }
}

String myBuildHexPayload(const APEntry &ap) {
  // 1. Conversion du SSID en hexadécimal (on prend les 4 premiers caractères pour économiser de l'espace)
  String ssidHex;
  for (int i = 0; i < MIN(4, ap.ssid.length()); i++) {
    char c = ap.ssid.charAt(i);
    ssidHex += String(c, HEX);
  }
  // Remplir avec des zéros si le SSID est plus court que 4 caractères
  while (ssidHex.length() < 8) {
    ssidHex = "0" + ssidHex;
  }

  // 2. Conversion du BSSID en hexadécimal (sans les ":" et en majuscules)
  String bssidHex = ap.bssid;
  bssidHex.replace(":", "");
  bssidHex.toUpperCase();

  // 3. Conversion du RSSI en hexadécimal (1 octet signé)
  int8_t rssi = (int8_t)ap.rssi;
  char rssiHex[3];
  sprintf(rssiHex, "%02X", (uint8_t)rssi);

  // 4. Conversion du CH en hexadécimal (1 octet)
  char chHex[3];
  sprintf(chHex, "%02X", ap.channel);

  // 5. Conversion de l'Encryption en code numérique (1 octet)
  uint8_t encCode = 0; // Par défaut : OPEN
  if (ap.enc == "WEP") encCode = 1;
  else if (ap.enc == "WPA-PSK") encCode = 2;
  else if (ap.enc == "WPA2-PSK") encCode = 3;
  else if (ap.enc == "WPA/WPA2-PSK") encCode = 4;
  else if (ap.enc == "WPA2-ENT") encCode = 5;
  else if (ap.enc == "WPA3-PSK") encCode = 6;
  else if (ap.enc == "WPA2/WPA3-PSK") encCode = 7;
  char encHex[3];
  sprintf(encHex, "%02X", encCode);

  // 6. Construction du payload final (SSID(8) + BSSID(12) + RSSI(2) + CH(2) + Enc(2))
  String payloadHex = ssidHex + bssidHex + rssiHex + chHex + encHex;

  return payloadHex;
}

void mySendLoRaMessage(const APEntry &ap) {
  String payloadHex = myBuildHexPayload(ap);

  // Construction de la commande AT pour LoRa-E5
  String cmd = "AT+MSGHEX=\"";
  cmd += payloadHex;
  cmd += "\"\r\n";

  Serial.print("Send to LoRa-E5: ");
  Serial.println(cmd);

  // Envoi au module LoRa-E5
  LoRaSerial.print(cmd);

  // Lecture de la réponse
  delay(2000);
  String resp;
  while (LoRaSerial.available()) {
    resp = LoRaSerial.readString();
    Serial.println("LoRa-E5: " + resp);
  }
}


void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("[INFO] ESP32 WiFi passive grouped-sorted scan");
  myInitiateLoraJoin();
}

void loop() {
  delay(10000);
  SSIDGroup_RSSISort_Scan();
}
