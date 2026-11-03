// Copy this file to src/secrets.h and fill in your networks. secrets.h is not committed.
// The clock tries each network in order and sets the time over NTP.
#pragma once
static const char* const WIFI_NETS[][2] = {
  { "your-network", "your-password" },
  { "another-network", "another-password" },
};
