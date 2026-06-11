#include <Arduino.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <time.h>

#include "ble_server.h"
#include "esp_bt.h"
#include <Keypad.h>

// --- Pins (As specified) ---
#define LOCK_PIN 7
#define SBC_PWR_PIN 8
#define BATTERY_PIN 6
#define PIR_PIN 4
#define BUTTON_PIN 5
#define T_IRQ 21        // Touch Interrupt
#define TFT_LED 13      // TFT Backlight
#define RGB_LED_PIN 48  // RGB LED data pin for ESP32-S3 DevKitM
#define NUM_LEDS 1
#define BRIGHTNESS 40
CRGB leds[NUM_LEDS];

// Display PINS initialized in User_Setup.h of TFT_eSPI Library

// --- Configuration & Credentials ---
#define LOCK_ID "c0ffee01-1234-4abc-9def-9876543210aa"  // Unique Lock Identifier UUIDv7
#define SIMPLE_ID "coff"                                // Simple ID (first 4 characters of LOCK_ID)
#define LOCK_MODEL "JUPY Lock Pro"                      // Lock Model
#define FIRMWARE_VERSION "v1.0"                         // Firmware version
#define PAIRING_CODE "123456"                           // Lock Pairing Code
#define RESET_PIN "01455489"               // Reset PIN for factory reset/delete lock via REST API/touch pad
#define BACKUP_PIN "01455489"              // Dynamic Backup PIN that changes after use via REST API
#define AUTH_DISABLE_TIME 30 * 60000UL     // 30 minutes
#define BATT_CHECK_TIMEOUT 5 * 60000ULL    // 5 minutes
#define COMMISSION_TIME 10 * 60000UL       // 10 minutes
#define MQTT_ACTIVE_TIMEOUT 2 * 60000UL    // 2 minutes
#define SBC_POWEROFF_DELAY 2000UL          // 2s grace period for Pi cleanup before GPIO LOW
#define SBC_MAX_RUNTIME 6 * 1000UL         // 6s max runtime for SBC before auto power-off
#define MAX_NOTIF 10                       // Max notifications to store offline
#define DEF_NOTIF_NAME "Manual Operation"  // Default notification name

// --- Remote MQTT (HiveMQ Cloud) ---
const char *mqtt_server = "72300a81dfec465f840ffafaa812a8bb.s1.eu.hivemq.cloud";
const char *mqtt_user = "jupyLock";
const char *mqtt_pwd = "jupyLock012";

// --- Local MQTT broker (Raspberry Pi 5 / jupy_bridge.local) ---
// Uses same credentials as HiveMQ for consistency (Mosquitto configured with password auth)
const char *local_mqtt_host = "jupy-bridge.local";
const uint16_t local_mqtt_port = 1883;
const char *local_mqtt_user = "jupyLock";
const char *local_mqtt_pwd = "jupyLock012";

// --- Local MQTT Topics ---
// ESP32 → Pi
#define TOPIC_SBC_CMD                                                                                                  \
  "jupy/sbc/cmd"  // Commands to SBC: on, capture_face, doorbell_chime, start_call, update_settings, commission,
                  // power_off
#define TOPIC_SETTINGS "jupy/settings"  // Full settings JSON push → Pi writes to settings.json
// Pi → ESP32
#define TOPIC_SBC_STATUS "jupy/sbc/status"  // Status responses from SBC: match, intruder, no_face, awake, error, etc.

// --- Supabase ---
const char *projectId = "ienqcmbfdobzcggkhajc";
const String api_base_endpoint = "https://" + String(projectId) + ".supabase.co/functions/v1/make-server-a213de84";
// ("http://" + String(laptop_ip) + ":3000/api/lock/register").c_str(); //local dev
const String analytics_endpoint = api_base_endpoint + "/analytics";
const String notification_endpoint = api_base_endpoint + "/event/" + LOCK_ID;
const String register_lock_endpoint = api_base_endpoint + "/locks/register";
const String lock_endpoint = api_base_endpoint + "/lock/" + LOCK_ID;

String touchpad_keys[4][3] = {
    {"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}, {"x", "0", "#"}  // X: Clear, B: Bell/Enter
};

// Keypad configuration
const byte ROWS = 4;
const byte COLS = 4;
char keypad_keys[ROWS][COLS] = {{'1', '2', '3', ' '}, {'4', '5', '6', ' '}, {'7', '8', '9', ' '}, {'x', '0', '#', ' '}};
byte rowPins[ROWS] = {13, 12, 11, 10};
byte colPins[COLS] = {18, 17, 16, 15};
// Remapped pinout for telephone style-membrane switch 3x4 Keypad {C1, C2, C3, C4, R1, R2, R3, R4}=>{C2, R1, C1, R4, C3,
// R3, R2}
// byte rowPins[ROWS] = {16, 12, 11, 18};
// byte colPins[COLS] = {17, 15, 10};

Keypad keypad = Keypad(makeKeymap(keypad_keys), rowPins, colPins, ROWS, COLS);

/****** root certificate (for HiveMQ TLS) *********/
static const char *root_ca PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

// --- Client Instances ---
TFT_eSPI tft = TFT_eSPI();

// Remote MQTT (HiveMQ Cloud) — TLS, for app commands and online presence
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

// Local MQTT (Mosquitto on Pi) — plaintext, for SBC command/response
WiFiClient localEspClient;
PubSubClient localMqttClient(localEspClient);

WebServer localServer(80);
Preferences prefs;
BLECommissioningServer bleServer;

// --- Stored Variables ---
String LOCK_NAME = "";
String OWNER_NAME = "";
String USER_ID = "";
String REFRESH_TOKEN = "";

boolean FORCED_RESET_LOCK = false;  // Warning: Changing this to true will erase all user data

// --- State Management ---
unsigned long authTimeout = 0;
unsigned long bootTime = 0;
unsigned long commissionTimeout = 0;
unsigned long faceUnlockTimeout = 0;
unsigned long lastActivity = 0;
unsigned long sbcStartTime = 0;
unsigned long sbcUpTime = 0;
unsigned long lastBatCheck = 0;
unsigned long sbcPowerOffAt = 0;  // Non-zero = GPIO power-off pending after grace delay
std::vector<unsigned long> touchFadeTime(12, 0);

std::vector<int> lastTouchX(12, -1);
std::vector<int> lastTouchY(12, -1);
std::vector<int> lastTouchColor(12, TFT_BLACK);
uint8_t unlockAttempts = 0;

bool mqttActive = false;
bool localMqttConnected = false;
unsigned long lastLocalMqttRetry = 0;
unsigned long lastMqttRetry = 0;
bool sbcIsRunning = false;
bool longSBCActivity = false;
bool pinManuallyEntered = false;
bool share_analytics = false;
bool notify_all_motion = false;
bool sending_pending_notif = false;

uint8_t pendingNotificationsCount = 0;
uint8_t head = 0;
uint8_t tail = 0;
uint8_t intruder = 0;
uint8_t authFail = 0;
String passcodeBuffer = "";
std::array<String[3], 10> pendingNotifications;

// Function Prototypes
void handlePIR();
void handleTouch();
void handleKeypad();
void processKeyPress(int row, int col, int x, int y);
void handleTimeouts();
void monitorBattery(const bool startupCheck = false);
void initialCommissioning();
void connectToWifi(const String &ssid, const String &password);
void sendToSBC(String command);
void SBCPowerOff();
void reconnectLocalMQTT();
void localMqttCallback(char *topic, byte *payload, unsigned int length);
void handleLocalMqttMsg(const String &status, JsonDocument &doc);

bool checkPin(const char *);
void touch_calibrate();
void unlockDoor(String, String);
void drawKeypad();
void notify(String, String, String name = DEF_NOTIF_NAME);
void setupREST();
bool setupPin();
void serverLog(String, String);
void setOnline(bool);
void mqttCallback(char *, byte *, unsigned int);
void reconnectMQTT();
void startMQTTSession();
void endMQTTSession();
String getCurrentTimestamp();
void startSleepMode(unsigned long milli_sec = 0);

struct HTTPResponse {
  int code;
  const char *contentType;
  String body;
};

HTTPResponse handleApiRequest(const String &endpoint, String body, String token = "", HTTPMethod method = HTTP_POST);

void wakeUpReason() {
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.println("Woke up from PIR or Button!");
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Woke up from Touch!");
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Woke up from Timer!");
  } else {
    Serial.println("Natural first wakeup. Not waking from deep sleep");
  }
}

CRGB DEF_LED_COLOR = CRGB::Red;
CRGB CURRENT_LED_COLOR = DEF_LED_COLOR;
CRGB KEY_PRESSED_COLOR = CRGB::Green;

void setLEDColor(uint8_t r, uint8_t g, uint8_t b) {
  leds[0] = CRGB(r, g, b);
  FastLED.show();
  CURRENT_LED_COLOR = CRGB(r, g, b);
}

void setLEDColor(CRGB color) {
  leds[0] = color;
  FastLED.show();
  CURRENT_LED_COLOR = color;
}

void resetLEDColor() {
  CURRENT_LED_COLOR = DEF_LED_COLOR;
  setLEDColor(CURRENT_LED_COLOR);
}

void setup() {
  Serial.begin(115200);
  wakeUpReason();

  // Configure screen
  Serial.println("TFT Calibrating ...");
  pinMode(TFT_CS, OUTPUT);
  pinMode(TOUCH_CS, OUTPUT);
  pinMode(T_IRQ, INPUT_PULLUP);
  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TOUCH_CS, HIGH);
  digitalWrite(TFT_LED, HIGH);

  tft.init();
  tft.setRotation(0);
  uint16_t calData[5] = {309, 3472, 357, 3449, 4};
  tft.setTouch(calData);

  pinMode(PIR_PIN, INPUT_PULLDOWN);
  pinMode(LOCK_PIN, OUTPUT);
  pinMode(SBC_PWR_PIN, OUTPUT);
  pinMode(BATTERY_PIN, INPUT_PULLDOWN);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  FastLED.addLeds<NEOPIXEL, RGB_LED_PIN>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);

  digitalWrite(LOCK_PIN, LOW);  // Fail-secure: LOW keeps locked
  digitalWrite(SBC_PWR_PIN, LOW);

  setLEDColor(CRGB::Lavender);

  Serial.println("Booting up... Turning on SBC & Unlock for 2 seconds");
  digitalWrite(SBC_PWR_PIN, HIGH);
  delay(2000);
  digitalWrite(SBC_PWR_PIN, LOW);
  digitalWrite(LOCK_PIN, HIGH);
  delay(2000);
  digitalWrite(LOCK_PIN, LOW);

  Serial.print("Touch CS pin: " + String(TOUCH_CS));
  Serial.printf("TFT SPI Frequency: %d Hz\n", SPI_FREQUENCY);
  Serial.printf("TFT SPI PINS: MISO: %d, MOSI: %d, SCLK: %d, CS: %d, DC: %d, RST: %d, T_CS: %d, T_IRQ: %d\n", TFT_MISO,
                TFT_MOSI, TFT_SCLK, TFT_CS, TFT_DC, TFT_RST, TOUCH_CS, T_IRQ);

  // 0. Initialize Storage
  prefs.begin("my_storage", false);
  prefs.putString("pairing_code", PAIRING_CODE);
  prefs.putString("refresh_token", "refresh_token");
  Serial.println("Stored pin: " + prefs.getString("pin"));
  notify_all_motion = prefs.getBool("notify_all_motion", notify_all_motion);
  share_analytics = prefs.getBool("share_analytics", share_analytics);

  if (prefs.getString("settings").isEmpty()) {
    JsonDocument defaultSettings;
    defaultSettings["cam_resolution"] = 1024;
    defaultSettings["call_timeout"] = 40;
    defaultSettings["snippet_time"] = 15;
    defaultSettings["record_motion"] = true;
    defaultSettings["record_person"] = true;
    defaultSettings["notify_all_motion"] = notify_all_motion;
    defaultSettings["share_analytics"] = share_analytics;
    String defaultSettingsStr;
    serializeJson(defaultSettings, defaultSettingsStr);
    prefs.putString("settings", defaultSettingsStr);
  }

  // 1. BLE Provisioning & Transition
  Serial.println("Check for commissioning");
  initialCommissioning();

  // 2. Local REST API
  Serial.println("Setup Rest Server");
  setupREST();

  // 3. SNTP, Battery, RESET mode
  monitorBattery();
  configTime(0, 0, "pool.ntp.org");

  FORCED_RESET_LOCK = prefs.getBool("forced_reset", false);
  if (FORCED_RESET_LOCK) {
    Serial.println("Awaiting reset complete from user... MQTT will remain disabled.");
    return;
  }

  // 4. Remote MQTT (HiveMQ)
  espClient.setCACert(root_ca);
  mqttClient.setServer(mqtt_server, 8883);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024);
  if (WiFi.status() == WL_CONNECTED) {
    mqttActive = true;
    reconnectMQTT();
  }

  // 5. Local MQTT (Mosquitto on SBC)
  localMqttClient.setServer(local_mqtt_host, local_mqtt_port);
  localMqttClient.setCallback(localMqttCallback);
  localMqttClient.setBufferSize(1024);
  if (WiFi.status() == WL_CONNECTED) {
    reconnectLocalMQTT();
  }

  Serial.print("Device Setup Complete. ");
  Serial.println(getCurrentTimestamp());
  drawKeypad();
  Serial.println("===========================\n");
  resetLEDColor();
}

void loop() {
  handleTimeouts();

  if (digitalRead(BUTTON_PIN) == LOW) {
    unlockDoor("Manual Operation", "Button");
  }

  handlePIR();
  handleKeypad();
  handleTouch();

  // Fade touch feedback
  if (!touchFadeTime.empty() && millis() > touchFadeTime.front()) {
    if (lastTouchX.front() != -1 && lastTouchY.front() != -1) {
      tft.fillCircle(lastTouchX.front(), lastTouchY.front(), 5, lastTouchColor.front());
    }

    // Shift all fade times and touch coordinates left, unshift first element
    touchFadeTime.erase(touchFadeTime.begin());
    lastTouchX.erase(lastTouchX.begin());
    lastTouchY.erase(lastTouchY.begin());
    lastTouchColor.erase(lastTouchColor.begin());
  }

  monitorBattery();
  localServer.handleClient();

  // Remote MQTT loop
  if (mqttActive) {
    if (!mqttClient.connected()) {
      reconnectMQTT();
    }
    mqttClient.loop();
  }

  // Local MQTT loop — always attempt to stay connected when WiFi is up
  if (WiFi.status() == WL_CONNECTED) {
    if (!localMqttClient.connected()) {
      reconnectLocalMQTT();
    }
    localMqttClient.loop();
  }

  // Deferred GPIO power-off: fire after grace delay so Pi can finish cleanup
  if (sbcPowerOffAt > 0 && millis() >= sbcPowerOffAt) {
    sbcPowerOffAt = 0;
    digitalWrite(SBC_PWR_PIN, LOW);
    Serial.println("[SBC] GPIO power rail LOW (deferred).");
  }
}

// --- CORE LOGIC FUNCTIONS ---

void handlePIR() {
  if (digitalRead(PIR_PIN) == HIGH && !sbcIsRunning) {
    delay(50);  // Debounce
    Serial.println("[PIR] Motion Detected! Waking up Vision System...");
    startMQTTSession();
    sendToSBC("{\"cmd\":\"on\"}");
  }
}

void sendToSBC(String command) {
  // Power on the Pi GPIO rail (still used for hardware power control)
  digitalWrite(SBC_PWR_PIN, HIGH);

  // Augment command with face_timeout if relevant
  if (faceUnlockTimeout) {
    command.replace("}", ", \"face_timeout\":true }");
  }

  sbcStartTime = millis();
  sbcIsRunning = true;

  if (localMqttClient.connected()) {
    bool ok = localMqttClient.publish(TOPIC_SBC_CMD, command.c_str());
    Serial.printf("[LOCAL MQTT] → %s : %s [%s]\n", TOPIC_SBC_CMD, command.c_str(), ok ? "ok" : "FAIL TO SEND");
  } else {
    // Broker not reachable — log and attempt reconnect next loop tick
    Serial.printf("[LOCAL MQTT] Not connected. Dropping SBC command: %s\n", command.c_str());
    serverLog("sbc_cmd_dropped", "{\"cmd\":" + command + "}");
  }
}

// SBCPowerOff: sends power_off command to Pi for cleanup, then schedules
// GPIO LOW after SBC_POWEROFF_DELAY ms to give the Pi time to respond.
void SBCPowerOff() {
  if (localMqttClient.connected()) {
    String offCmd = "{\"cmd\":\"power_off\", \"uptime\":" + String(sbcUpTime / 1000) + "}";
    localMqttClient.publish(TOPIC_SBC_CMD, offCmd.c_str());
    Serial.println("[LOCAL MQTT] → power_off command sent to SBC.");
  }

  // Schedule GPIO LOW after grace period
  sbcPowerOffAt = millis() + SBC_POWEROFF_DELAY;

  sbcIsRunning = false;
  longSBCActivity = false;
  serverLog("sbc_power_off", "{\"uptime\":\"" + String(sbcUpTime / 1000) + "\"}");
  sbcUpTime = 0;
  Serial.println("[SBC] Power-off scheduled (GPIO LOW in " + String(SBC_POWEROFF_DELAY) + "ms).");
}

// localMqttCallback: called by PubSubClient when a message arrives on a subscribed local topic.
// This is the direct replacement for handleUART() — same status branch logic, no behavioral changes.
void localMqttCallback(char *topic, byte *payload, unsigned int length) {
  byte buffer[length + 1];
  memcpy(buffer, payload, length);
  buffer[length] = '\0';

  String topicStr = String(topic);
  Serial.printf("[LOCAL MQTT] ← %s : %s\n", topic, (char *)buffer);

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, buffer);
  if (error) {
    Serial.printf("[LOCAL MQTT] JSON parse error on topic %s: %s\n", topic, error.c_str());
    return;
  }

  if (topicStr == TOPIC_SBC_STATUS) {
    handleLocalMqttMsg(doc["status"].as<String>(), doc);
  }
}

String sanitizeJsonToStr(const String &msg) {
  String cleaned = msg;
  if (cleaned.isEmpty()) return msg;
  cleaned.replace("\n", "\\n");
  cleaned.replace("\r", "\\r");
  cleaned.replace("\"", "\\\"");
  if (cleaned.isEmpty()) return "\"\"";
  return cleaned;
}

// handleLocalMqttMsg: extracted from the old handleUART() body — unchanged logic.
void handleLocalMqttMsg(const String &status, JsonDocument &doc) {
  lastActivity = millis();

  if (status == "match") {
    unlockDoor(doc["name"], "Face");
    String name = doc["name"].as<String>();
    notify("Lock Status", "Unlocked by " + name + " (Face)", doc["name"].as<String>());
    serverLog("unlock", "{\"method\":\"face\", \"success\":\"true\", \"name\":\"" + doc["name"].as<String>() + "\"}");
    SBCPowerOff();

  } else if (status == "intruder") {
    notify("Intruder Alert!", "Unknown face detected at door.");
    serverLog("unlock", "{\"method\":\"face\", \"success\":\"false\"}");
    intruder += 1;
    if (intruder <= 3) {
      sbcUpTime += millis() - sbcStartTime;
      sbcStartTime = millis();
    } else {
      faceUnlockTimeout = millis();
      SBCPowerOff();
    }
  } else if (status == "no_face") {
    Serial.println("[SBC] No face detected.");
    if (notify_all_motion) notify("Motion Detected", "Motion detected but no face found.");
  } else if (status == "awake") {
    bootTime = (millis() - sbcStartTime);
    serverLog("boot", "{\"bootTime\":\"" + String(float(bootTime) / 1000.0, 4) + "\"}");
  } else if (status == "call_active" || status == "long_task_active") {
    longSBCActivity = true;
    sbcIsRunning = true;
  } else if (status == "call_ended" || status == "long_task_ended") {
    longSBCActivity = false;
    SBCPowerOff();
  } else if (status == "call_log") {
    String log;
    serializeJson(doc["log"].as<JsonArray>(), log);
    serverLog("call",
              "{\"call_duration\":" + String(doc["call_duration"].as<int>()) + ",\"call_log\":\"" + log + "\"}");
  } else if (status == "error") {
    String error = doc["error"].as<String>();
    if (error.equals("0")) {
      Serial.println("[SBC] Received generic success code. Last command executed/stopped successfully.");
      return;
    }
    notify("SBC Error", sanitizeJsonToStr(error), "SBC");
    serverLog("sbc_error", "{\"error\":\"" + error + "\"}");
    longSBCActivity = false;
  } else if (status == "commission_success") {
    Serial.println("[SBC] Commissioning SBC successful!");

    if (bleServer.isConnected()) {
      bleServer.sendResponse("{\"status\": \"sbc_commission_success\"}");
    }

    String payload = "{\"sbc_ip_address\":\"" + doc["sbc_ip_address"].as<String>() + "\",\"sbc_hostname\":\"" +
                     doc["sbc_hostname"].as<String>() + "\"}";

    handleApiRequest(lock_endpoint, payload.c_str(), "", HTTP_PATCH);
  } else {
    Serial.println("[SBC] Unknown status: " + status);
  }
}

// reconnectLocalMQTT: mirrors the reconnectMQTT() pattern with non-blocking 5s retry.
void reconnectLocalMQTT() {
  if (localMqttClient.connected()) return;

  unsigned long now = millis();
  if (lastLocalMqttRetry != 0 && now - lastLocalMqttRetry < 5000) return;
  lastLocalMqttRetry = now;

  String clientId = String(LOCK_MODEL) + " (local) " + String(SIMPLE_ID);
  Serial.printf("[LOCAL MQTT] Connecting to %s:%d as %s ...\n", local_mqtt_host, local_mqtt_port, clientId.c_str());

  if (localMqttClient.connect(clientId.c_str(), local_mqtt_user, local_mqtt_pwd)) {
    Serial.println("[LOCAL MQTT] Connected.");
    localMqttClient.subscribe(TOPIC_SBC_STATUS, 1);
    localMqttConnected = true;
    Serial.printf("[LOCAL MQTT] Subscribed to %s\n", TOPIC_SBC_STATUS);
  } else {
    localMqttConnected = false;
    Serial.printf("[LOCAL MQTT] Connection failed, rc=%d — will retry in 5s\n", localMqttClient.state());
  }
}

void startSleepMode(unsigned long milli_sec) {
  if (mqttActive) {
    endMQTTSession();
  }

  // Inform Pi before going to sleep
  if (localMqttClient.connected()) {
    localMqttClient.publish(TOPIC_SBC_CMD, "{\"cmd\":\"power_off\", \"reason\":\"sleep\"}");
    delay(200);  // Brief flush
    localMqttClient.disconnect();
  }

  esp_wifi_stop();
  esp_wifi_deinit();
  btStop();
  esp_bt_controller_disable();
  esp_bt_controller_deinit();

  Serial.println("Radios gracefully shut down.");
  leds[0] = CRGB::Blue;
  FastLED.show();
  digitalWrite(TFT_LED, LOW);
  digitalWrite(SBC_PWR_PIN, LOW);  // Hard off on sleep, Pi already notified

  uint64_t high_pin_mask = (1ULL << BUTTON_PIN);
  esp_sleep_enable_ext1_wakeup(high_pin_mask, ESP_EXT1_WAKEUP_ANY_LOW);
  // esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);  // Wake on LOW (button press)
  if (milli_sec > 0) esp_sleep_enable_timer_wakeup(milli_sec * 1000ULL);
  else if (milli_sec == 0)
    esp_sleep_enable_timer_wakeup(1 * 24 * 60 * 60 * 1000ULL);  // Sleep for a day if no timer specified

  Serial.println("Entering Deep Sleep now...");

  Serial.flush();  // Ensure flush completes
  esp_deep_sleep_start();
}

void display_reset_message(uint8_t presses) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(0, 0);
  tft.println("Press Open Door Button Twice");
  tft.println("To Confirm Reset");
  tft.print("Pressed: ");
  tft.println(String(presses));
}

void display_good_bye_wave() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.println("'Waving' Goodbye!");
  for (int i = 0; i < 4; i++) {
    leds[0] = CRGB::Blue;
    FastLED.show();
    delay(500);
    leds[0] = CRGB::DarkMagenta;
    FastLED.show();
  }
  tft.println("Thanks for using JUPY Locks!");
  delay(2000);
}

void handleTimeouts() {
  // FORCED LOCK RESET
  if (FORCED_RESET_LOCK) {
    setLEDColor(CRGB::Yellow);
    Serial.println("[FORCED RESET]: Resetting lock...");
    uint8_t button_presses = 0;
    // TODO: Add require button presses to continue reset (physical intervention to mitigate remote hacks for security)
    display_reset_message(button_presses);

    HTTPResponse response =
        handleApiRequest(lock_endpoint, "{\"user_id\":\"" + String(USER_ID) + "\"}", "", HTTP_DELETE);
    response.code = 200;  // Mock success response for testing without backend
    if (response.code == 200) {
      display_good_bye_wave();
      Serial.println("FORCED RESET: Clearing user data...");
      prefs.clear();
      FORCED_RESET_LOCK = false;
      Serial.println("FORCED RESET: User data cleared.");
      startSleepMode(2000);
    } else {
      notify("Reset Failed",
             "Unable to reset lock. Please try again.\nConnection Failed. Error: " + String(response.body), "System");
      Serial.println("FORCED RESET: Failed. Code: " + String(response.code) + ", body: " + response.body);
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_RED);
      tft.setTextSize(1);
      tft.setCursor(0, 0);
      tft.println("Reset Failed!");
      tft.println("Please try again later.");
      delay(3000);
      FORCED_RESET_LOCK = true;
      startSleepMode(2000);
    }
  }

  // Auth timeout
  if (authTimeout) {
    if (millis() >= authTimeout + AUTH_DISABLE_TIME) {
      authTimeout = 0;
      authFail = 0;
    }
  }

  if (mqttActive) {
    if (millis() - lastActivity > MQTT_ACTIVE_TIMEOUT) {
      endMQTTSession();
    }
  }

  // SBC timeout (unchanged logic)
  if (sbcIsRunning && !longSBCActivity && (millis() - sbcStartTime > SBC_MAX_RUNTIME)) {
    Serial.println("SBC Timeout: No face detected. Powering down.");
    SBCPowerOff();
  }

  if (faceUnlockTimeout && pinManuallyEntered) {
    faceUnlockTimeout = 0;
    intruder = 0;
    pinManuallyEntered = false;
  }

  if (!sbcIsRunning && (millis() - lastActivity > 3 * 60 * 1000)) {
    Serial.println("Inactivity Timeout (3mins): Entering deep sleep.");
    startSleepMode();
  }
}

void unlockDoor(String name, String method) {
  lastActivity = millis();
  if (mqttActive) {
    Serial.println("[MQTT] Publishing door open status to MQTT...");
    mqttClient.publish(
        (String(LOCK_ID) + "/lock/status").c_str(),
        ("{\"event\":\"unlock\", \"open\":\"true\", \"timestamp\":\"" + getCurrentTimestamp() + "\"}").c_str());
  }
  setLEDColor(CRGB::Green);
  digitalWrite(LOCK_PIN, HIGH);
  delay(3000);
  digitalWrite(LOCK_PIN, LOW);
  resetLEDColor();
  Serial.println("[DOOR] Door Unlocked by " + name + " using " + method);
  if (mqttActive) {
    Serial.println("[MQTT] Publishing door locked status to MQTT...");
    mqttClient.publish((String(LOCK_ID) + "/lock/status").c_str(),
                       ("unlock\", \"open\":\"false\", \"timestamp\":\"" + getCurrentTimestamp() + "\"}").c_str());
  }
  notify("Lock Status", "Unlocked by " + name + " (" + method + ")", name);
  if (share_analytics) serverLog("unlock", "{\"method\":\"" + method + "\", \"name\":\"" + name + "\"}");
}

bool checkPin(const char *passCode) {
  String pin = prefs.getString("pin");
  if (pin.equals("")) {
    Serial.println("No pin code is set");
    return true;
  }
  return pin.equals(passCode);
}

uint8_t getBatteryLevel() {
  int raw = analogRead(BATTERY_PIN);
  int R1 = 470000;
  int R2 = 36000;
  float scaleV = ((float)R2 / (float)(R1 + R2));
  float sumVolt = 0;
  for (uint8_t i = 0; i < 10; i++) {
    float voltage = (raw / 1024.0) * 3.3;
    sumVolt += voltage / scaleV;
  }
  float voltage = (sumVolt / 10);
  int scaled = (int)(voltage * 100);
  switch (scaled) {
    case 1260 ... 1300: return 100;
    case 1250: return 90;
    case 1242: return 80;
    case 1232: return 70;
    case 1220: return 60;
    case 1206: return 50;
    case 1190: return 40;
    case 1175: return 30;
    case 1158: return 20;
    case 1131: return 10;
    case 1050: return 0;
    default: return 0;
  }
}

void monitorBattery(const bool startupCheck) {
  if (millis() - lastBatCheck > BATT_CHECK_TIMEOUT || startupCheck) {
    uint8_t batLevel = getBatteryLevel();
    switch (batLevel) {
      case 20:
        notify("Low Battery", "Battery: 20%");
        notify("Low Battery", "Warning: Battery Low. Charge battery soon.");
        break;
      case 10:
        notify("Low Battery", "Battery: 10%");
        notify("Low Battery", "Warning: Battery Low. Charge battery.");
        break;
      case 0: notify("Low Battery", "Warning: Battery depleted. Recharge Now!"); break;
      default: notify("Lock Battery", "Battery at " + String(batLevel) + "%");
    }
    lastBatCheck = millis();
    handleApiRequest(lock_endpoint, "{\"battery\":" + String(batLevel) + "}", "", HTTP_PATCH);
  }
}

// --- NOTIFICATIONS & CONNECTIVITY ---

const char *resolveMethod(HTTPMethod method) {
  switch (method) {
    case HTTP_GET: return "GET";
    case HTTP_POST: return "POST";
    case HTTP_DELETE: return "DELETE";
    case HTTP_PUT: return "PUT";
    case HTTP_PATCH: return "PATCH";
    case HTTP_OPTIONS: return "OPTIONS";
    default: return "UNKNOWN";
  }
}

HTTPResponse handleApiRequest(const String &endpoint, String payload, String token, HTTPMethod method) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[API CALL] Failed @%s: Not connected to WiFi",
                  endpoint.substring(api_base_endpoint.length()).c_str());
    return {-1, "", ""};
  }

  HTTPClient http;
  http.begin(endpoint);
  http.addHeader("Content-Type", "application/json");
  if (!token.isEmpty()) {
    http.addHeader("Authorization", "Bearer " + token);
  } else {
    http.addHeader("Authorization", "Bearer " + REFRESH_TOKEN);
  }
  String trimmed_endpoint = endpoint.substring(api_base_endpoint.length());
  trimmed_endpoint.replace(LOCK_ID, "[LOCK_ID]");
  Serial.printf("[API CALL] Endpoint: %s - Method: %s\n[API PAYLOAD] %s\n", trimmed_endpoint.c_str(),
                resolveMethod(method), payload.c_str());

  int httpResponseCode = 0;
  switch (method) {
    case HTTP_GET: {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);
      String queryEndpoint = endpoint + "?";
      if (!error) {
        for (JsonPair pair : doc.as<JsonObject>()) {
          queryEndpoint += String(pair.key().c_str()) + "=" + String(pair.value().as<String>().c_str()) + "&";
        }
      }
      httpResponseCode = http.GET();
      break;
    }
    case HTTP_POST: httpResponseCode = http.POST(payload); break;
    case HTTP_PATCH: httpResponseCode = http.PATCH(payload); break;
    case HTTP_PUT: httpResponseCode = http.PUT(payload); break;
    case HTTP_DELETE: httpResponseCode = http.sendRequest("DELETE", payload); break;
  }

  String response = "";
  if (httpResponseCode > 0) {
    response = http.getString();
    if (method == HTTP_PATCH) {
      int start = response.indexOf(payload.substring(1, 10));
      response = "Updated: " + response.substring(start, start + payload.length());
    }
    Serial.printf("[API RESPONSE] Code: %d\n[API RESPONSE] %s%s\n", httpResponseCode, response.c_str(),
                  (response.length() >= 100 ? "..." : ""));
  } else {
    Serial.printf("[HTTP] Request failed, error: %s\n", http.errorToString(httpResponseCode).c_str());
  }
  String content_type = http.header("Content-Type");
  http.end();

  return {httpResponseCode, content_type.c_str(), response};
}

String getCurrentTimestamp() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return String(millis());
  }
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buffer);
}

void notify(String title, String body, String name) {
  if (WiFi.status() != WL_CONNECTED) {
    if (head == tail && pendingNotificationsCount == MAX_NOTIF) {
      head = (head + 1) % MAX_NOTIF;
      pendingNotificationsCount--;
    }

    pendingNotifications[tail][0] = getCurrentTimestamp();
    pendingNotifications[tail][1] = title;
    pendingNotifications[tail][2] = body;
    tail = (tail + 1) % MAX_NOTIF;
    pendingNotificationsCount++;
    return;
  }

  if (pendingNotificationsCount > 0 && !sending_pending_notif) {
    Serial.println("Send " + String(pendingNotificationsCount) + " backlog pending notifications");
    for (uint8_t i = 0; i < pendingNotificationsCount; i++) {
      String timestamp = pendingNotifications[(head + i) % MAX_NOTIF][0];
      String title = pendingNotifications[(head + i) % MAX_NOTIF][1];
      String body = pendingNotifications[(head + i) % MAX_NOTIF][2];
      String payload = "{\"user_id\":\"" + USER_ID + "\",\"lock_id\":\"" + LOCK_ID + "\",\"name\":\"" + name +
                       "\",\"timestamp\":\"" + timestamp + "\",\"title\":\"" + title + "\",\"body\":\"" + body + "\"}";
      handleApiRequest(notification_endpoint, payload);
      head = (head + 1) % MAX_NOTIF;
      pendingNotificationsCount--;
    }
    head = tail;
  }

  if (share_analytics) {
    serverLog("notification", "{\"body\":\"" + body + "\",\"title\":\"" + title + "\"}");
  }

  String payload = "{\"user_id\":\"" + USER_ID + "\",\"lock_id\":\"" + LOCK_ID + "\",\"name\":\"" + name +
                   "\",\"timestamp\":\"" + getCurrentTimestamp() + "\",\"title\":\"" + title + "\",\"body\":\"" + body +
                   "\"}";
  handleApiRequest(notification_endpoint, payload);
  mqttClient.publish(
      (String(LOCK_ID) + "/notification").c_str(),
      String("{\"title\":\"" + title + "\",\"body\":\"" + body + "\",\"timestamp\":\"" + getCurrentTimestamp() + "\"}")
          .c_str());
}

bool registerLock(String token) {
  String body = "{\"user_id\":\"" + USER_ID + "\", \"lock_id\":\"" + LOCK_ID + "\",\"lock_name\":\"" + LOCK_NAME +
                "\",\"owner\":\"" + OWNER_NAME + "\",\"model\":\"" + String(LOCK_MODEL) + "\",\"firmware_version\":\"" +
                String(FIRMWARE_VERSION) + "\",\"ip_address\":\"" + WiFi.localIP().toString() + "\", \"hostname\":\"" +
                String(WiFi.getHostname()) + "\"}";
  Serial.printf("Post Data: %s\n", body.c_str());

  int httpResponseCode = handleApiRequest(register_lock_endpoint, body, token, HTTP_POST).code;

  if (httpResponseCode > 0) {
    return (httpResponseCode == HTTP_CODE_CREATED);
  } else {
    HTTPClient http;
    Serial.printf("[HTTP] Register lock failed, error: %s\n", http.errorToString(httpResponseCode).c_str());
    return false;
  }
}

void disableBLE() {
  btStop();
  esp_bt_controller_disable();
  esp_bt_controller_deinit();
  Serial.println("BLE Disabled.");
}

void connectToWifi(const String &ssid, const String &password) {
  if (ssid.isEmpty()) {
    Serial.println("WiFi SSID is empty. Cannot connect to WiFi.");
    return;
  }

  String clean_model = String(LOCK_MODEL);
  clean_model.toLowerCase();
  clean_model.replace(" ", "_");
  String hostname = clean_model + "_" + SIMPLE_ID;
  WiFi.setHostname(hostname.c_str());

  Serial.println("Connecting to WiFi: " + ssid);
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long wifiStartTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStartTime < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection failed.");
    return;
  }

  Serial.println("WiFi connected! IP: " + WiFi.localIP().toString());
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  wifi_config_t conf;
  esp_wifi_get_config(WIFI_IF_STA, &conf);
  conf.sta.listen_interval = 3;
  esp_wifi_set_config(WIFI_IF_STA, &conf);
}

void wifiPowerSaveOff() {
  esp_wifi_set_ps(WIFI_PS_NONE);
  Serial.println("Wi-Fi Power Save Mode Disabled");
}

void initialCommissioning() {
  String WIFI_SSID = (prefs.isKey("wifi_ssid")) ? prefs.getString("wifi_ssid") : "";
  String WIFI_PWD = (!WIFI_SSID.isEmpty()) ? prefs.getString("wifi_pwd") : "";

  if (!WIFI_SSID.isEmpty()) {
    Serial.println("Device is already commissioned");
    LOCK_NAME = prefs.getString("lock_name");
    OWNER_NAME = prefs.getString("owner");
    USER_ID = prefs.getString("user_id");
    REFRESH_TOKEN = prefs.getString("refresh_token");
    connectToWifi(WIFI_SSID, WIFI_PWD);
    return;
  }

  bleServer.begin("JUPY Lock Pro");
  Serial.println("Waiting for BLE commissioning payload to complete...");

  unsigned long commissionStart = millis();
  while (millis() - commissionStart < COMMISSION_TIME) {
    delay(100);
    if (bleServer.requireWifiScan()) {
      int n = WiFi.scanNetworks(false, false, false, 20);
      JsonDocument doc;
      JsonArray networks = doc["wifi_networks"].to<JsonArray>();
      int limit = (n < 10) ? n : 10;
      for (int i = 0; i < limit; i++) {
        JsonObject network = networks.add<JsonObject>();
        network["ssid"] = WiFi.SSID(i);
        network["rssi"] = WiFi.RSSI(i);
        network["secured"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      }
      WiFi.scanDelete();
      String wifiScanJson;
      serializeJson(doc, wifiScanJson);
      bleServer.wifiScanCompleted(wifiScanJson);
    }

    if (bleServer.hasReceivedPayload()) {
      WIFI_SSID = prefs.getString("wifi_ssid");
      WIFI_PWD = prefs.getString("wifi_pwd");
      USER_ID = prefs.getString("user_id");
      LOCK_NAME = prefs.getString("lock_name");
      OWNER_NAME = prefs.getString("owner");
      REFRESH_TOKEN = prefs.getString("refresh_token");
      break;
    }
  }

  if (WIFI_SSID.isEmpty()) {
    Serial.println("Commission timeout. Require Restart...");
    startSleepMode();
    return;
  }

  // if (!setupPin()) {
  //   Serial.println("Failed to setup pin.");
  //   startSleepMode();
  //   return;
  // }

  connectToWifi(WIFI_SSID, WIFI_PWD);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection failed. Restarting...");
    prefs.clear();
    bleServer.sendResponse("{\"status\":\"wifi_fail\"}");
    startSleepMode(200);
    return;
  }
  bleServer.sendResponse("{\"status\":\"wifi_success\"}");

  // Commission SBC via MQTT (replaces UART wakeSBC commission call)
  // GPIO fires first, then command published once broker connection is established
  localMqttClient.setServer(local_mqtt_host, local_mqtt_port);
  localMqttClient.setCallback(localMqttCallback);
  reconnectLocalMQTT();

  if (!localMqttClient.connected()) {
    Serial.println("Failed to connect to local MQTT broker for SBC commissioning.");
    bleServer.sendResponse("{\"status\":\"sbc_mqtt_commission_failed\"}");
    prefs.clear();
    startSleepMode();
    return;
  }

  String commissionCmd = "{\"cmd\":\"commission\", \"user_id\":\"" + USER_ID + "\", \"lock_id\":\"" + LOCK_ID +
                         "\", \"owner\":\"" + OWNER_NAME + "\", \"model\":\"" + String(LOCK_MODEL) + "\", \"name\":\"" +
                         LOCK_NAME + "\", \"firmware_version\":\"" + String(FIRMWARE_VERSION) +
                         "\", \"ip_address\":\"" + WiFi.localIP().toString() + "\", \"hostname\":\"" +
                         String(WiFi.getHostname()) + "\", \"wifi_ssid\":\"" + WIFI_SSID + "\", \"wifi_pwd\":\"" +
                         WIFI_PWD + "\", \"token\":\"" + prefs.getString("token") + "\"}";
  sendToSBC(commissionCmd);

  if (!registerLock(prefs.getString("token"))) {
    prefs.clear();
    bleServer.sendResponse("{\"error\":\"Failed to register Lock\"}");
    Serial.println("Registering lock failed.");
    startSleepMode();
  }

  notify("Lock Registered", "Hi " + OWNER_NAME + "! I am your new " + LOCK_MODEL +
                                ".\nWelcome to JUPY Locks, keeping you safe and secure! :).");

  String ipStatus = "{\"lock_id\":\"" + String(LOCK_ID) + "\",\"lock_ip\":\"" + WiFi.localIP().toString() +
                    "\",\"hostname\":\"" + String(WiFi.getHostname()) + "\"}";
  bleServer.sendResponse(ipStatus);

  for (int i = 0; i < 5; i++) {
    delay(100);
    if (bleServer.hasReceivedIPAck()) {
      break;
    } else {
      bleServer.sendResponse(ipStatus);
      delay(100);
    }
  }

  bleServer.end();
  disableBLE();
}

void setOnline(bool online) {
  String status = (online) ? "online" : "offline";
  if (share_analytics) {
    serverLog("mqtt_status", "{\"state\":\"" + status + "\", \"timestamp\":\"" + getCurrentTimestamp() + "\"}");
  }
  handleApiRequest(lock_endpoint, "{\"online\":" + String(online ? "true" : "false") + "}", "", HTTP_PATCH);

  String topic = String(LOCK_ID) + "/lock/status";
  String msg = (online) ? "I am here :) ~ All Might" : "I am not here, Young Midoriya! :<";
  String statusMsg = (online) ? "online" : "offline";
  String payload =
      ("{\"status\":\"" + statusMsg + "\", \"timestamp\":\"" + getCurrentTimestamp() + "\", \"msg\":\"" + msg + "\"}");
  mqttClient.publish(topic.c_str(), payload.c_str(), true);
  Serial.println("[MQTT] Published status: " + payload);

  sendToSBC("{\"cmd\":\"on\", \"state\":\"" + status + "\"}");
}

void endMQTTSession() {
  setOnline(false);
  mqttClient.disconnect();
  mqttActive = false;
  Serial.println("[MQTT] Session Terminated Successfully.");
}

void startMQTTSession() {
  lastActivity = millis();
  mqttActive = true;
}

void reconnectMQTT() {
  if (mqttClient.connected()) return;

  unsigned long now = millis();
  if (lastMqttRetry != 0 && now - lastMqttRetry < 5000) return;
  lastMqttRetry = now;

  Serial.printf("[MQTT] Connecting to HiveMQ as %s ...\n", LOCK_MODEL);
  if (mqttClient.connect(LOCK_MODEL, mqtt_user, mqtt_pwd)) {
    Serial.println("[MQTT] Connected Successfully.");
    mqttClient.subscribe((String(LOCK_ID) + "/lock/status").c_str(), 1);
    mqttClient.subscribe((String(LOCK_ID) + "/lock/commands").c_str(), 1);
    setOnline(true);
  } else {
    Serial.printf("[MQTT] Connection failed, rc=%d — will retry in 5s\n", mqttClient.state());
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  byte buffer[length + 1];
  memcpy(buffer, payload, length);
  buffer[length] = '\0';

  lastActivity = millis();
  JsonDocument doc;
  deserializeJson(doc, buffer);

  if (doc["cmd"] == "unlock") unlockDoor(doc["name"], "Remote App");
  else if (doc["cmd"] == "start_call")
    sendToSBC("{\"cmd\":\"start_call\",\"room_id\":\"" + doc["room_id"].as<String>() + "\"}");
  else if (doc["cmd"] == "end_call") {
    endMQTTSession();
    sendToSBC("{\"cmd\":\"power_off\"}");
  }
}

void serverLog(String event, String data) {
  handleApiRequest(analytics_endpoint, "{\"lock_id\":\"" + String(LOCK_ID) + "\",\"event\":\"" + event +
                                           "\",\"data\":\"" + sanitizeJsonToStr(data) + "\"}");
}

void handleRequest(String route, HTTPMethod method, std::function<HTTPResponse(const String &)> callback) {
  localServer.on(route, HTTP_ANY, [method, route, callback]() {
    localServer.sendHeader("Access-Control-Allow-Origin", "*");
    localServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, PUT, PATCH, OPTIONS");
    localServer.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    localServer.sendHeader("Access-Control-Allow-Private-Network", "true");

    if (localServer.method() == HTTP_OPTIONS) {
      localServer.send(204);
      return;
    }
    if (localServer.method() != method) {
      localServer.send(405, "text/plain", "Method Not Allowed");
      return;
    }

    String body = "";
    if (localServer.hasArg("plain")) {
      body = localServer.arg("plain");
    } else if (method == HTTP_GET) {
      if (localServer.args() == 0) {
        body = "{}";
      } else {
        body = "{";
        for (uint8_t i = 0; i < localServer.args(); i++) {
          body += "\"" + localServer.argName(i) + "\":\"" + localServer.arg(i) + "\",";
        }
        body.remove(body.length() - 1);
        body += "}";
      }
    } else {
      Serial.print("At route " + route);
      Serial.println(" No body received");
    }

    Serial.printf("[Local Server] %s %s - %s\n", resolveMethod(method), route.c_str(), body.c_str());
    HTTPResponse resp = callback(body);
    if (resp.code == 0 && resp.contentType == "") resp = HTTPResponse{200, "text/plain", String("")};
    localServer.send(resp.code, resp.contentType, resp.body.c_str());
  });
}

bool validateSettings(const char *setting) {
  String settings[] = {"lock_name",     "cam_resolution", "call_timeout",     "notify_all_motion",
                       "record_motion", "record_person",  "vid_snippet_time", "share_analytics"};
  for (String option : settings) {
    if (option.equals(setting)) return true;
  }
  return false;
}

HTTPResponse updateSettings(String body) {
  if (authFail == 3) {
    return HTTPResponse{401, "application/json",
                        ("{\"status\":\"fail\", \"error\":\"Authorization Timeout\", \"timeRemaining\":" +
                         String((AUTH_DISABLE_TIME - (millis() - authTimeout)) / 60000UL) + "}")};
  }
  JsonDocument data;
  DeserializationError error = deserializeJson(data, body);
  if (error) {
    return HTTPResponse{400, "application/json", "{\"status\":\"fail\", \"error\":\"Parsing failed. Try Again.\"}"};
  }

  bool correctPin = checkPin(data["pin"].as<const char *>());
  if (!correctPin || !data["user_id"].as<String>().equals(USER_ID)) {
    authFail += 1;
    if (authFail == 3) {
      authTimeout = millis();
    }
    return HTTPResponse{401, "application/json",
                        "{\"status\":\"fail\", \"error\":\"Unauthorized " + String(correctPin ? "User" : "Pin") +
                            "\"}"};
  }

  JsonObject settings = data["settings"];
  JsonDocument savedSettings;
  deserializeJson(savedSettings, prefs.getString("settings"));
  String lock_name = settings["lock_name"] ? settings["lock_name"].as<String>() : LOCK_NAME;
  int cam_resolution =
      settings["cam_resolution"] ? settings["cam_resolution"].as<int>() : savedSettings["cam_resolution"].as<int>();
  int call_timeout =
      settings["call_timeout"] ? settings["call_timeout"].as<int>() : savedSettings["call_timeout"].as<int>();
  int snippet_time =
      settings["snippet_time"] ? settings["snippet_time"].as<int>() : savedSettings["snippet_time"].as<int>();
  bool record_motion =
      settings["record_motion"] ? settings["record_motion"].as<bool>() : savedSettings["record_motion"].as<bool>();
  bool record_person = (record_motion ? settings["record_person"].as<bool>() : false);
  bool set_notify_all_motion =
      settings["notify_all_motion"] ? settings["notify_all_motion"].as<bool>() : notify_all_motion;
  bool set_share_analytics = settings["share_analytics"] ? settings["share_analytics"].as<bool>() : share_analytics;

  if (settings) {
    for (JsonPair kvp : settings) {
      String option = String(kvp.key().c_str());
      if (validateSettings(option.c_str())) continue;
      else {
        return HTTPResponse{400, "application/json",
                            "{\"status\":\"fail\", \"error\":\"Unknown settings '" + option +
                                "'. May need firmware update\"}"};
      }
    }

    String SBC_settings[] = {"lock_name",     "call_timeout",  "snippet_time",
                             "record_motion", "record_person", "cam_resolution"};
    String settingsJson;
    serializeJson(settings, settingsJson);

    // Push settings to SBC via local MQTT and also publish to settings topic for Pi file write
    for (const String &setting : SBC_settings) {
      if (settings[setting]) {
        sendToSBC("{\"cmd\":\"update_settings\", \"settings\":" + settingsJson + "}");
        // Also publish full settings to dedicated topic so Node-RED writes settings.json
        localMqttClient.publish(TOPIC_SETTINGS, settingsJson.c_str());
        break;
      }
    }

    if (!lock_name.equals(LOCK_NAME)) {
      prefs.putString("lock_name", LOCK_NAME);
      settings["lock_name"] = NULL;
      serializeJson(settings, settingsJson);
      handleApiRequest(lock_endpoint, "{\"settings\":" + settingsJson + "}", "", HTTP_PATCH);
    }

    if (set_share_analytics != share_analytics) {
      share_analytics = set_share_analytics;
      prefs.putBool("share_analytics", share_analytics);
    }

    if (set_notify_all_motion != notify_all_motion) {
      notify_all_motion = set_notify_all_motion;
      prefs.putBool("notify_all_motion", notify_all_motion);
    }

    if (share_analytics) {
      serverLog("settings", "{\"data\":" + settingsJson + ", timestamp\":\"" + getCurrentTimestamp() + "\"}");
    }

    prefs.putString("settings", settingsJson);
    return HTTPResponse{200, "application/json", "{\"status\":\"success\"}"};
  } else {
    return HTTPResponse{400, "application/json", "{\"status\":\"fail\", \"error\":\"Bad request.\"}"};
  }
}

void setupREST() {
  if (WiFi.status() != WL_CONNECTED) {
    String WIFI_SSID = prefs.getString("wifi_ssid");
    String WIFI_PWD = prefs.getString("wifi_pwd");
    if (!WIFI_SSID.isEmpty()) {
      connectToWifi(WIFI_SSID, WIFI_PWD);
    }

    if (WiFi.status() != WL_CONNECTED) {
      String ap_ssid = "JUPY_SmartLock_" + String(SIMPLE_ID);
      WiFi.softAP(ap_ssid.c_str(), "12345678");
      delay(100);
      return;
    }
  }
  wifiPowerSaveOff();

  handleRequest("/unlock", HTTP_POST, [](String body) {
    JsonDocument data;
    DeserializationError error = deserializeJson(data, body);
    if (error) {
      return HTTPResponse{400, "application/json", "{\"status\":\"fail\", \"error\":\"Parsing failed. Try again\" }"};
    }
    if (checkPin(data["pin"].as<const char *>())) {
      unlockDoor(data["name"], "local WiFi");
      return HTTPResponse{200, "application/json", "{\"status\":\"success\"}"};
    } else {
      return HTTPResponse{401, "application/json",
                          "{\"status\":\"fail\", \"error\":\"Wrong pin stored, pin may have been updated\" }"};
    }
  });

  handleRequest("/health", HTTP_GET,
                [](String body) { return HTTPResponse{200, "application/json", "{\"status\":\"I am healthy\"}"}; });
  handleRequest("/settings", HTTP_PATCH, &updateSettings);

  handleRequest("/status", HTTP_GET, [](String body) {
    String status = "{";
    status += "\"lock_name\":\"" + LOCK_NAME + "\",";
    status += "\"owner\":\"" + OWNER_NAME + "\",";
    status += "\"wifi_ssid\":\"" + prefs.getString("wifi_ssid") + "\",";
    status += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
    status += "\"battery\":\"" + String(getBatteryLevel()) + "\",";
    status += "\"sbc_online\":" + String(localMqttClient.connected() ? "true" : "false") + ",";
    status += "\"settings\":" + prefs.getString("settings", "{}");
    status += "}";
    return HTTPResponse{200, "application/json", status};
  });

  handleRequest("/change-pin", HTTP_PATCH, [](String body) {
    JsonDocument data;
    DeserializationError error = deserializeJson(data, body);
    if (error) {
      return HTTPResponse{400, "application/json", "{\"status\":\"fail\", \"error\":\"Parsing failed. Try again\" }"};
    }
    if (!data["user_id"].as<String>().equals(USER_ID) && !data["reset_pin"].as<String>().equals(RESET_PIN)) {
      return HTTPResponse{401, "application/json", "{\"status\":\"fail\", \"error\":\"Unauthorized request\"}"};
    }
    String newPin = data["new_pin"].as<String>();
    if (newPin.length() < 4 || newPin.length() > 12) {
      return HTTPResponse{400, "application/json",
                          "{\"status\":\"fail\", \"error\":\"New pin must be between 4 and 12 characters\"}"};
    } else {
      prefs.putString("pin", newPin);
      notify("Pin Changed", OWNER_NAME + " changed the lock pin.", OWNER_NAME);
      return HTTPResponse{200, "application/json", "{\"status\":\"success\"}"};
    }
    return HTTPResponse{400, "application/json", "{\"status\":\"fail\", \"error\":\"Required fields missing\"}"};
  });

  handleRequest("/reset", HTTP_POST, [](String body) {
    JsonDocument data;
    DeserializationError error = deserializeJson(data, body);
    if (error) {
      return HTTPResponse{400, "application/json", "{\"status\":\"fail\", \"error\":\"Parsing failed. Try again\" }"};
    }
    if (data["reset_pin"]) {
      if (!data["user_id"].as<String>().equals(USER_ID) && !data["reset_pin"].as<String>().equals(RESET_PIN))
        return HTTPResponse{401, "application/json", "{\"status\":\"fail\", \"error\":\"Unauthorized\"}"};
      unlockDoor("Reset Lock", "REST API");
      digitalWrite(LOCK_PIN, HIGH);
      delay(6000);
      digitalWrite(LOCK_PIN, LOW);
      FORCED_RESET_LOCK = true;
      prefs.putBool("forced_reset", true);
      return HTTPResponse{200, "application/json", "{\"status\":\"success\"}"};
    }
    return HTTPResponse{400, "application/json", "{\"status\":\"fail\", \"error\":\"Required fields missing\"}"};
  });

  localServer.onNotFound([]() {
    if (localServer.method() == HTTP_OPTIONS) {
      localServer.sendHeader("Access-Control-Allow-Origin", "*");
      localServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, PUT, PATCH, OPTIONS");
      localServer.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
      localServer.sendHeader("Access-Control-Allow-Private-Network", "true");
      localServer.send(204);
    } else {
      localServer.send(404, "text/plain", "Not Found");
    }
  });

  localServer.begin();
  Serial.print("[Server] REST Server started on: ");
  Serial.println(WiFi.localIP());
}

// --- DISPLAY & TOUCH ---
void drawKeypad() {
  int width = tft.width();
  int height = tft.height();
  int cellWidth = width / 3;
  int cellHeight = height / 4;
  Serial.println("Display dimensions: " + String(width) + "x" + String(height));
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setTextFont(2);

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 3; c++) {
      String key = touchpad_keys[r][c];
      int textWidth = tft.textWidth(key);
      int textHeight = tft.fontHeight();
      int cellLeft = c * cellWidth;
      int cellTop = r * cellHeight + 10;
      int cellW = cellWidth - 4;
      int cellH = cellHeight - 10;
      int centerX = cellLeft + cellW / 2;
      int centerY = cellTop + cellH / 2;
      int textX = centerX - textWidth / 2;
      int textY = centerY - textHeight / 2;

      tft.drawRect(cellLeft, cellTop, cellW, cellH, TFT_WHITE);
      tft.drawString(key, textX, textY);
    }
  }
}

unsigned long valid_touch_time = 0;
unsigned long tft_touch_response_time = 0;
bool isBeingPressed = false;
unsigned long t_irq_response = 0;

void handleTouch() {
  uint16_t x = 0, y = 0, z = 0;
  bool tftTouched = tft.getTouch(&x, &y, 600);
  z = tft.getTouchRawZ();

  if (tftTouched) {
    if (x == 0 && y == 0) return;
    tft_touch_response_time = millis();
  }

  if (tftTouched && !isBeingPressed) {
    if (millis() - valid_touch_time < 300) return;
    isBeingPressed = true;
    valid_touch_time = millis();
    Serial.printf("Touch START tft=(%u,%u,%u) T_IRQ=%d\n", x, y, z, digitalRead(T_IRQ));
    int col = x / (tft.width() / 3);
    int row = y / (tft.height() / 4);
    if (row >= 0 && row < 4 && col >= 0 && col < 3) {
      processKeyPress(row, col, x, y);
    }
  }

  if (!tftTouched && isBeingPressed) {
    if (millis() - tft_touch_response_time > 100) {
      isBeingPressed = false;
      Serial.printf("Touch RELEASED\n");
      t_irq_response = millis();
    }
  }
}

unsigned long keypad_last_pressed_time = 0;
void handleKeypad() {
  if (millis() - keypad_last_pressed_time < 300) return;
  keypad_last_pressed_time = millis();

  if (CURRENT_LED_COLOR != DEF_LED_COLOR) {
    resetLEDColor();
  }

  char key = keypad.getKey();
  if (key) {
    int cellWidth = tft.width() / 3;
    int cellHeight = tft.height() / 4;
    Serial.println("Keypad presessed key: " + String(key));
    for (int r = 0; r < ROWS; r++) {
      for (int c = 0; c < COLS; c++) {
        if (keypad_keys[r][c] == key) {
          if (c >= 3) return;
          int cellLeft = c * cellWidth;
          int cellTop = r * cellHeight + 10;
          int cellW = cellWidth - 4;
          int cellH = cellHeight - 10;
          int centerX = cellLeft + cellW / 2;
          int centerY = cellTop + cellH / 2;
          processKeyPress(r, c, centerX, centerY + 15);
          return;
        }
      }
    }
  }
}

void captureFace() { sendToSBC("{\"cmd\":\"capture_face\"}"); }

void doorbellChime() {
  sendToSBC("{\"cmd\":\"doorbell_chime\"}");
  notify("Doorbell", "Someone is at " + OWNER_NAME + "'s " + LOCK_NAME + "!");
  startMQTTSession();
}

bool enteringNewPin = false;
bool validating_pin = false;
bool checkNewPin = false;
String newPinBuffer = "";

void processKeyPress(int row, int col, int x, int y) {
  tft.fillCircle(x, y, 5, TFT_WHITE);
  lastTouchX.push_back(x);
  lastTouchY.push_back(y);
  lastTouchColor.push_back(TFT_BLACK);
  touchFadeTime.push_back(millis() + 750);

  lastActivity = millis();  // Extend User Aware Timeouts

  Serial.printf("Key press at row: %d, col: %d\n", row, col);
  setLEDColor(CRGB::HotPink);
  if (row == 3 && col == 0) {
    passcodeBuffer = "";
  } else if (row == 3 && col == 2) {
    if (passcodeBuffer.length() == 0) {
      doorbellChime();
    } else {
      if (enteringNewPin) {
        checkNewPin = true;
      } else if (String("1234").equals(passcodeBuffer)) {
        Serial.println("Test shutdown command received.");
        startSleepMode(2000);
      } else if (checkPin(passcodeBuffer.c_str())) {
        unlockDoor("Manual Operation", "Pin Pad");
        if (faceUnlockTimeout) pinManuallyEntered = true;
      } else {
        if (unlockAttempts < 3) unlockAttempts += 1;
        else {
          faceUnlockTimeout = millis();
          intruder = 0;
          captureFace();
          notify("Intruder Alert!", "Multiple incorrect pin attempts at " + OWNER_NAME + "'s " + LOCK_NAME + "!");
        }
        Serial.println("[Keypad] Incorrect passcode: " + passcodeBuffer);
        Serial.println("[Keypad] Stored pin: " + prefs.getString("pin"));
      }
      passcodeBuffer = "";
    }
  } else {
    if (passcodeBuffer.length() < 12) passcodeBuffer += touchpad_keys[row][col];
    else Serial.println("Passcode buffer full: " + touchpad_keys[row][col]);
  }

  if (enteringNewPin) newPinBuffer += passcodeBuffer;
  Serial.println("Current passcode buffer: " + passcodeBuffer);
}

void touch_calibrate() {
  uint16_t calData[5];
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(20, 0);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.println("Touch corners as indicated");
  tft.setTextFont(1);
  tft.println();
  tft.calibrateTouch(calData, TFT_MAGENTA, TFT_BLACK, 15);

  Serial.println();
  Serial.println("// Use this calibration code in setup():");
  Serial.print("  uint16_t calData[5] = { ");
  for (uint8_t i = 0; i < 5; i++) {
    Serial.print(calData[i]);
    if (i < 4) Serial.print(", ");
  }
  Serial.println(" };");
  Serial.println("  tft.setTouch(calData);");

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.println("Calibration complete!");
}

bool setupPin() {
  passcodeBuffer = "";
  drawKeypad();
  enteringNewPin = true;
  bleServer.sendResponse("{\"status\":\"prompt_enter_pin\"}");
  long int pinTimeout = millis() + 2 * 60 * 1000;

  while (millis() < pinTimeout) {
    handleTouch();
    handleKeypad();
    if (checkNewPin) {
      checkNewPin = false;
      if (newPinBuffer.length() >= 4 && newPinBuffer.length() <= 12) {
        if (validating_pin) {
          if (!checkPin(newPinBuffer.c_str())) {
            bleServer.sendResponse("{\"status\":\"enter_pin\", \"success\":\"false\", \"error\":\"Entered pin does not "
                                   "match current pin. Try again.\"}");
            passcodeBuffer = "";
            enteringNewPin = false;
            prefs.putString("pin", "");
            return false;
          } else {
            bleServer.sendResponse("{\"status\":\"new_pin\", \"success\":\"true\"}");
            passcodeBuffer = "";
            newPinBuffer = "";
            validating_pin = false;
            enteringNewPin = false;
            return true;
          }
        }
        prefs.putString("pin", newPinBuffer);
        notify("Pin Changed", OWNER_NAME + " set a new lock pin.", OWNER_NAME);
      } else {
        bleServer.sendResponse("{\"status\":\"enter_pin\", \"warn\":\"Pin must be 4-12 characters. Try again.\"}");
        passcodeBuffer = "";
      }
      newPinBuffer = "";
    }
  }
  bleServer.sendResponse(
      "{\"status\":\"enter_pin\", \"success\":\"false\", \"error\":\"Pin setup timed out. Please try again.\"}");
  return false;
}
