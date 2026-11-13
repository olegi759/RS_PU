#ifndef __WEB_KEEP_ALIVE_H
#define __WEB_KEEP_ALIVE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool is_any_socket_established(uint8_t* sns, uint8_t length);
void init_socket_keepalive(uint8_t* sns, uint8_t length, uint8_t interval_units);
bool is_any_socket_keep_alive(uint8_t* sns, uint8_t length);
bool is_phy_link_alive();
#endif