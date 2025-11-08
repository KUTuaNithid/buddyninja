#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "time.h"

// -------- CONFIG -----------
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

const char* MQTT_BROKER = "test.mosquitto.org";
const uint16_t MQTT_PORT = 1883;

const char* DEVICE_ID = "ESP32_001";
const char* TOPIC_FMT = "v1/devices/%s/telemetry";

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 60 * 60; // Thailand is UTC+7
const int   daylightOffset_sec = 0;

// --- Publish time window config (hours, 24h) ---
const int PUBLISH_ALLOW_START_HOUR = 6;  // 06:00
const int PUBLISH_ALLOW_END_HOUR   = 18; // 18:00 (12 hours window)

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
TinyGPSPlus gps;

char topic[128];

// ---- reconnect / status control ----
volatile bool wifiReconnectRequested = false; // set by event handler for main loop to act on
volatile bool wifiConnected = false;

unsigned long wifiLastReconnectAttempt = 0;
unsigned long wifiReconnectMs = 2000; // start 2s

bool mqttConnected = false;
unsigned long mqttLastConnectAttempt = 0;
unsigned long mqttConnectMs = 1000;

uint16_t lon_to_u16(double lon){
  double scaled = (lon + 180.0) * (65535.0 / 360.0);
  return (uint16_t)round(scaled);
}
uint16_t lat_to_u16(double lat){
  double scaled = (lat + 90.0) * (65535.0 / 180.0);
  return (uint16_t)round(scaled);
}

String make_payload_hex(double lon, double lat, int batt){
  uint16_t lon16 = lon_to_u16(lon);
  uint16_t lat16 = lat_to_u16(lat);
  uint8_t batt8 = (uint8_t)constrain(batt, 0, 255);
  char buf[10];
  snprintf(buf, sizeof(buf), "%04X%04X%02X", lon16, lat16, batt8);
  return String(buf);
}

bool checkPublishPeriod(){

  bool publishAllowed = false;

  // Get local time to decide if publishing is allowed
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    int curMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int startMinutes = PUBLISH_ALLOW_START_HOUR * 60;
    int endMinutes = PUBLISH_ALLOW_END_HOUR * 60;

    if (startMinutes <= endMinutes) {
      // normal (same-day) window, e.g. 06:00 - 18:00
      publishAllowed = (curMinutes >= startMinutes && curMinutes < endMinutes);
    } else {
      // wrap-around window (e.g. 20:00 - 04:00)
      publishAllowed = (curMinutes >= startMinutes) || (curMinutes < endMinutes);
    }
  } else {
    // Couldn't get time; be conservative and disallow publish
    publishAllowed = false;
    Serial.println("Time not available, skipping publish window check");
  }

  return publishAllowed;
}

/* ----------------- MQTT helpers ----------------- */
void tryConnectMQTT(){
  if(mqttClient.connected()){
    mqttConnected = true;
    return;
  }
  unsigned long now = millis();
  if(now - mqttLastConnectAttempt < mqttConnectMs) 
  {
    return;
  }
  
  mqttLastConnectAttempt = now;

  String clientId = String(DEVICE_ID) + "-" + String((uint32_t)random());
  Serial.print("Attempt MQTT connect...");
  if(mqttClient.connect(clientId.c_str())){
    Serial.println("MQTT connected");
    mqttConnected = true;
  } else {
    Serial.print("MQTT connect failed rc=");
    Serial.println(mqttClient.state());
    mqttConnected = false;
  }
}

/* ----------------- WiFi reconnect policy helper -----------------
   Behaviour: return true if the "reason" is considered reconnectable now (so we can call WiFi.reconnect()).
   If false, we'll defer reconnection attempts into the main loop by setting wifiReconnectRequested.
   This mirrors the concept of the _is_staReconnectableReason helper from the Arduino/esp32 repo.
   You can tune the reason list to your needs. */
// From https://github.dev/espressif/esp-idf/blob/master/components/esp_wifi/include/esp_wifi_types.h
typedef enum {
    WIFI_REASON_UNSPECIFIED                        = 1,     /**< Unspecified reason */
    WIFI_REASON_AUTH_EXPIRE                        = 2,     /**< Authentication expired */
    WIFI_REASON_AUTH_LEAVE                         = 3,     /**< Deauthentication due to leaving */
    WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY         = 4,     /**< Disassociated due to inactivity */
    WIFI_REASON_ASSOC_TOOMANY                      = 5,     /**< Too many associated stations */
    WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA      = 6,     /**< Class 2 frame received from nonauthenticated STA */
    WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA     = 7,     /**< Class 3 frame received from nonassociated STA */
    WIFI_REASON_ASSOC_LEAVE                        = 8,     /**< Deassociated due to leaving */
    WIFI_REASON_ASSOC_NOT_AUTHED                   = 9,     /**< Association but not authenticated */
    WIFI_REASON_DISASSOC_PWRCAP_BAD                = 10,    /**< Disassociated due to poor power capability */
    WIFI_REASON_DISASSOC_SUPCHAN_BAD               = 11,    /**< Disassociated due to unsupported channel */
    WIFI_REASON_BSS_TRANSITION_DISASSOC            = 12,    /**< Disassociated due to BSS transition */
    WIFI_REASON_IE_INVALID                         = 13,    /**< Invalid Information Element (IE) */
    WIFI_REASON_MIC_FAILURE                        = 14,    /**< MIC failure */
    WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT             = 15,    /**< 4-way handshake timeout */
    WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT           = 16,    /**< Group key update timeout */
    WIFI_REASON_IE_IN_4WAY_DIFFERS                 = 17,    /**< IE differs in 4-way handshake */
    WIFI_REASON_GROUP_CIPHER_INVALID               = 18,    /**< Invalid group cipher */
    WIFI_REASON_PAIRWISE_CIPHER_INVALID            = 19,    /**< Invalid pairwise cipher */
    WIFI_REASON_AKMP_INVALID                       = 20,    /**< Invalid AKMP */
    WIFI_REASON_UNSUPP_RSN_IE_VERSION              = 21,    /**< Unsupported RSN IE version */
    WIFI_REASON_INVALID_RSN_IE_CAP                 = 22,    /**< Invalid RSN IE capabilities */
    WIFI_REASON_802_1X_AUTH_FAILED                 = 23,    /**< 802.1X authentication failed */
    WIFI_REASON_CIPHER_SUITE_REJECTED              = 24,    /**< Cipher suite rejected */
    WIFI_REASON_TDLS_PEER_UNREACHABLE              = 25,    /**< TDLS peer unreachable */
    WIFI_REASON_TDLS_UNSPECIFIED                   = 26,    /**< TDLS unspecified */
    WIFI_REASON_SSP_REQUESTED_DISASSOC             = 27,    /**< SSP requested disassociation */
    WIFI_REASON_NO_SSP_ROAMING_AGREEMENT           = 28,    /**< No SSP roaming agreement */
    WIFI_REASON_BAD_CIPHER_OR_AKM                  = 29,    /**< Bad cipher or AKM */
    WIFI_REASON_NOT_AUTHORIZED_THIS_LOCATION       = 30,    /**< Not authorized in this location */
    WIFI_REASON_SERVICE_CHANGE_PERCLUDES_TS        = 31,    /**< Service change precludes TS */
    WIFI_REASON_UNSPECIFIED_QOS                    = 32,    /**< Unspecified QoS reason */
    WIFI_REASON_NOT_ENOUGH_BANDWIDTH               = 33,    /**< Not enough bandwidth */
    WIFI_REASON_MISSING_ACKS                       = 34,    /**< Missing ACKs */
    WIFI_REASON_EXCEEDED_TXOP                      = 35,    /**< Exceeded TXOP */
    WIFI_REASON_STA_LEAVING                        = 36,    /**< Station leaving */
    WIFI_REASON_END_BA                             = 37,    /**< End of Block Ack (BA) */
    WIFI_REASON_UNKNOWN_BA                         = 38,    /**< Unknown Block Ack (BA) */
    WIFI_REASON_TIMEOUT                            = 39,    /**< Timeout */
    WIFI_REASON_PEER_INITIATED                     = 46,    /**< Peer initiated disassociation */
    WIFI_REASON_AP_INITIATED                       = 47,    /**< AP initiated disassociation */
    WIFI_REASON_INVALID_FT_ACTION_FRAME_COUNT      = 48,    /**< Invalid FT action frame count */
    WIFI_REASON_INVALID_PMKID                      = 49,    /**< Invalid PMKID */
    WIFI_REASON_INVALID_MDE                        = 50,    /**< Invalid MDE */
    WIFI_REASON_INVALID_FTE                        = 51,    /**< Invalid FTE */
    WIFI_REASON_TRANSMISSION_LINK_ESTABLISH_FAILED = 67,    /**< Transmission link establishment failed */
    WIFI_REASON_ALTERATIVE_CHANNEL_OCCUPIED        = 68,    /**< Alternative channel occupied */

    WIFI_REASON_BEACON_TIMEOUT                     = 200,    /**< Beacon timeout */
    WIFI_REASON_NO_AP_FOUND                        = 201,    /**< No AP found */
    WIFI_REASON_AUTH_FAIL                          = 202,    /**< Authentication failed */
    WIFI_REASON_ASSOC_FAIL                         = 203,    /**< Association failed */
    WIFI_REASON_HANDSHAKE_TIMEOUT                  = 204,    /**< Handshake timeout */
    WIFI_REASON_CONNECTION_FAIL                    = 205,    /**< Connection failed */
    WIFI_REASON_AP_TSF_RESET                       = 206,    /**< AP TSF reset */
    WIFI_REASON_ROAMING                            = 207,    /**< Roaming */
    WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG       = 208,    /**< Association comeback time too long */
    WIFI_REASON_SA_QUERY_TIMEOUT                   = 209,    /**< SA query timeout */
    WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY  = 210,    /**< No AP found with compatible security */
    WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD  = 211,    /**< No AP found in auth mode threshold */
    WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD      = 212,    /**< No AP found in RSSI threshold */
} wifi_err_reason_t;

// From, https://github.dev/espressif/arduino-esp32/blob/master/libraries/WiFi/src/STA.cpp
bool isStaReconnectableReason(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_UNSPECIFIED:
    //Timeouts (retry)
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    //Transient error (reconnect)
    case WIFI_REASON_AUTH_LEAVE:
    case WIFI_REASON_ASSOC_EXPIRE:
    case WIFI_REASON_ASSOC_TOOMANY:
    case WIFI_REASON_NOT_AUTHED:
    case WIFI_REASON_NOT_ASSOCED:
    case WIFI_REASON_ASSOC_NOT_AUTHED:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
    case WIFI_REASON_INVALID_PMKID:
    case WIFI_REASON_BEACON_TIMEOUT:
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
    case WIFI_REASON_AP_TSF_RESET:
    case WIFI_REASON_ROAMING:            return true;
    default:                             return false;
  }
}

/* ----------------- WiFi event handler -----------------
   This handler runs in the WiFi-task context. Do minimal, non-blocking work here.
   If we can attempt an immediate reconnect we call WiFi.reconnect() (it returns immediately).
   Otherwise we set a flag and record reason for the main loop to handle.
*/
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info){
  switch(event){
    case SYSTEM_EVENT_STA_CONNECTED:
      Serial.println("[WiFi] STA connected");
      wifiConnected = true;
      wifiReconnectRequested = false; // clear any pending request
      break;

    case SYSTEM_EVENT_STA_DISCONNECTED: {
      uint32_t reason = info.wifi_sta_disconnected.reason;
      wifiConnected = false;
      mqttConnected = false; // mark MQTT disconnected, we'll reconnect later when WiFi is back
      Serial.printf("[WiFi] Disconnected, reason=%u\n", reason);

      bool IsReconnect = isStaReconnectableReason(reason);

      if(IsReconnect){
        // WIFI should reconnect automatically
      } else {
        // For non-reconnectable reasons, schedule reconnect attempts in main loop to avoid busy loops
        Serial.println("[WiFi] Non-reconnectable reason -> scheduling reconnect in main loop");
        wifiReconnectRequested = true;
        // reset backoff so main loop will wait initial interval before trying
        wifiLastReconnectAttempt = millis();
      }
      break;
    }

    case SYSTEM_EVENT_STA_GOT_IP:
      Serial.print("[WiFi] GOT IP: ");
      Serial.println(WiFi.localIP());
      wifiConnected = true;
      wifiReconnectRequested = false;
      break;

    default:
      // ignore other events
      break;
  }
}

void publishTelemetry(){
  String dateStr = "1970-01-01";
  String timeStr = "00:00:00";

  // Get time
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return;
  }

  dateStr = String(timeinfo.tm_year + 1900) + "-" + String(timeinfo.tm_mon + 1) + "-" + String(timeinfo.tm_mday);
  timeStr = String(timeinfo.tm_hour) + ":" + String(timeinfo.tm_min) + ":" + String(timeinfo.tm_sec);

  // Get GPS
  double lon = random(-180, 180);
  double lat = random(-90, 90);
  // Get battery
  int batt = random(0, 100);

  // Publish
  StaticJsonDocument<256> doc;
  doc["id"] = DEVICE_ID;
  doc["payload"] = make_payload_hex(lon, lat, batt);
  doc["date"] = dateStr;
  doc["time"] = timeStr;
  String s;
  size_t n = serializeJson(doc, s);

  snprintf(topic, sizeof(topic), TOPIC_FMT, DEVICE_ID);
  mqttClient.publish(topic, buffer, n);
  Serial.print("Published: ");
  Serial.println(buffer);
}

/* ----------------- connect functions ----------------- */
void connectWiFi(){
  // start connection
  if(WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[WiFi] start connect to \"%s\"\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void tryReconnectWiFiFromLoop(){
  unsigned long now = millis();
  
  // Check period to reconnect
  if(now - wifiLastReconnectAttempt < wifiReconnectMs) 
  {
    return;
  }
  
  wifiLastReconnectAttempt = now;

  Serial.printf("[WiFi] Loop: attempting reconnect\n");

  WiFi.disconnect(true); // clear any stale state
  delay(10); // tiny yield
  // Do a non-blocking begin() to trigger connection attempts in WiFi task
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

/* ----------------- setup/loop ----------------- */

void setup(){
  Serial.begin(115200);
  delay(100);

  // GPS serial

  // MQTT client
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);

  // Register WiFi event handler (must be done before connect)
  WiFi.onEvent(onWiFiEvent);

  // Start WiFi
  connectWiFi();

  // Config the time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

unsigned long lastPublish = 0;
const unsigned long PUBLISH_INTERVAL_MS = 60 * 60 * 1000; // 1 hour

void loop()
{
    // Always process incoming serial GPS data

    // If wifi reconnect was requested by event handler, let main-loop do controlled attempts
    if (wifiReconnectRequested && !wifiConnected)
    {
        tryReconnectWiFiFromLoop();
    }

    // If WiFi is up but MQTT is not, attempt to connect MQTT (non-blocking attempts with backoff)
    if (WiFi.status() == WL_CONNECTED && !mqttClient.connected())
    {
        tryConnectMQTT();
    }

    // If MQTT connected, call loop() to keep connection alive
    if (mqttClient.connected())
    {
        mqttClient.loop();
    }

    // Publish telemetry periodically (example)
    unsigned long now = millis();
    bool publishAllowed = checkPublishPeriod();

    // Only update lastPublish and attempt actual publish when inside allowed window
    if (now - lastPublish >= PUBLISH_INTERVAL_MS)
    {
        if (!publishAllowed)
        {
            Serial.println("Outside allowed publish window, skipping publish");
        }
        else
        {
            lastPublish = now;
            if (mqttClient.connected())
            {
                publishTelemetry();
            }
            else
            {
                Serial.println("MQTT not connected, skipping publish");
            }
        }
    }

    // small yield to allow other tasks to run
    delay(10);
}