#include "ble_server.h"
#include <Preferences.h>
#include <Wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Forward declaration
static void wifiScanTask(void *parameter);

// ==================== BLECommissioningServer ====================
BLECommissioningServer::BLECommissioningServer()
    : pServer(nullptr), pRxCharacteristic(nullptr), pTxCharacteristic(nullptr), deviceConnected(false),
      payloadReceived(false) {}

BLECommissioningServer::~BLECommissioningServer() { end(); }

void BLECommissioningServer::end() {
  sendResponse("{\"status\":\"disconnected\"}");
  pServer->getAdvertising()->stop();
  BLEDevice::deinit();
}

void BLECommissioningServer::begin(const char *deviceName) {
  BLEDevice::init(deviceName);
  BLEDevice::setMTU(517);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks(this));

  BLEService *commissionService = pServer->createService(SERVICE_UUID);

  // RX: Write-only (client sends commissioning payload)
  pRxCharacteristic = commissionService->createCharacteristic(RX_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE_NR);
  pRxCharacteristic->setCallbacks(new RxCharacteristicCallbacks(this));

  // TX: Read + Notify (server sends lock info after WiFi connects)
  pTxCharacteristic = commissionService->createCharacteristic(TX_CHAR_UUID, BLECharacteristic::PROPERTY_READ |
                                                                                BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());
  pTxCharacteristic->setValue("{}");  // Default empty response

  commissionService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] Server started - Device: " + String(deviceName));
}

void BLECommissioningServer::sendResponse(const String &response) {
  if (!pTxCharacteristic) return;

  pTxCharacteristic->setValue(response.c_str());
  pTxCharacteristic->notify();

  Serial.println("[BLE] Response sent: " + response);
}

bool BLECommissioningServer::isConnected() { return deviceConnected; }

bool BLECommissioningServer::hasReceivedPayload() { return payloadReceived; }

bool BLECommissioningServer::hasReceivedIPAck() { return ipReceivedAck; }

bool BLECommissioningServer::requireWifiScan() { return wifiScanRequired; }

void BLECommissioningServer::wifiScanCompleted(const String &networks) {
  wifiScanRequired = false;
  sendResponse(networks);
}

// ==================== ServerCallbacks ====================
void ServerCallbacks::onConnect(BLEServer *pServer) {
  bleServer->deviceConnected = true;
  Serial.println("[BLE] Client connected");
}

void ServerCallbacks::onDisconnect(BLEServer *pServer) {
  bleServer->deviceConnected = false;
  Serial.println("[BLE] Client disconnected");
  pServer->getAdvertising()->start();
}

// ==================== WiFi Scan Task ====================
static TaskHandle_t wifiScanTaskHandle = nullptr;
static BLECommissioningServer *scanResultBleServer = nullptr;

static void wifiScanTask(void *parameter) {
  Serial.println("[WiFi Scan] Starting scan...");

  int n = WiFi.scanNetworks(false, false, false, 20);

  Serial.printf("[WiFi Scan] Found %d networks\n", n);

  // Use DynamicJsonDocument - much more memory efficient
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

  String response;
  serializeJson(doc, response);

  Serial.println("[WiFi Scan] Sending response...");
  scanResultBleServer->sendResponse(response);

  scanResultBleServer = nullptr;
  wifiScanTaskHandle = nullptr;
  vTaskDelete(NULL);
}

// ==================== RxCharacteristicCallbacks ====================
void RxCharacteristicCallbacks::onWrite(BLECharacteristic *pCharacteristic) {
  std::string rxValue = pCharacteristic->getValue();

  if (rxValue.length() == 0) return;

  String payload = String((const char *)rxValue.c_str());
  Serial.println("[BLE] Received payload:");
  Serial.println(payload);

  // Parse and validate JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    bleServer->sendResponse("{\"error\":\"JSON parse error\"}");
    Serial.println("[BLE] JSON parse error");
    return;
  }

  if (doc["status"] && doc["status"].as<String>().equals("ip_ack")) {
    bleServer->ipReceivedAck = true;
    Serial.println("[BLE] Received IP acknowledgment from app.");
    return;
  }

  if (doc["request"] && doc["request"].as<String>().equals("wifi_networks")) {
    // Check if a scan is already in progress
    if (bleServer->requireWifiScan()) {
      bleServer->sendResponse("{\"error\":\"Scan already in progress\"}");
      return;
    }

    bleServer->wifiScanRequired = true;  // Set flag to start WiFi scan

    // Send initial response that scan has started
    bleServer->sendResponse("{\"status\":\"scanning\"}");
    return;
  }

  // Validate required fields
  if (!doc["user_id"] || !doc["wifi_ssid"] || !doc["wifi_pwd"] || !doc["lock_name"] || !doc["owner"] || !doc["pin"] ||
      !doc["pairing_code"] || !doc["token"]) {
    bleServer->sendResponse("{\"error\":\"Missing required fields\"}");
    Serial.println("[BLE] Missing required fields");
    return;
  }

  Preferences prefs;
  prefs.begin("my_storage", false);
  if (!doc["pairing_code"].as<String>().equals(prefs.getString("pairing_code"))) {
    bleServer->sendResponse("{\"error\":\"Invalid pairing code\"}");
    Serial.println("[BLE] Invalid pairing code");
    return;
  }

  // Store in NVS
  prefs.putString("user_id", doc["user_id"].as<String>());
  prefs.putString("wifi_ssid", doc["wifi_ssid"].as<String>());
  prefs.putString("wifi_pwd", doc["wifi_pwd"].as<String>());
  prefs.putString("lock_name", doc["lock_name"].as<String>());
  prefs.putString("owner", doc["owner"].as<String>());
  prefs.putString("token", doc["token"].as<String>());
  prefs.putString("pin", doc["pin"].as<String>());
  prefs.end();

  // Send acknowledgment via TX characteristic
  String ack = "{\"status\":\"received\"}";
  bleServer->sendResponse(ack);

  bleServer->payloadReceived = true;
  Serial.println("[BLE] Credentials stored successfully");
}
