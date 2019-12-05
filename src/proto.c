/*
 * Protocol specific implementations
 */
#include "proto.h"

static uint16_t crc_table[256];

void make_crc16_table() {
  uint16_t r;
  int s, s1;

  for (s = 0; s < 256; s++) {
    r = (uint16_t)s << 8;

    for (s1 = 0; s1 < 8; s1++) {
      if (r & (1 << 15))
        r = (r << 1) ^ 0x8005;
      else
        r = r << 1;
    }
    crc_table[s] = r;
  }
}

uint16_t get_crc16(const uint8_t *buf, uint16_t len) {
  uint16_t crc = 0xFFFF;

  while (len--) crc = crc_table[((crc>>8) ^ *buf++) & 0xFF] ^ (crc << 8);
  crc ^= 0xFFFF;

  return crc;
}
