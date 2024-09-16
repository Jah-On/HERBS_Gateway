// Comment out for actual use to save energy
// Otherwise it will print useful info over serial
#define DEBUG 

// HERBS Data Types
#include "herbsTypes.h"

// STL Includes
#include <chrono>        // Time related
#include <list>          // Front/Back optimized lists
#include <optional>
#include <unordered_map> // Performant hash table

// Arduino Includes
#include <timers.h>

// External Includes
#include <arduino-timer.h>
#include <ChaChaPoly.h>
#include <Crypto.h>
#include <heltec_unofficial.h>
#include <WiFi.h>
#include <WiFiClient.h>

// Info that should not be public
// Such as Wi-Fi SSID and passphrase
#include "secrets.h"

// LoRa constants
#define LORA_FREQUENCY_US        905.2

#define LORA_BANDWIDTH_7_8       7.8
#define LORA_BANDWIDTH_20_8      20.8
#define LORA_BANDWIDTH_62_5      62.5

#define LORA_SPREADING_FACTOR_5  5
#define LORA_SPREADING_FACTOR_6  6
#define LORA_SPREADING_FACTOR_7  7
#define LORA_SPREADING_FACTOR_8  8
#define LORA_SPREADING_FACTOR_9  9
#define LORA_SPREADING_FACTOR_10 10
#define LORA_SPREADING_FACTOR_11 11
#define LORA_SPREADING_FACTOR_12 12

#define LORA_CODING_RATE_4_5     5
#define LORA_CODING_RATE_4_6     6
#define LORA_CODING_RATE_4_7     7
#define LORA_CODING_RATE_4_8     8

using namespace std;

std::unordered_map<uint64_t, ChaChaPoly> nodeEncryption;

std::list<std::pair<uint8_t, Packet>> recievedPackets = {};

size_t valid = 0;
size_t packets = 0;

WiFiClient client;

Timer<2, millis> timer;

uint8_t tempBuffer[sizeof(DataPacket)];

void setup() {

#if defined(DEBUG)

  Serial.begin(115200);

#endif

  heltec_setup();

  // Setup display
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.display();

  // Init encryption
  for (auto node : knownMonitors){
    nodeEncryption.insert({node.first, ChaChaPoly()});

    if (!nodeEncryption[node.first].setKey(node.second.key, 16)){
      throw("Could not set key!");
    }
    if (!nodeEncryption[node.first].setIV(node.second.iv, 8)){
      throw("Could not set IV!");
    }
  }

  // Init functions
  if (!LoRaInit()) {
    display.println("LoRa Failed");
    display.display();

    return;
  }

  heltec_delay(2000);

  int wifiStatus = WiFi.begin(WIFI_SSID, WIFI_PASSPHRASE);

  while (wifiStatus != WL_CONNECTED){
    delay(1000);
    switch (wifiStatus) {
    case WL_CONNECT_FAILED:
      display.println("WiFi Not Connected");
      display.display();
      break;
    default:
      break;
    }
    wifiStatus = WiFi.status();
  }

  radio.setPacketReceivedAction(onRecieve);
  radio.startReceive();

  timer.every(1e2, updateDisplay);

  for (auto node : knownMonitors){
    sendEventPacket(node.first, EventCode::NODE_ONLINE);
  }
}

void loop() {
  timer.tick();

  if (recievedPackets.size() == 0){
    heltec_delay(1);
    return;
  }

  uint8_t packetSize = recievedPackets.front().first;
  Packet& packet     = recievedPackets.front().second;

  if (!knownMonitors.contains(packet.id)){

#ifdef DEBUG
    Serial.printf("Unknown packet node: %llx.\n", packet.id);
#endif

    recievedPackets.pop_front();
    return;
  }

  switch (packetSize) {
  case sizeof(DataPacket):
    handleDataPacket(packet);
    break;
  default:
    handleEventPacket(packet);
    break;
  }

  recievedPackets.pop_front();

  // client.connect(HTTP_HOST, HTTP_PORT);

  // client.printf("POST /%s HTTP/1.1\n", peerUuids.at(packet.id).c_str());

  // client.printf("Host: %s\n", HTTP_HOST);
  // client.println("Connection: close");

  // size_t len = 43;

  // len++;
  // int8_t absValue;
  // if (packet.temperature < 0){
  //   len++;
  //   absValue = -1 * packet.temperature;
  // } else {
  //   absValue = packet.temperature;
  // }
  // for (int8_t comp = 10; comp < absValue; comp *= 10){
  //   len++;
  // }

  // len++;
  // for (uint8_t comp = 10; comp < packet.humidity; comp *= 10){
  //   len++;
  // }

  // len++;
  // for (uint16_t comp = 10; comp < packet.pressure; comp *= 10){
  //   len++;
  // }

  // client.print("Content-Length: ");
  // client.println(len);
  // client.println();

  // // Send JSON to stream
  // client.print("{");
  // client.printf("\"temperature\": %d,", packet.temperature);
  // client.printf("\"humidity\": %d,",    packet.humidity);
  // client.printf("\"pressure\": %d",     packet.pressure);
  // client.print("}");
}

void handleDataPacket(Packet& packet){
  nodeEncryption[packet.id].decrypt(
    (uint8_t*)&packet.type, 
    (uint8_t*)packet.type.encrypted, 
    sizeof(DataPacket)
  );

  if (!nodeEncryption[packet.id].checkTag(packet.tag, tagSize)){

#ifdef DEBUG
    Serial.printf("Tag check failed for node %llx.\n", packet.id);
#endif

    return;
  }

#if defined(DEBUG)
  Serial.printf("Temperature is %d\n", packet.type.data.temperature);
#endif

  sendEventPacket(packet.id, EventCode::DATA_RECVED);

  valid++;
}

void handleEventPacket(Packet& packet){
  ChaChaPoly newCrypt;

  if (!newCrypt.setKey(knownMonitors.at(packet.id).key, 16)){
    throw("Could not set key!");
  }
  if (!newCrypt.setIV(knownMonitors.at(packet.id).iv, 8)){
    throw("Could not set IV!");
  }

  newCrypt.decrypt(
    (uint8_t*)&packet.type, 
    (uint8_t*)packet.type.encrypted, 
    sizeof(EventPacket)
  ); 

  if (!newCrypt.checkTag(packet.tag, tagSize)){

#ifdef DEBUG
    Serial.printf("Tag check failed for reset from %llx.\n", packet.id);
#endif

    return;
  } 

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

bool LoRaInit(){
  int16_t res = radio.begin(
    LORA_FREQUENCY_US,
    LORA_BANDWIDTH_20_8,
    LORA_SPREADING_FACTOR_5,
    LORA_CODING_RATE_4_5
  );
  radio.setRxBoostedGainMode(true);

  return res == 0;
}

bool updateDisplay(void* cbData){
  display.clear();

  display.drawString(0,  0, WiFi.localIP().toString());
  display.drawString(0, 16, "Packets: " + String(packets));
  display.drawString(0, 32, "Valid: " + String(valid));

  display.display();

  return true;
}

void onRecieve(){
  packets++;

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

  recievedPackets.back().first = packetSize - idSize - tagSize;

  radio.readData(
    (uint8_t*)&recievedPackets.back().second, 
    packetSize
  );
}