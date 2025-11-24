// WiFiSniffing_debug_fixed.ino
/*
 Mode :
  - #define DEBUG 1 => envoi HTTP POST local vers /ttn
  - #define DEBUG 0 => envoi via LoRa-E5 (mode actuel)
*/

#define DEBUG 0   // 1 = HTTP local (test), 0 = LoRa

#include <WiFi.h>
#include <HTTPClient.h>
#include <vector>
#include <algorithm>

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#define LORAE5TX_RX 16   // ESP32 RX2 <- LoRa TX
#define LORAE5RX_TX 17   // ESP32 TX2 -> LoRa RX
HardwareSerial LoRaSerial(2);

// Ajuster ici si tu veux un envoi toutes les 60s en mode DEBUG (test)
#define SEND_INTERVAL_MS 60000UL

// --- WiFi credentials (à adapter) ---
const char* ssid = "Galaxy A...";
const char* password = "xxxxxxx";
// Endpoint du serveur (modifier si besoin)
const char* SERVER_HOST = "http://172.XX.XX.34:8000"; // IP DU PC
const char* TTN_PATH = "/ttn"; // POST target

unsigned long lastSend = 0;

// ---------- Structures ----------
struct APEntry {
  String ssid;
  String bssid;
  int rssi;
  int channel;
  String enc;
};

struct Group {
  String ssid;
  std::vector<APEntry> items;
  int bestRssi;
};

// ---------- Helpers ----------
String escapeJsonString(const String &s) {
  String out = "";
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s.charAt(i);
    switch (c) {
      case '\"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[7];
          sprintf(buf, "\\u%04x", (unsigned char)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

bool isPhoneHotspot(const String &bssid, const String &ssid) {
  String mac = bssid; mac.replace(":", ""); mac.toUpperCase();
  if (mac.length() < 6) return false;
  String oui = mac.substring(0,6);

  static const char* PHONE_OUI[] = {
    "F8FFC2","D0034B","28E14C","B8634D","A4B1C1","F0D1A9","F4F15A","A0EDCD","A8B1D4","18AF61",
    "FC48EF","CC79CF","842519","D47B75","8C501E","4C66A6","D0C1B1","A4A72B","28BA18","78D6F0",
    "A47733","64CC2E","94E979","B0E235","38E7D8","50EED6","7CB95C",
    "E47614","009ACD","60A8FE","F4C247","D4E6B7",
    "3C286D","D85D4C","F4F5D8","8C2DAA",
    "44D884","F0BF97","38A4ED","D463C6"
  };

  for (const char* known : PHONE_OUI) {
    if (oui.equalsIgnoreCase(known)) return true;
  }

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

// ---------- LoRa helpers (unchanged) ----------
void myInitiateLoraJoin() {
  LoRaSerial.begin(9600, SERIAL_8N1, LORAE5TX_RX, LORAE5RX_TX); // UART to LoRa-E5
  delay(1000);
  Serial.println("Connecting LoRa-E5 Modem to TTN");
  LoRaSerial.println("AT");
  delay(1000);
  while (LoRaSerial.available()) {
    Serial.println("LoRa-E5: " + LoRaSerial.readString());
  }
  LoRaSerial.println("AT+DR=EU868");
  delay(200);
  LoRaSerial.println("AT+ID=DevEUI,70B3D57ED0074082");
  delay(200);
  LoRaSerial.println("AT+ID=AppEUI,0000000000000000");
  delay(200);
  LoRaSerial.println("AT+KEY=APPKEY,1F0CA11AB49FF4DEB923ACA8B69D3DE9");
  delay(200);
  LoRaSerial.println("AT+MODE=LWOTAA");
  delay(200);
  LoRaSerial.println("AT+JOIN");
  delay(5000);
  bool joined = false;
  unsigned long joinStart = millis();
  while (!joined && millis() - joinStart < 15000UL) {
    if (LoRaSerial.available()) {
      String resp = LoRaSerial.readString();
      Serial.println("LoRa-E5: " + resp);
      if (resp.indexOf("failed") == -1) {
        joined = true;
        Serial.println("Module joined the network!");
      }
    }
    delay(500);
  }
  if (!joined) Serial.println("Join timeout (continuing anyway)");
}

String myBuildHexPayload(const APEntry &ap) {
  String ssidHex;
  for (int i = 0; i < MIN(12, ap.ssid.length()); i++) {
    char c = ap.ssid.charAt(i);
    char buf[3];
    sprintf(buf, "%02X", (uint8_t)c);
    ssidHex += String(buf);
  }
  while (ssidHex.length() < 24) {
    ssidHex = "0" + ssidHex;
  }

  String bssidHex = ap.bssid;
  bssidHex.replace(":", "");
  bssidHex.toUpperCase();

  int8_t rssi = (int8_t)ap.rssi;
  char rssiHex[3];
  sprintf(rssiHex, "%02X", (uint8_t)rssi);

  char chHex[3];
  sprintf(chHex, "%02X", ap.channel);

  uint8_t encCode = 0;
  if (ap.enc == "WEP") encCode = 1;
  else if (ap.enc == "WPA-PSK") encCode = 2;
  else if (ap.enc == "WPA2-PSK") encCode = 3;
  else if (ap.enc == "WPA/WPA2-PSK") encCode = 4;
  else if (ap.enc == "WPA2-ENT") encCode = 5;
  else if (ap.enc == "WPA3-PSK") encCode = 6;
  else if (ap.enc == "WPA2/WPA3-PSK") encCode = 7;
  char encHex[3];
  sprintf(encHex, "%02X", encCode);

  String payloadHex = ssidHex + bssidHex + rssiHex + chHex + encHex;
  return payloadHex;
}

void mySendLoRaMessage(const APEntry &ap) {
  String payloadHex = myBuildHexPayload(ap);
  String cmd = "AT+MSGHEX=\"";
  cmd += payloadHex;
  cmd += "\"\r\n";
  Serial.print("Send to LoRa-E5: ");
  Serial.println(cmd);
  LoRaSerial.print(cmd);
  delay(2000);
  while (LoRaSerial.available()) {
    String resp = LoRaSerial.readString();
    Serial.println("LoRa-E5: " + resp);
    if (resp.indexOf("Please join network first") != -1) {
      myInitiateLoraJoin();
    }
  }
}

// ---------- Scan, grouping and sorting ----------
std::vector<Group> performScanGroupSort() {
  std::vector<Group> groupList;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.println("[INFO] Starting passive WiFi scan...");
  int n = WiFi.scanNetworks(false, true, true);
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
    }
  }

  // Group by SSID (Arduino String-based, linear search)
  for (auto &ap : aps) {
    bool found = false;
    for (auto &g : groupList) {
      if (g.ssid == ap.ssid) {
        g.items.push_back(ap);
        found = true;
        break;
      }
    }
    if (!found) {
      Group ng;
      ng.ssid = ap.ssid;
      ng.items.clear();
      ng.items.push_back(ap);
      ng.bestRssi = -9999;
      groupList.push_back(ng);
    }
  }

  // Sort each group's items by rssi desc and compute bestRssi
  for (auto &g : groupList) {
    std::sort(g.items.begin(), g.items.end(), [](const APEntry &a, const APEntry &b){
      return a.rssi > b.rssi;
    });
    g.bestRssi = g.items.size() ? g.items.front().rssi : -9999;
  }

  // sort groups by bestRssi desc
  std::sort(groupList.begin(), groupList.end(), [](const Group &a, const Group &b){
    return a.bestRssi > b.bestRssi;
  });

  WiFi.scanDelete();
  return groupList;
}

// ---------- Build JSON from groupList ----------
String buildJsonFromGroups(const std::vector<Group> &groups) {
  String json = "{";
  // device id: MAC
  String mac = WiFi.macAddress();
  json += "\"device_id\":\"" + escapeJsonString(mac) + "\"";

  // timestamp (millis) and optional simple time
  json += ",\"timestamp_ms\":" + String(millis());

  json += ",\"groups\":[";
  bool firstG = true;
  for (auto &g : groups) {
    if (!firstG) json += ",";
    firstG = false;
    json += "{";
    json += "\"ssid\":\"" + escapeJsonString(g.ssid) + "\"";
    json += ",\"bestRssi\":" + String(g.bestRssi);
    json += ",\"items\":[";
    bool firstI = true;
    for (auto &it : g.items) {
      if (!firstI) json += ",";
      firstI = false;
      json += "{";
      json += "\"ssid\":\"" + escapeJsonString(it.ssid) + "\"";
      json += ",\"bssid\":\"" + escapeJsonString(it.bssid) + "\"";
      json += ",\"rssi\":" + String(it.rssi);
      json += ",\"channel\":" + String(it.channel);
      json += ",\"enc\":\"" + escapeJsonString(it.enc) + "\"";
      json += "}";
    }
    json += "]";
    json += "}";
  }
  json += "]";
  json += "}";
  return json;
}

// ---------- HTTP POST send ----------
bool sendHTTPPost(const String &url, const String &payload) {
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int httpResponseCode = http.POST(payload);
  if (httpResponseCode > 0) {
    String resp = http.getString();
    Serial.printf("[HTTP] code=%d resp=%s\n", httpResponseCode, resp.c_str());
    http.end();
    return (httpResponseCode >= 200 && httpResponseCode < 300);
  } else {
    Serial.printf("[HTTP] POST failed, error: %s\n", http.errorToString(httpResponseCode).c_str());
    http.end();
    return false;
  }
}

void sendScanOverHttp(const std::vector<Group> &groups) {
  String json = buildJsonFromGroups(groups);
  String url = String(SERVER_HOST) + String(TTN_PATH);
  Serial.println("[HTTP] Sending JSON to " + url);
  Serial.println(json);
  bool ok = sendHTTPPost(url, json);
  if (!ok) {
    Serial.println("[HTTP] Send failed");
  } else {
    Serial.println("[HTTP] Send OK");
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("[INFO] ESP32 WiFi passive grouped-sorted scan");

#if DEBUG
  // Connect WiFi (needed for HTTP)
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("[WIFI] Connecting");
  unsigned long tstart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - tstart < 20000UL) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WIFI] Connected, IP=");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WIFI] Connect failed (continuing, scan still possible)");
  }
#else
  // Initialize LoRa modem for TTN
  myInitiateLoraJoin();
#endif

  lastSend = 0;
}

void loop() {
  // do scan+send every SEND_INTERVAL_MS
  if (millis() - lastSend >= SEND_INTERVAL_MS) {
    lastSend = millis();
    auto groups = performScanGroupSort();

    // Debug print on serial
    Serial.println("------ Groups/Networks ------");
    int idx = 1;
    for (auto &g : groups) {
      Serial.printf("-- SSID: %s (RSSI max=%d) --\n", g.ssid.c_str(), g.bestRssi);
      for (auto &it : g.items) {
        Serial.printf("%2d | %s | %s | %4d | %2d | %s\n",
                      idx++,
                      it.ssid.c_str(),
                      it.bssid.c_str(),
                      it.rssi,
                      it.channel,
                      it.enc.c_str());
      }
    }

#if DEBUG
    sendScanOverHttp(groups);
#else
    Serial.println("\n[LoRa] Début de l'envoi des messages...");
    for (auto &g : groups) {
      for (auto &it : g.items) {
        mySendLoRaMessage(it);
        delay(10000);
      }
    }
    Serial.println("[LoRa] Fin de l'envoi des messages.");
#endif
  }

  delay(200); // petit yield
}
