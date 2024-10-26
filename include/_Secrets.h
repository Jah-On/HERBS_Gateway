/******************************************************************************

Rename this file to Secrets.h after modifying!

******************************************************************************/

// HERBS Data Types
#include <include/Herbs.h>

#define WIFI_SSID       ""
#define WIFI_PASSPHRASE ""
#define HTTP_HOST       ""
#define HTTP_PORT       1234

const std::unordered_map<uint64_t, MonitorEncryption> knownMonitors = {
  {0x0000000000000000, {"0000000000000000", "00000000"}}
};