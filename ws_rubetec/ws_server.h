#ifndef WS_SERVER_H
#define WS_SERVER_H

#include <stdint.h>


void ws_server_init(uint8_t ws_sock);
void ws_server_poll(void);

void ws_send_data(uint8_t *data, uint32_t len, uint8_t opcode);

void ws_start_waiting_new_msg();
#endif