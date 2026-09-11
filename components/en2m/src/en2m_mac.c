/**
 * @file en2m_mac.c
 * @brief MAC address string helpers.
 */

#include <stdio.h>

#include "en2m_proto.h"

void en2m_mac_to_str(const uint8_t mac[6], char buf[18])
{
    snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool en2m_mac_from_str(const char *str, uint8_t out[6])
{
    unsigned bytes[6];

    if (sscanf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
               &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return false;
    }

    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)bytes[i];
    }
    return true;
}
