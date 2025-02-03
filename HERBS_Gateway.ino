// Comment out for actual use to save energy
// Otherwise it will print useful info over serial
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
#include <WiFiClient.h>

// ESP32 Includes
#include <esp_sleep.h>

const char* JSON_FMT_STRING = "{\
\"battery\":%d,\
\"hive_temp\":%d,\
\"extern_temp\":%d,\
\"humidity\":%d,\
\"pressure\":%d,\
\"acoustics\":%d\
}";

char formattedJson[256];

std::unordered_map<uint64_t, ChaChaPoly> nodeEncryption;

std::list<std::pair<uint8_t, Packet>> recievedPackets = {};

WiFiClient client;

uint8_t tempBuffer[sizeof(DataPacket)];

uint16_t ledCounter  = 1;
uint16_t pingCounter = 0;
int8_t   direction   = 1;

void setup() {
  #if defined(DEBUG)
  Serial.begin(115200);
  #endif

  pinMode(35, OUTPUT);

  heltec_setup();

  // Setup display
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.display();

  // Init encryption
  for (auto node : knownMonitors){
    nodeEncryption.insert({node.first, ChaChaPoly()});

    if (!nodeEncryption[node.first].setKey(node.second.key, 16)) raiseError("Could not set key...");
    if (!nodeEncryption[node.first].setIV( node.second.iv,   8)) raiseError("Could not set IV...");
  }

  // Init functions
  if (!LoRaInit()) raiseError("LoRa setup failed...");

  heltec_delay(2000);

  WiFi.begin(WIFI_SSID, WIFI_PASSPHRASE, 0, NULL, false);

  radio.setPacketReceivedAction(onRecieve);
  radio.startReceive();

  for (auto node : knownMonitors){
    sendEventPacket(node.first, EventCode::NODE_ONLINE);
  }

  esp_sleep_enable_ulp_wakeup();
  esp_sleep_enable_timer_wakeup(3400);
}

void loop() {
  esp_light_sleep_start();

  switch (ledCounter) {
  case 0:
    digitalWrite(35, LOW);
    break;
  default:
    --ledCounter;
    break;
  }

  switch (++pingCounter) {
  case 17647:
    putPing();
    pingCounter = 0;
    break;
  default:
    break;
  }

  switch (recievedPackets.size()) {
  case 0:  return;
  default: break;
  }

  uint8_t packetSize = recievedPackets.front().first;
  Packet& packet     = recievedPackets.front().second;

  if (!knownMonitors.contains(packet.id)){
    #ifdef DEBUG
    Serial.printf("Unknown packet node: %" PRIx64 "\n", packet.id);
    #endif

    recievedPackets.pop_front();
    return;
  }

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

void postData(uint64_t& monitorId, DataPacket& data){
  while (!WiFi.reconnect()){
    delay(1000);
  }

  client.connect(HTTP_HOST, HTTP_PORT);

  client.printf(
    "POST /%" PRIx64 " HTTP/1.1\nHost: %s:%d\nConnection: keep-alive\nKeep-Alive: timeout=600, max=1000\n", 
    monitorId,
    HTTP_HOST,
    HTTP_PORT
  );

  // Send content length
  client.printf(
    "Content-Length: %d\n\n", 
    snprintf(formattedJson, sizeof(formattedJson), JSON_FMT_STRING, 
      data.battery,
      data.hive_temp,
      data.extern_temp,
      data.humidity,
      data.pressure,
      data.acoustics
    )
  );
  
  // Send JSON to stream
  client.println(formattedJson);

  // Closes connection with the server
  client.stop();

  // Disconnects and sleeps Wi-Fi
  WiFi.disconnect(true);
}

void putPing(){
  while (!WiFi.reconnect()){
    delay(1000);
  }

  client.connect(HTTP_HOST, HTTP_PORT);

  client.printf(
    "PUT /%s/ping HTTP/1.1\nHost: %s:%d\nConnection: keep-alive\nKeep-Alive: timeout=600, max=1000\n", 
    HTTP_ACCESS_KEY,
    HTTP_HOST,
    HTTP_PORT
  );

  // Send content length
  client.printf(
    "Content-Length: %d\n\n", 
    0
  );
  
  // Closes connection with the server
  client.stop();

  // Disconnects and sleeps Wi-Fi
  WiFi.disconnect(true);
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
    #ifdef DEBUG
    Serial.printf("Invalid packet size of %d recieved.\n", packetSize);
    #endif

    radio.readData(&dump, 1);
    return;
  }

  // Blink LED
  ledCounter = 88;
  digitalWrite(35, HIGH);

  recievedPackets.back().first = packetSize - idSize - tagSize;

  radio.readData(
    (uint8_t*)&recievedPackets.back().second, 
    packetSize
  );
}

void raiseError(String message){
  display.println(message);
  display.display();

  heltec_delay(10e3);

  throw("Critical error!");
}