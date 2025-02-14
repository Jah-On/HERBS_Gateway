#define FIRMWARE_VERSION "00.01.000"

#define DEBUG

// Secrets
#include <include/Secrets.h>

// LoRa constants
#include <include/LoRa.h>

// STL Includes
#include <list>          // Front/Back optimized lists
#include <unordered_map> // Performant hash table

// External Includes
#include <ChaChaPoly.h>
#include <heltec_unofficial.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

// ESP32 Includes
#include <esp_https_ota.h>
#include <esp_ota_ops.h>
#include <esp_sleep.h>
#include <esp_timer.h>

#define JSON_FMT_STRING "{\
\"battery\":%d,\
\"hive_temp\":%d,\
\"extern_temp\":%d,\
\"humidity\":%d,\
\"pressure\":%d,\
\"acoustics\":%d\
}"

#define DATAPACKET_FIELDS data.battery,\
                          data.hive_temp,\
                          data.extern_temp,\
                          data.humidity,\
                          data.pressure,\
                          data.acoustics

typedef enum CallbackFlags : uint32_t {
  NONE                      = 0x00000000,
  PING                      = 0x00000001,
  CHECK_FOR_FIRMWARE_UPDATE = 0x00000002,
  TURN_OFF_LED              = 0x00000004
} CallbackFlags;

void setCallbackFlag(void*);

const char DATA_POST_REQUEST[] = 
"POST /" APIARY_ID "/%" PRIx64 " HTTP/1.1\n"
"Host:" HTTP_HOST ":" TO_STRING(HTTP_PORT)"\n"
"Content-Type: application/json\n"
"Content-Length: %d\n\n" 
JSON_FMT_STRING "\r\n"
;

const char PING_PUT_REQUEST[] = "\
PUT /" APIARY_ID "/ping HTTP/1.1\n\
Host:" HTTP_HOST ":" TO_STRING(HTTP_PORT)"\n\r\n"
;

const char GATEWAY_FW_INFO[] = "\
GET /gateway/firmware/info/" APIARY_ID "/" TO_STRING(GATEWAY_ID) " HTTP/1.1\n\
Host:" HTTP_HOST ":" TO_STRING(HTTP_PORT)"\n\r\n"
;

const char GATEWAY_FW_UPDATE_URL[] = "\
https://" HTTP_HOST ":" TO_STRING(HTTP_PORT)
"/gateway/firmware/bin/" APIARY_ID "/" TO_STRING(GATEWAY_ID);

const esp_http_client_config_t otaHttpConfig = {
  .url = GATEWAY_FW_UPDATE_URL,
  .cert_pem = CLIENT_CERT,
};

const esp_https_ota_config_t otaConfig = {
  .http_config = &otaHttpConfig 
};

std::unordered_map<uint64_t, ChaChaPoly> nodeEncryption;

std::list<std::pair<uint8_t, Packet>> recievedPackets = {};

WiFiClientSecure client;

uint8_t tempBuffer[sizeof(DataPacket)];

const esp_timer_create_args_t pingTimerCfg = {
  .callback        = setCallbackFlag,
  .arg             = (void*)CallbackFlags::PING,
  .dispatch_method = ESP_TIMER_TASK,
  .name            = "Server Ping Timer"
};
esp_timer_handle_t pingTimer;

const esp_timer_create_args_t firmwareCheckTimerCfg = {
  .callback        = setCallbackFlag,
  .arg             = (void*)CallbackFlags::CHECK_FOR_FIRMWARE_UPDATE,
  .dispatch_method = ESP_TIMER_TASK,
  .name            = "Firmware Check Timer"
};
esp_timer_handle_t firmwareCheckTimer; 

const esp_timer_create_args_t ledBlinkTimerCfg = {
  .callback        = setCallbackFlag,
  .arg             = (void*)CallbackFlags::TURN_OFF_LED,
  .dispatch_method = ESP_TIMER_TASK,
  .name            = "Firmware Check Timer"
};
esp_timer_handle_t ledBlinkTimer; 

uint32_t cbFlags = CallbackFlags::NONE;

void setup() {
  pinMode(35, OUTPUT);

  heltec_setup();

  client.setCACert(CLIENT_CERT);

  // Init encryption
  for (auto node : knownMonitors){
    nodeEncryption.insert({node.first, ChaChaPoly()});

    if (!nodeEncryption[node.first].setKey(node.second.key, 16)) raiseError("Could not set key...");
    if (!nodeEncryption[node.first].setIV( node.second.iv,   8)) raiseError("Could not set IV...");
  }

  // Init functions
  if (!LoRaInit()) raiseError("LoRa setup failed...");

  heltec_delay(2000);

  radio.setPacketReceivedAction(onRecieve);
  radio.startReceive();

  for (auto [id, key] : knownMonitors) sendEventPacket(id, EventCode::NODE_ONLINE);

  esp_sleep_enable_ulp_wakeup();
  esp_sleep_enable_timer_wakeup(3400);

  esp_timer_create(&pingTimerCfg,          &pingTimer);
  esp_timer_create(&firmwareCheckTimerCfg, &firmwareCheckTimer);
  esp_timer_create(&ledBlinkTimerCfg,      &ledBlinkTimer);

  esp_timer_start_periodic(pingTimer,           600e6);
  esp_timer_start_periodic(firmwareCheckTimer, 1800e6);

  putPing();
}

void loop() {
  while (recievedPackets.size()) {
    uint8_t packetSize = recievedPackets.front().first;
    Packet& packet     = recievedPackets.front().second;

    uint8_t notFound = 1;
    for (const auto& [id, keys] : knownMonitors){
      if (id != packet.id) continue;

      --notFound;
      break;
    }

    if (notFound) return recievedPackets.pop_front();

    switch (packetSize) {
    case sizeof(DataPacket):
      if (!handleDataPacket(packet)) {
        recievedPackets.pop_front();
        return;
      }
      break;
    default:
      handleEventPacket(packet);
      recievedPackets.pop_front();
      return;
    }

    postData(packet.id, packet.type.data);

    recievedPackets.pop_front();
  }

  switch (cbFlags) {
  case NONE:
    esp_light_sleep_start();
    return;
  default: break;
  }

  if (cbFlags & TURN_OFF_LED){
    cbFlags ^= TURN_OFF_LED;
    digitalWrite(35, LOW);
  } else if (cbFlags & PING){
    cbFlags ^= PING;
    putPing();
  } else {
    cbFlags ^= CHECK_FOR_FIRMWARE_UPDATE;
    checkForFirmwareUpdate();
  }
}

bool handleDataPacket(Packet& packet){
  nodeEncryption[packet.id].decrypt(
    (uint8_t*)&packet.type, 
    packet.type.encrypted, 
    sizeof(DataPacket)
  );

  #ifdef DEBUG
  Serial.printf("Checking tag for node %" PRIx64 ".\n", packet.id);
  #endif

  if (!nodeEncryption[packet.id].checkTag(packet.tag, tagSize)) return false;

  #if defined(DEBUG)
  Serial.println("Check passed.");
  #endif

  sendEventPacket(packet.id, EventCode::DATA_RECVED);

  return true;
}

void handleEventPacket(Packet& packet){
  ChaChaPoly newCrypt;

  if (!newCrypt.setKey(knownMonitors.at(packet.id).key, 16)) 
    raiseError("Could not set key...");
  if (!newCrypt.setIV( knownMonitors.at(packet.id).iv,   8)) 
    raiseError("Could not set IV...");

  newCrypt.decrypt(
    (uint8_t*)&packet.type, 
    (uint8_t*)packet.type.encrypted, 
    sizeof(EventPacket)
  ); 

  #ifdef DEBUG
  Serial.printf("Checking tag for reset from %." PRIx64 "\n", packet.id);
  #endif

  if (!newCrypt.checkTag(packet.tag, tagSize)) return;

  nodeEncryption[packet.id].clear();
  nodeEncryption[packet.id] = newCrypt;

  #ifdef DEBUG
  Serial.printf("Node %" PRIx64 " back online!\n", packet.id);
  #endif
}

void sendEventPacket(uint64_t target, EventCode event){
  Packet packet;

  radio.clearPacketReceivedAction();

  packet.id                   = target;
  packet.type.event.eventCode = event;

  nodeEncryption[target].encrypt(
    packet.type.encrypted,
    (uint8_t*)&packet.type,
    sizeof(EventPacket)
  );

  nodeEncryption[target].computeTag(&packet.tag, tagSize);

  radio.transmit((uint8_t*)&packet, eventPacketSize);

  radio.setPacketReceivedAction(onRecieve);
  radio.startReceive();
}

void setCallbackFlag(void* cbFlag){
  uint32_t asValue = (uint32_t)cbFlag;

  cbFlags |= asValue;
}

void checkForFirmwareUpdate(){
  httpConnect();
  client.print(GATEWAY_FW_INFO);

  for (uint8_t count = 0; count < 10; ++count){
    if (client.available()) break;

    delay(100);
  }

  switch (client.available()) {
    case 86: break;
    default:
      return disconnect();
  }

  uint8_t index = 0;
  for (; index < 76; ++index) client.read();

  for (index = 0; index < 9; ++index){
    if (client.read() > FIRMWARE_VERSION[index]) break;
  }

  switch (index) {
    case 9:  return disconnect();
    default: break;
  }

  client.clear();
  client.stop();

  esp_err_t otaStatus = esp_https_ota(&otaConfig);
  if (otaStatus != ESP_OK) {
    Serial.println("Updating failed...");
    return;
  }

  esp_restart();
}

void postData(uint64_t& monitorId, DataPacket& data){
  httpConnect();

  client.printf(
    DATA_POST_REQUEST, 
    monitorId,
    snprintf(NULL, 0, JSON_FMT_STRING, DATAPACKET_FIELDS),
    DATAPACKET_FIELDS
  );

  for (uint8_t count = 0; count < 10; ++count){
    if (client.available()) break;

    delay(100);
  }

  disconnect();
}

void putPing(){
  httpConnect();
  client.print(PING_PUT_REQUEST);

  for (uint8_t count = 0; count < 10; ++count){
    if (client.available()) break;

    delay(100);
  }

  disconnect(); 
}

void httpConnect(){
  WiFi.begin(WIFI_SSID, WIFI_PASSPHRASE);

  for (uint8_t count = 0; count < 10; ++count){
    if (WiFi.status() == WL_CONNECTED) break;

    delay(300);
  }

  client.connect(HTTP_HOST, HTTP_PORT);

  for (uint8_t count = 0; count < 10; ++count){
    if (client.connected()) break;

    client.connect(HTTP_HOST, HTTP_PORT);
    delay(10);
  }
}

bool LoRaInit(){
  int16_t res = radio.begin(
    LORA_FREQUENCY_US,
    LORA_BANDWIDTH_125,
    LORA_SPREADING_FACTOR_9,
    LORA_CODING_RATE_4_8
  );
  radio.setRxBoostedGainMode(true);

  return res == 0;
}

void onRecieve(){
  uint8_t dump;
  size_t  packetSize = radio.getPacketLength(true);

  switch (packetSize) {
  case eventPacketSize:
  case dataPacketSize:
    recievedPackets.emplace_back();
    break;
  default:
    radio.readData(&dump, 1);
    return;
  }

  // Blink LED
  digitalWrite(35, HIGH);
  esp_timer_start_once(ledBlinkTimer, 300e3);

  recievedPackets.back().first = packetSize - idSize - tagSize;

  radio.readData(
    (uint8_t*)&recievedPackets.back().second, 
    packetSize
  );
}

void raiseError(String message){
  heltec_delay(10e3);

  throw("Critical error!");
}

void disconnect(){
  client.clear();
  client.stop();
  WiFi.disconnect(true);
}