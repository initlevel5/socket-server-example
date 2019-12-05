/*
 * Protocol specific declarations
 */
#ifndef PROTO_H_
#define PROTO_H_

#include <inttypes.h>

#define PACKET_MAC_SIZE (6)
#define PACKET_HEADER_SIZE (12)
#define PACKET_DATA_SIZE (1400)

enum packet_type {
    PTYPE_AUTH = 1,
    PTYPE_PING = 2,
    PTYPE_SENSOR_DATA = 3,
    PTYPE_FILE = 4,
    PTYPE_LOG = 5,
    PTYPE_NODE_PACKET = 6,
    PTYPE_BRIDGE_PACKET = 7,
    PTYPE_REAUTH = 8,
    PTYPE_CAM = 9,
    PTYPE_NODE_FILE = 10,
    PTYPE_VEND_FILE = 11,
    PTYPE_REGISTER_BRIDGE = 12,
    PTYPE_PURCHASE = 13,
    PTYPE_COUNT = 14,
};

struct packet {
    uint16_t len;
    uint16_t crc;
    uint8_t mac[PACKET_MAC_SIZE];
    uint8_t seq;
    uint8_t type;
    uint8_t data[PACKET_DATA_SIZE];
};

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

void make_crc16_table();
uint16_t get_crc16(const uint8_t *buf, uint16_t len);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* PROTO_H_ */
