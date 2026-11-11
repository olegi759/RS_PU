#include <stdio.h>
#include <string.h>
#include "wizchip_conf.h"
#include "socket.h"
#include "ws_crypto.h"
#include "ws_server.h"

//#include "ulog.h"
#include "stm_util.h"

#include "ws_rubetec.h"

#define WS_PORT 8080
#define WS_BUF_SIZE 4096     // Максимальный размер собираемого сообщения
#define WS_TX_CHUNK 1024     // Размер куска при отправке (чтобы не переполнить буфер W5500)

static uint8_t ws_sock_number = 0xFF;

static uint8_t ws_rx_buf[2048]; // Буфер для чтения из W5500 (сырые TCP-данные)
static uint8_t ws_tx_buf[2048]; // Буфер для отправки в W5500

// Буферы для сборки сообщений
uint8_t  ws_msg_buf[WS_BUF_SIZE];
uint32_t ws_msg_len = 0;
static uint8_t  ws_ctrl_buf[125];
static uint8_t  ws_ctrl_len = 0;

// Состояние парсера входящего потока
static uint8_t  ws_header_buf[14];
static uint8_t  ws_header_cnt = 0;
static uint8_t  ws_header_needed = 2;
static uint8_t  ws_in_payload = 0;
static uint8_t  ws_opcode = 0;
static uint8_t  ws_fin = 0;
static uint8_t  ws_mask[4];
static uint8_t  ws_mask_idx = 0;
static uint64_t ws_payload_len = 0;
static uint64_t ws_payload_recv = 0;

// Флаги для защиты буфера от перезаписи
volatile uint8_t ws_msg_ready = 0;  
static uint8_t ws_discard_msg = 0;  

// Буфер для рукопожатия (защита от фрагментации HTTP)
static uint8_t ws_handshake_buf[512];
static uint16_t ws_handshake_len = 0;
static uint8_t ws_handshake_done = 0; 

// --- Инициализация сокета ---
void ws_server_init(uint8_t ws_sock) {
    if (socket(ws_sock, Sn_MR_TCP, WS_PORT, 0) == ws_sock) {
        ws_sock_number = ws_sock;
        listen(ws_sock_number);
#ifdef RUBETEC_LOG
        ulog("WS: listening on port 8080\r\n");
#endif
    }
}

// --- ОТПРАВКА: Функция с поддержкой фрагментации и Extended Length ---
void ws_send_data(uint8_t *data, uint32_t len, uint8_t opcode) {
    uint32_t sent = 0;
    
    if (len == 0) {
        uint8_t empty[2] = {0x80 | opcode, 0};
        send(ws_sock_number, empty, 2);
        return;
    }

    while (sent < len) {
        uint32_t chunk = len - sent;
        uint8_t fin = 0;
        
        if (chunk > WS_TX_CHUNK) {
            chunk = WS_TX_CHUNK;
            fin = 0;
        } else {
            fin = 1;
        }

        ws_tx_buf[0] = (fin ? 0x80 : 0x00) | (sent == 0 ? opcode : 0x00);
        uint32_t header_len = 2;
        
        if (chunk <= 125) {
            ws_tx_buf[1] = (uint8_t)chunk;
        } else if (chunk <= 65535) {
            ws_tx_buf[1] = 126;
            ws_tx_buf[2] = (chunk >> 8) & 0xFF;
            ws_tx_buf[3] = chunk & 0xFF;
            header_len = 4;
        } else {
            ws_tx_buf[1] = 127;
            ws_tx_buf[2] = 0; ws_tx_buf[3] = 0; ws_tx_buf[4] = 0; ws_tx_buf[5] = 0;
            ws_tx_buf[6] = (chunk >> 24) & 0xFF;
            ws_tx_buf[7] = (chunk >> 16) & 0xFF;
            ws_tx_buf[8] = (chunk >> 8) & 0xFF;
            ws_tx_buf[9] = chunk & 0xFF;
            header_len = 10;
        }

        memcpy(&ws_tx_buf[header_len], data + sent, chunk);
        
        // Ждем, пока в TX-буфере W5500 освободится место (защита от переполнения)
        uint32_t timeout = 1000;
        while (getSn_TX_FSR(ws_sock_number) < (header_len + chunk)) {
            if (--timeout == 0) {
#ifdef RUBETEC_LOG
                ulog("WS: TX timeout, connection might be lost\r\n");
#endif
                return;
            }
            HAL_Delay(1);
        }

        send(ws_sock_number, ws_tx_buf, header_len + chunk);
        sent += chunk;
    }
}

// --- ОТПРАВКА: Pong ---
void ws_send_pong(uint8_t *payload, uint16_t len) {
    if (len > 125) len = 125;
    ws_tx_buf[0] = 0x8A; // FIN=1, Opcode=0xA
    ws_tx_buf[1] = (uint8_t)len;
    
    // Копируем данные только если они реально есть
    if (len > 0 && payload != NULL) {
        memcpy(&ws_tx_buf[2], payload, len);
    }
    
    send(ws_sock_number, ws_tx_buf, len + 2);
}

// --- ПРИЕМ: Потоковый парсер (с поддержкой фрагментации) ---
void ws_process_data(uint8_t *data, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        // ==========================================
        // 1. ЧТЕНИЕ ЗАГОЛОВКА КАДРА
        // ==========================================
        if (!ws_in_payload) {
            ws_header_buf[ws_header_cnt++] = byte;

            if (ws_header_cnt == 2) {
                ws_fin = (ws_header_buf[0] & 0x80) >> 7;
                ws_opcode = ws_header_buf[0] & 0x0F;
                uint8_t masked = (ws_header_buf[1] & 0x80) >> 7;
                ws_payload_len = ws_header_buf[1] & 0x7F;

                if (ws_payload_len == 126)      ws_header_needed = 4;
                else if (ws_payload_len == 127) ws_header_needed = 10;
                else                            ws_header_needed = masked ? 6 : 2;
            }

            if (ws_header_cnt >= 4 && ws_header_needed == 4) {
                ws_payload_len = (ws_header_buf[2] << 8) | ws_header_buf[3];
                ws_header_needed = (ws_header_buf[1] & 0x80) ? 8 : 4;
            }

            if (ws_header_cnt >= 10 && ws_header_needed == 10) {
                ws_payload_len = 0;
                for(int j=0; j<8; j++) ws_payload_len = (ws_payload_len << 8) | ws_header_buf[2+j];
                ws_header_needed = (ws_header_buf[1] & 0x80) ? 14 : 10;
            }

            if (ws_header_cnt == ws_header_needed) {
                if (ws_header_buf[1] & 0x80) { 
                    memcpy(ws_mask, &ws_header_buf[ws_header_cnt - 4], 4);
                }
                ws_mask_idx = 0;
                ws_payload_recv = 0;
                
                if (ws_payload_len == 0) {
                    ws_in_payload = 0;
                    ws_header_cnt = 0;
                    
                    if (ws_opcode == 0x9) {          
                        ws_send_pong(NULL, 0);
                    } else if (ws_opcode == 0x8) {   
#ifdef RUBETEC_LOG
                        ulog("WS: Close received\r\n");
#endif
                        uint8_t close_frame[2] = {0x88, 0x00};
                        send(ws_sock_number, close_frame, 2);
                        disconnect(ws_sock_number);
                        return;
                    } else if (ws_opcode == 0x1) {   
                        ws_send_data(NULL, 0, 0x1); 
                    }
                } else {
                    ws_in_payload = 1;
                    
                    if (ws_opcode == 0x1 || ws_opcode == 0x2) {
                        if (ws_msg_ready) {
                            // --- ОТПРАВКА BUSY В ФОРМАТЕ JSON ---
                            ws_discard_msg = 1; 
#ifdef RUBETEC_LOG
                            ulog("WS: Busy! Discarding incoming text message.\r\n");
#endif
                            const char* busy_json = "{\"status\":\"busy\"}";
                            ws_send_data((uint8_t*)busy_json, strlen(busy_json), 0x1); 
                        } else {
                            ws_discard_msg = 0;
                            ws_msg_len = 0;
                        }
                    }
                }
            }
        } 
        // ==========================================
        // 2. ЧТЕНИЕ ДАННЫХ (PAYLOAD)
        // ==========================================
        else {
            if (ws_header_buf[1] & 0x80) { 
                byte ^= ws_mask[ws_mask_idx++ % 4];
            }

            if (ws_opcode & 0x08) {
                if (ws_ctrl_len < sizeof(ws_ctrl_buf)) ws_ctrl_buf[ws_ctrl_len++] = byte;
            } else {
                if (!ws_discard_msg) {
                    if (ws_msg_len < WS_BUF_SIZE) {
                        ws_msg_buf[ws_msg_len++] = byte;
                    }
                }
            }

            ws_payload_recv++;
            
            if (ws_payload_recv >= ws_payload_len) {
                ws_in_payload = 0;
                ws_header_cnt = 0;

                if (ws_opcode & 0x08) {
                    if (ws_opcode == 0x9) {
#ifdef RUBETEC_LOG
                        ulog("WS: Ping received, sending Pong\r\n");
#endif
                        ws_send_pong(ws_ctrl_buf, ws_ctrl_len);
                    } else if (ws_opcode == 0x8) {
#ifdef RUBETEC_LOG
                        ulog("WS: Close received\r\n");
#endif
                        uint8_t close_frame[2] = {0x88, 0x00};
                        send(ws_sock_number, close_frame, 2);
                        disconnect(ws_sock_number);
                        return;
                    }
                    ws_ctrl_len = 0;
                } else {
                    if (ws_fin) {
                        if (!ws_discard_msg) {
                            ws_msg_buf[ws_msg_len] = '\0'; 
                            ws_msg_ready = 1; 
#ifdef RUBETEC_LOG
                            ulog("WS: Message ready for main loop\r\n");
#endif
                        }
                        
                        if(try_parce_rubetec_msg(ws_msg_buf, ws_msg_len)){
#ifdef RUBETEC_LOG
                            ulog("WS: Message parsed\r\n");
#endif
                        } else {
#ifdef RUBETEC_LOG
                            ulog("WS: Message parse failed\r\n");
#endif
                        }
                    }
                }
            }
        }
    }
}

// --- Главный опрос сокета ---
void ws_server_poll(void) {
    uint8_t sr = getSn_SR(ws_sock_number);

    switch (sr) {
        case SOCK_ESTABLISHED:
            if (getSn_IR(ws_sock_number) & Sn_IR_CON) {
                setSn_IR(ws_sock_number, Sn_IR_CON);
#ifdef RUBETEC_LOG
                ulog("WS: Client connected\r\n");
#endif
            }

            uint16_t rx_len = getSn_RX_RSR(ws_sock_number);
            if (rx_len > 0) {
                int32_t ret = recv(ws_sock_number, ws_rx_buf, rx_len);
                if (ret > 0) {
                    if (!ws_handshake_done) {
                        // --- РЕЖИМ HANDSHAKE (Накапливаем данные) ---
                        if (ws_handshake_len + ret < sizeof(ws_handshake_buf)) {
                            memcpy(&ws_handshake_buf[ws_handshake_len], ws_rx_buf, ret);
                            ws_handshake_len += ret;
                            ws_handshake_buf[ws_handshake_len] = '\0'; // Null-terminate для strstr

                            // Ищем конец HTTP-заголовков
                            if (strstr((char*)ws_handshake_buf, "\r\n\r\n") != NULL) {
                                char* key_ptr = strstr((char*)ws_handshake_buf, "Sec-WebSocket-Key:");
                                if (key_ptr) {
                                    key_ptr += 18;
                                    while (*key_ptr == ' ') key_ptr++;
                                    char client_key[32] = {0};
                                    int i = 0;
                                    while (key_ptr[i] != '\r' && key_ptr[i] != '\0' && i < 31) {
                                        client_key[i] = key_ptr[i]; 
                                        i++;
                                    }
                                    
                                    // ПРОВЕРКА: ключ WebSocket всегда 24 байта (Base64)
                                    if (i != 24) {
                                        ulog("WS: Invalid Sec-WebSocket-Key length! Rejecting.\r\n");
                                        // Отправляем 400 Bad Request и закрываем сокет
                                        sprintf((char*)ws_tx_buf, "HTTP/1.1 400 Bad Request\r\n\r\n");
                                        send(ws_sock_number, ws_tx_buf, strlen((char*)ws_tx_buf));
                                        disconnect(ws_sock_number);
                                        return;
                                    }
                                    
                                    char magic_key[80]; // Теперь безопасно, так как 24 + 36 + 1 = 61 < 64
                                    sprintf(magic_key, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", client_key);
                                    uint8_t sha1_res[20];
                                    char accept_key[32];
                                    sha1_hash((uint8_t*)magic_key, strlen(magic_key), sha1_res);
                                    base64_encode(sha1_res, 20, accept_key);

                                    sprintf((char*)ws_tx_buf, 
                                        "HTTP/1.1 101 Switching Protocols\r\n"
                                        "Upgrade: websocket\r\n"
                                        "Connection: Upgrade\r\n"
                                        "Sec-WebSocket-Accept: %s\r\n\r\n", accept_key);
                                    
                                    send(ws_sock_number, ws_tx_buf, strlen((char*)ws_tx_buf));
#ifdef RUBETEC_LOG
                                    ulog("WS: Handshake complete!\r\n");
#endif
                                    ws_handshake_done = 1; // Переключились в режим WebSocket
                                } else {
#ifdef RUBETEC_LOG
                                    ulog("WS: Key not found in request!\r\n");
#endif
                                    ws_handshake_len = 0; 
                                }
                            }
                        } else {
#ifdef RUBETEC_LOG
                            ulog("WS: Handshake buffer overflow!\r\n");
#endif
                            ws_handshake_len = 0; 
                        }
                    } else {
                        // --- РЕЖИМ WEBSOCKET ---
                        ws_process_data(ws_rx_buf, ret);
                    }
                }
            }
            break;

        case SOCK_CLOSE_WAIT:
            disconnect(ws_sock_number);
            ws_msg_ready = 0; 
            ws_handshake_done = 0;   
            ws_handshake_len = 0;    
            break;

        case SOCK_CLOSED:
            ws_server_init(ws_sock_number); // ИСПРАВЛЕНО: добавлен аргумент
            ws_handshake_done = 0;   
            ws_handshake_len = 0;    
            break;
    }
}

void ws_start_waiting_new_msg(){
    ws_msg_ready = 0; 
}