#ifndef HERBS_TYPES_H
#define HERBS_TYPES_H

#include <stdint.h>
#include <float16.h>

typedef struct MonitorEncryption {
  uint8_t key[17];
  uint8_t iv[9];
} MonitorEncryption;

typedef enum class EventCode : uint8_t {
  NODE_ONLINE = 0,
  DATA_RECVED = 1
} EventCode;

typedef struct DataPacket {
  int8_t   temperature;  // Celcius
  uint8_t  humidity;     // Percentage from 0 to 100
  uint16_t pressure;    // Millibars
  float16  acoustics;    // Decibels
  uint16_t hiveMass;     // Grams
} DataPacket;

typedef struct EventPacket {
  EventCode eventCode;
} EventPacket;

const size_t tagSize         = 8;
const size_t idSize          = sizeof(uint64_t);
const size_t dataPacketSize  = idSize + tagSize + sizeof(DataPacket);
const size_t eventPacketSize = idSize + tagSize + sizeof(EventPacket);

#pragma pack(push, 1)
typedef struct Packet {
  uint64_t id;
  uint8_t  tag[tagSize];
  union {
    DataPacket   data = {};
    EventPacket  event;
    uint8_t      encrypted[sizeof(DataPacket)];
  } type;
} Packet;
#pragma pack(pop)

#endif