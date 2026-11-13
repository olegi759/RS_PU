#include "web_keep_alive.h"
#include "wizchip_conf.h"
#include "net.h"
#include "user_timer.h"

// Функция для установки Keep-Alive таймера
static void set_socket_keepalive(uint8_t sn, uint8_t interval_units) {
    // interval_units = 0 -> выключено
    // interval_units = 1 -> ~5 сек, 2 -> ~10 сек и т.д.
    WIZCHIP_WRITE(Sn_KPALVTR(sn), interval_units);
}

bool is_any_socket_established(uint8_t* sns, uint8_t length){
    for(uint8_t i = 0; i < length; ++i){
        if(getSn_SR(i) == SOCK_ESTABLISHED) return true;
    }
    return false;
}

void init_socket_keepalive(uint8_t* sns, uint8_t length, uint8_t interval_units){
    for(uint8_t i = 0; i < length; ++i){
        set_socket_keepalive(sns[i], interval_units);
    }
}

bool is_any_socket_keep_alive(uint8_t* sns, uint8_t length){
    bool ret = false;
    for(uint8_t i = 0; i < length; ++i){
        uint8_t ir = getSn_IR(sns[i]);
        if (ir & Sn_IR_TIMEOUT) {
            setSn_IR(sns[i], Sn_IR_TIMEOUT);
        } else ret = true;
    }
    return ret;
}



bool is_phy_link_alive(){
    return  getPHYCFGR() == PHYCFGR_LNK_ON;
}