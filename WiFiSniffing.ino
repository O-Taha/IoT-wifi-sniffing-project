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

#include <WiFi.h>
#include <vector>
#include <map>
#include <algorithm>

struct APEntry {
  String ssid;
  String bssid;
  int rssi;
  int channel;
  String enc;
};

// --------------------------------------------------------------
//  Détection d'un hotspot smartphone
// --------------------------------------------------------------
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
  if (s.indexOf("HUAWEI") >= 0) return true;
  if (s.startsWith("ANDROID")) return true;
  if (s.startsWith("GALAXY")) return true;
  if (s.startsWith("XIAOMI")) return true;
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

  Serial.println("Démarrage du scan WiFi passif...");
  // mode passif activé (3ème paramètre = true)
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true, /*passive=*/true);
  Serial.printf("Scan terminé, %d réseaux trouvés\n", n);

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

  // Filtrage : exclusion des hotspots de smartphones
  // Liste simplifiée d'OUI (Apple, Samsung, Xiaomi, Huawei)
  auto isPhoneHotspot = [](const String &bssid, const String &ssid){
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
    if (s.startsWith("IPHONE")) return true;
    if (s.startsWith("ANDROID")) return true;
    if (s.startsWith("GALAXY")) return true;
    if (s.indexOf("HOTSPOT") >= 0) return true;

    return false;
  };

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
    }
  }

  WiFi.scanDelete();
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("ESP32 WiFi passive grouped-sorted scan");
  SSIDGroup_RSSISort_Scan();
}

void loop() {
  delay(10000);
}
