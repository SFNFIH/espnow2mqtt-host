/**
 * @file en2m_proto.h
 * @brief On-air ESP-NOW packet format and MAC helpers.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EN2M_MAGIC   0xA5
#define EN2M_VERSION 2
#define EN2M_DATA_MAX 160

/** Mesh node roles (selected at init time). */
typedef enum {
    EN2M_ROLE_COORDINATOR = 1,
    EN2M_ROLE_ROUTER = 2,
    EN2M_ROLE_LEAF = 3,
} en2m_role_t;

/** On-air message types. */
typedef enum {
    EN2M_MSG_BEACON = 1,
    EN2M_MSG_HELLO = 2,
    EN2M_MSG_STATE = 3,
    EN2M_MSG_CMD = 4,
    EN2M_MSG_ACK = 5,
    EN2M_MSG_HEARTBEAT = 6,
} en2m_msg_type_t;

/** Packet flag bits. */
typedef enum {
    EN2M_FLAG_PAIRING = 0x01,
} en2m_flags_t;

/**
 * @brief Packed ESP-NOW mesh frame (must stay <= 250 bytes).
 */
typedef struct __attribute__((packed)) {
    uint8_t magic;
    uint8_t version;
    uint8_t msg_type;
    uint8_t role;
    uint8_t hop;
    uint8_t hop_limit;
    uint8_t cost;
    uint8_t flags;
    uint32_t seq;
    uint16_t cmd_id;
    uint8_t origin[6];
    uint8_t dest[6];
    uint8_t via[6];
    char model[12];
    char name[16];
    uint8_t data_len;
    uint8_t data[EN2M_DATA_MAX];
} en2m_pkt_t;

_Static_assert(sizeof(en2m_pkt_t) <= 250, "ESP-NOW packet too large");

static inline void en2m_mac_broadcast(uint8_t mac[6])
{
    memset(mac, 0xFF, 6);
}

static inline bool en2m_mac_is_broadcast(const uint8_t mac[6])
{
    return mac[0] == 0xFF && mac[1] == 0xFF && mac[2] == 0xFF &&
           mac[3] == 0xFF && mac[4] == 0xFF && mac[5] == 0xFF;
}

static inline bool en2m_mac_equal(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static inline void en2m_mac_copy(uint8_t dst[6], const uint8_t src[6])
{
    memcpy(dst, src, 6);
}

/**
 * @brief Format MAC as "AA:BB:CC:DD:EE:FF" into @p buf (18 bytes).
 */
void en2m_mac_to_str(const uint8_t mac[6], char buf[18]);

/**
 * @brief Parse "AA:BB:CC:DD:EE:FF" into @p out.
 * @return true on success
 */
bool en2m_mac_from_str(const char *str, uint8_t out[6]);

#ifdef __cplusplus
}
#endif
