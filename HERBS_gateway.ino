// Comment out for actual use to save energy
// Otherwise it will print useful info over serial
// #define DEBUG 

// STL Includes
#include <chrono>        // Time related
#include <list>          // Front/Back optimized lists
#include <unordered_map> // Performant hash table

// Arduino Includes
#include <float16.h>
#include <timers.h>

// External Includes
#include <arduino-timer.h>
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

void createUuidFromU64(uint64_t raw);

using namespace std;

typedef struct PacketData {
  uint64_t id;
  int8_t   temperature; // Celcius
  uint8_t  humidity;    // Percentage from 0 to 100
  uint16_t preassure;   // Millibars
} PacketData;

std::list<PacketData> recievedPackets = {};

size_t valid = 0;
size_t packets = 0;

WiFiClient client;

Timer<2, millis> timer;

unordered_map<uint64_t, String> peerUuids = {};

const unordered_map<uint8_t, char> U8_AS_CHAR = {
  {0x00, '0'}, {0x01, '1'}, {0x02, '2'}, {0x03, '3'},
  {0x04, '4'}, {0x05, '5'}, {0x06, '6'}, {0x07, '7'},
  {0x08, '8'}, {0x09, '9'}, {0x0A, 'a'}, {0x0B, 'b'},
  {0x0C, 'c'}, {0x0D, 'd'}, {0x0E, 'e'}, {0x0F, 'f'}
};

void setup() {

#if defined(DEBUG)

  Serial.begin(115200);

#endif

  heltec_setup();

  // Setup display
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.display();

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

  timer.every(1e2, updateDisplay);

  radio.setPacketReceivedAction(onRecieve);

  radio.startReceive();
}


void loop() {
  timer.tick();

  if (recievedPackets.size() == 0){
    heltec_delay(1);
    return;
  }

  PacketData packet = recievedPackets.front();

  if (!peerUuids.contains(packet.id)){
    createUuidFromU64(packet.id);
  }

  client.connect(HTTP_HOST, HTTP_PORT);

  client.printf("POST /%s HTTP/1.1\n", peerUuids.at(packet.id).c_str());

  client.printf("Host: %s\n", HTTP_HOST);
  client.println("Connection: close");

  size_t len = 43;

  len++;
  int8_t absValue;
  if (packet.temperature < 0){
    len++;
    absValue = -1 * packet.temperature;
  } else {
    absValue = packet.temperature;
  }
  for (int8_t comp = 10; comp < absValue; comp *= 10){
    len++;
  }

  len++;
  for (uint8_t comp = 10; comp < packet.humidity; comp *= 10){
    len++;
  }

  len++;
  for (uint16_t comp = 10; comp < packet.preassure; comp *= 10){
    len++;
  }

  client.print("Content-Length: ");
  client.println(len);
  client.println();

  // Send JSON to stream
  client.print("{");
  client.printf("\"temperature\": %d,", packet.temperature);
  client.printf("\"humidity\": %d,",    packet.humidity);
  client.printf("\"preassure\": %d",    packet.preassure);
  client.print("}");

  recievedPackets.pop_front();

#if defined(DEBUG)
  Serial.printf("Temperature is %d\n", packet.temperature);
#endif
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

  PacketData rcvdPacket;

  if (radio.getPacketLength() != sizeof(PacketData)) return;

  radio.readData((uint8_t*)&rcvdPacket, sizeof(PacketData));

  recievedPackets.push_back(rcvdPacket);

  valid++;
}

void createUuidFromU64(uint64_t raw){
  String uuid = "00000000-0000-0000-0000-000000000000";
  size_t strIndex = 0;
  for (int8_t bitIndex = 60; bitIndex >= 0; bitIndex -= 4){
    switch (strIndex) {
    case 8:
    case 13:
      strIndex++;
    default:
      break;
    }
    uuid[strIndex++] = U8_AS_CHAR.at((raw >> bitIndex) & 0xF);
  }
  peerUuids.insert({raw, uuid});
}
