#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h> 
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <HTTPUpdate.h>
#include <esp_task_wdt.h>

// --- 1. CONFIGURATION ---
const char* googleUrl = "YOUR_GOOGLE_SCRIPT_URL_HERE"; 
const char* ap_ssid = "pacet_project";
const char* ap_pass = "1234512345";
const char* firmwareUrl = "YOUR_GITHUB_RAW_URL_HERE"; 

const int pumpPins[] = {18, 19, 21, 22}; 
const unsigned long stableDelay = 500; 
#define WDT_TIMEOUT 60 

// --- 2. SYSTEM VARIABLES ---
float flowRates[4];
String currSSID, currPASS, lastSyncStatus = "Never";
bool hasInternet = false, lastInternetState = false;
unsigned long lastStateChange[4], startTimeMillis[4], lastNetCheck = 0;
bool confirmedState[4] = {false, false, false, false};
String startTimeStr[4], startDateStr[4], startStatus[4];

unsigned long lastHeartbeat = 0;
const unsigned long heartbeatInterval = 3600000; 
unsigned long firstUploadFailTime = 0; 

WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "id.pool.ntp.org", 25200); 
Preferences prefs;

// --- STRICT TIME VALIDATOR (Prevents 2036 Bug) ---
bool isTimeValid() {
  unsigned long epoch = timeClient.getEpochTime();
  return (epoch > 1700000000 && epoch < 2000000000); 
}

// --- 3. BULLETPROOF SYNC ENGINE ---
void uploadFromFilesystem() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!isTimeValid()) return; 

  if (LittleFS.exists("/processing.txt")) {
    Serial.println("[SYNC] Rescuing interrupted upload...");
  } else if (LittleFS.exists("/backlog.txt")) {
    LittleFS.rename("/backlog.txt", "/processing.txt");
  } else { return; }

  File f = LittleFS.open("/processing.txt", "r");
  if (!f) return;

  WiFiClientSecure client; client.setInsecure(); 
  unsigned long currentEpoch = timeClient.getEpochTime();
  unsigned long currentUptimeSec = millis() / 1000;
  unsigned long bootEpoch = currentEpoch - currentUptimeSec;

  while (f.available()) {
    esp_task_wdt_reset(); 
    String line = f.readStringUntil('\n'); line.trim();
    
    if (line.startsWith("{") && line.endsWith("}")) {
      if (line.indexOf("\"date\":\"PENDING\"") != -1) {
        int upStart = line.indexOf("\"uptime\":") + 9;
        int upEnd = line.indexOf(",", upStart);
        if (upEnd == -1) upEnd = line.indexOf("}", upStart); 
        
        unsigned long eventUptime = line.substring(upStart, upEnd).toInt();
        time_t realEpoch = bootEpoch + eventUptime;
        struct tm * ti = localtime(&realEpoch);
        char dateBuf[15]; sprintf(dateBuf, "%04d-%02d-%02d", ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday);
        char timeBuf[10]; sprintf(timeBuf, "%02d:%02d:%02d", ti->tm_hour, ti->tm_min, ti->tm_sec);
        
        line.replace("\"date\":\"PENDING\"", "\"date\":\"" + String(dateBuf) + "\"");
        line.replace("\"timeOn\":\"PENDING\"", "\"timeOn\":\"" + String(timeBuf) + "\"");
        line.replace("\"timeOff\":\"PENDING\"", "\"timeOff\":\"" + String(timeBuf) + "\""); 
      }

      HTTPClient http; http.begin(client, googleUrl); http.setTimeout(15000); 
      http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      http.addHeader("Content-Type", "application/json");
      
      int code = http.POST(line);
      String response = http.getString(); response.trim();
      
      if ((code == 200 || code == 302) && response == "SUCCESS") {
        lastSyncStatus = timeClient.getFormattedTime();
        firstUploadFailTime = 0; 
      } else {
        if (firstUploadFailTime == 0) firstUploadFailTime = millis();
        if (millis() - firstUploadFailTime > 3600000) {
           Serial.println("[ERROR] Discarding buggy data to clear queue.");
        } else {
           File fRetry = LittleFS.open("/backlog.txt", "a");
           if(fRetry) { fRetry.println(line); fRetry.close(); }
           delay(2000); 
        }
      }
      http.end();
    }
  }
  f.close(); LittleFS.remove("/processing.txt");
}

bool checkInternet() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http; http.begin("http://connectivitycheck.gstatic.com/generate_204");
  int code = http.GET(); http.end(); return (code == 204);
}

// --- 4. WEB UI HANDLERS ---
void handleRoot() {
  String s = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  s += "<style>body{font-family:sans-serif; padding:20px; background:#f0f2f5;} .card{background:white; padding:15px; border-radius:10px; margin-bottom:15px;} .btn{color:white; padding:12px; border:none; border-radius:5px; width:100%; display:block; text-align:center; margin-top:10px;}</style></head><body>";
  s += "<div class='card'><h2>Pump Data Logger</h2>";
  s += "<p>Router: <b>" + String(WiFi.status()==WL_CONNECTED ? WiFi.SSID() : "Offline") + "</b></p>";
  s += "<p>Sync: " + lastSyncStatus + "</p>";
  s += "<a href='/sync' class='btn' style='background:#4CAF50;'>FORCE SYNC</a>";
  s += "<a href='/update_ota' class='btn' style='background:#9C27B0;' onclick=\"return confirm('Download firmware from GitHub?')\">UPDATE FIRMWARE</a>";
  if (WiFi.status() == WL_CONNECTED) s += "<a href='/disconnect' class='btn' style='background:#FF9800;'>DISCONNECT WIFI</a>";
  s += "<a href='/purge' class='btn' style='background:#f44336;' onclick=\"return confirm('Wipe all logs?')\">PURGE LOGS</a></div>";
  s += "<div class='card'><h3>Pump Calibration (L/sec)</h3>";
  for(int i=0; i<4; i++) s += "Pump "+String(i+1)+": <form action='/saveRate' style='display:inline;'><input name='v"+String(i)+"' value='"+String(flowRates[i], 3)+"' size='6'> <input type='submit' value='SAVE'></form><br><br>";
  s += "</div><div class='card'><a href='/scan' class='btn' style='background:#2196F3;'>WIFI SETUP</a></div></body></html>";
  server.send(200, "text/html", s);
}

void handleOTA() {
  if (WiFi.status() != WL_CONNECTED) { server.send(200, "text/html", "No WiFi."); return; }
  server.send(200, "text/html", "Update initiated. Rebooting in ~60s.");
  WiFiClientSecure client; client.setInsecure(); 
  t_httpUpdate_return ret = httpUpdate.update(client, firmwareUrl);
  if (ret == HTTP_UPDATE_OK) ESP.restart();
}

void handleScan() { WiFi.disconnect(); delay(500); int n = WiFi.scanNetworks(); String s = "<html><body><h3>Networks</h3>"; for (int i = 0; i < n; ++i) s += "<p><a href='/select?s=" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + "</a></p>"; WiFi.scanDelete(); WiFi.begin(currSSID.c_str(), currPASS.c_str()); server.send(200, "text/html", s + "</body></html>"); }
void handleSelect() { String ssid = server.arg("s"); String s = "<html><body><h3>Connect to " + ssid + "</h3><form action='/saveWiFi'><input type='hidden' name='s' value='" + ssid + "'>Pass: <input name='p' type='password'><input type='submit' value='OK'></form></body></html>"; server.send(200, "text/html", s); }
void handleSaveWiFi() { currSSID = server.arg("s"); currPASS = server.arg("p"); prefs.putString("ssid", currSSID); prefs.putString("pass", currPASS); server.send(200, "text/html", "Saved. Rebooting..."); delay(1000); ESP.restart(); }
void handleSaveRate() { for(int i=0; i<4; i++) { String arg = "v" + String(i); if(server.hasArg(arg)) { flowRates[i] = server.arg(arg).toFloat(); prefs.putFloat(("q"+String(i+1)).c_str(), flowRates[i]); } } server.send(200, "text/html", "Saved. <a href='/'>Back</a>"); }
void handleDisconnect() { prefs.putString("ssid", ""); prefs.putString("pass", ""); server.send(200, "text/html", "Forgotten. Rebooting..."); delay(1000); ESP.restart(); }

// --- 5. SETUP & MAIN LOOP ---
void setup() {
  Serial.begin(115200);
  
  // --- NEW V3.0 WATCHDOG INITIALIZATION ---
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, 
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
  
  LittleFS.begin(true); prefs.begin("flow", false);
  for(int i=0; i<4; i++) flowRates[i] = prefs.getFloat(("q"+String(i+1)).c_str(), 0.500);
  currSSID = prefs.getString("ssid", ""); currPASS = prefs.getString("pass", "");
  WiFi.mode(WIFI_AP_STA); WiFi.softAP(ap_ssid, ap_pass);
  if (currSSID != "") WiFi.begin(currSSID.c_str(), currPASS.c_str());
  for(int i=0; i<4; i++) { pinMode(pumpPins[i], INPUT_PULLUP); lastStateChange[i] = millis(); }
  timeClient.begin();
  server.on("/", handleRoot); server.on("/scan", handleScan); server.on("/select", handleSelect); server.on("/saveWiFi", handleSaveWiFi); server.on("/saveRate", handleSaveRate); server.on("/disconnect", handleDisconnect); server.on("/update_ota", handleOTA); server.on("/purge", [](){ LittleFS.remove("/backlog.txt"); server.send(200, "text/html", "Purged."); }); server.on("/sync", [](){ uploadFromFilesystem(); server.send(200, "text/html", "Syncing..."); });
  server.begin();
}

void loop() {
  esp_task_wdt_reset(); 
  server.handleClient(); timeClient.update();
  
  // --- HEARTBEAT & OTA LISTENER ---
  if (millis() - lastHeartbeat > heartbeatInterval) {
    if (WiFi.status() == WL_CONNECTED && isTimeValid()) {
      time_t rawtime = timeClient.getEpochTime(); struct tm * ti = localtime(&rawtime);
      char dateBuf[15]; sprintf(dateBuf, "%04d-%02d-%02d", ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday);
      String hTime = timeClient.getFormattedTime();
      String hbJson = "{\"date\":\"" + String(dateBuf) + "\",\"outlet\":0,\"timeOn\":\"" + hTime + "\",\"timeOff\":\"" + hTime + "\",\"duration\":0,\"volume\":0,\"status\":\"HEARTBEAT\"}";
      WiFiClientSecure client; client.setInsecure();
      HTTPClient http; http.begin(client, googleUrl); http.addHeader("Content-Type", "application/json");
      http.POST(hbJson); String payload = http.getString(); payload.trim();
      if (payload == "TRIGGER_OTA") {
        t_httpUpdate_return ret = httpUpdate.update(client, firmwareUrl);
        if (ret == HTTP_UPDATE_OK) ESP.restart();
      }
      http.end(); lastHeartbeat = millis();
    }
  }

  if (millis() - lastNetCheck > 20000) {
    bool currentHasInternet = checkInternet();
    if (currentHasInternet && !lastInternetState) { timeClient.forceUpdate(); uploadFromFilesystem(); }
    hasInternet = currentHasInternet; lastInternetState = currentHasInternet; lastNetCheck = millis();
  }
  
  // --- PUMP CYCLE LOGIC ---
  for (int i = 0; i < 4; i++) {
    bool raw = (digitalRead(pumpPins[i]) == LOW); 
    if (raw != confirmedState[i]) {
      if (millis() - lastStateChange[i] > stableDelay) {
        confirmedState[i] = raw;
        if (confirmedState[i]) {
          if (!isTimeValid()) {
            startDateStr[i] = "PENDING"; startTimeStr[i] = "PENDING"; startStatus[i] = "[!] RECOVERED_TIME";
          } else {
            time_t rawtime = timeClient.getEpochTime(); struct tm * ti = localtime(&rawtime);
            char dateBuf[15]; sprintf(dateBuf, "%04d-%02d-%02d", ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday);
            startDateStr[i] = String(dateBuf); startTimeStr[i] = timeClient.getFormattedTime(); startStatus[i] = "OK";
          }
          startTimeMillis[i] = millis();
        } else {
          float dur = (millis() - startTimeMillis[i]) / 1000.0;
          if (dur >= 5.0) { 
            String js;
            if (!isTimeValid()) {
              unsigned long currentUptimeSec = millis() / 1000;
              js = "{\"date\":\"PENDING\",\"outlet\":" + String(i+1) + ",\"timeOn\":\"PENDING\",\"timeOff\":\"PENDING\",\"uptime\":" + String(currentUptimeSec) + ",\"duration\":" + String(dur) + ",\"volume\":" + String(dur * flowRates[i]) + ",\"status\":\"" + startStatus[i] + "\"}";
            } else {
              js = "{\"date\":\"" + startDateStr[i] + "\",\"outlet\":" + String(i+1) + ",\"timeOn\":\"" + startTimeStr[i] + "\",\"timeOff\":\"" + timeClient.getFormattedTime() + "\",\"duration\":" + String(dur) + ",\"volume\":" + String(dur * flowRates[i]) + ",\"status\":\"" + startStatus[i] + "\"}";
            }
            File f = LittleFS.open("/backlog.txt", "a");
            if (f) { f.println(js); f.flush(); f.close(); }
            uploadFromFilesystem(); 
          }
        }
      }
    } else { lastStateChange[i] = millis(); }
  }
}