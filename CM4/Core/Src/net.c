/*
 * net.c
 *
 *  Created on: 17 янв. 2025 г.
 *      Author: Rafael Boltachev
 */

#include "net.h"
#include <string.h>
#include "stm_util.h"
#include "dhcp.h"
#include <stdbool.h>
#include "webpage.h"
#include "loopback.h"
#include "setting.h"
#include "str_util.h"
#include "common.h"

#include "ws_server.h"
#include "web_keep_alive.h"

//#define USE_IO_LIBRARY	1

#ifdef USE_IO_LIBRARY
#include "httpServer.h"
#else
#include "http_server.h"
#endif

#define DHCP_SOCKET     0
//#define DNS_SOCKET      1
//#define HTTP_SOCKET     2

//#define SOCK_TCPS       0
//#define SOCK_UDPS       1
//#define PORT_TCPS		5000
//#define PORT_UDPS       3000

//ко-лво http сокетов
#define MAX_HTTPSOCK	4
//#define DATA_BUF_SIZE   1024

//const uint8_t socknumlist[MAX_HTTPSOCK] = {2, 3, 4, 5, 6, 7};
//const uint8_t socknumlist[MAX_HTTPSOCK] = {2, 3, 4, 5};
//const uint8_t socknumlist[MAX_HTTPSOCK] = {0, 1, 2, 3, 4, 5, 6, 7};
//const uint8_t socknumlist[MAX_HTTPSOCK] = {0, 1};

const uint8_t socknumlist[MAX_HTTPSOCK] = {0, 1, 2, 3};

#ifdef USE_IO_LIBRARY
static uint8_t RX_BUF[DATA_BUF_SIZE];
static uint8_t TX_BUF[DATA_BUF_SIZE];
#endif
// 1K should be enough, see https://forum.wiznet.io/t/topic/1612/2
//static uint8_t dhcp_buffer[DATA_BUF_SIZE];
//#define  dhcp_buffer TX_BUF
//static uint8_t dhcp_buffer[548];
static uint8_t * dhcp_buffer = NULL;


wiz_NetInfo net_info = {0};

static BOOL ip_assigned = FALSE;


uint32_t http_session_tick = 0;

void W5500_SelectMainChip(void)
{
	//ulog("CS0 main"CRLF);
    HAL_GPIO_WritePin(E_CS_SPI_GPIO_Port, E_CS_SPI_Pin, GPIO_PIN_RESET);
}

void W5500_SelectReserveChip(void)
{
	//ulog("CS0 reserve"CRLF);
    HAL_GPIO_WritePin(E_CS_SPI_GPIO_Port, E_CS_SPI_Pin, GPIO_PIN_RESET);
} 

void W5500_UnselectMainChip(void)
{
	//ulog("CS1"CRLF);
    HAL_GPIO_WritePin(E_CS_SPI_GPIO_Port, E_CS_SPI_Pin, GPIO_PIN_SET);
}
void W5500_UnselectReserveChip(void)
{
	//ulog("CS1"CRLF);
    HAL_GPIO_WritePin(E_CS_SPI_GPIO_Port, E_CS_SPI_Pin, GPIO_PIN_SET);
}

void W5500_Select(void)
{
	//ulog("CS0"CRLF);
    HAL_GPIO_WritePin(E_CS_SPI_GPIO_Port, E_CS_SPI_Pin, GPIO_PIN_RESET);
}

void W5500_Unselect(void)
{
	//ulog("CS1"CRLF);
    HAL_GPIO_WritePin(E_CS_SPI_GPIO_Port, E_CS_SPI_Pin, GPIO_PIN_SET);
}

void W5500_ReadBuff(uint8_t* buff, uint16_t len)
{
	//HAL_StatusTypeDef res;
    //char str[MAX_STR_SIZE] = {'\0'};

	//res =
			HAL_SPI_Receive(&hspi6, buff, len, 100);
    //mem2hexstr(buff, len, str, sizeof(str), " ");
	//if (len > 4) ulog_fmt("R=%d"CRLF,  len);

	//while (HAL_SPI_GetState(&hspi6) == HAL_SPI_STATE_BUSY_RX) {};
	// HAL_SPI_Receive_DMA(&hspi6, buff, len);
	 //while(HAL_SPI_GetState(&hspi6) == HAL_SPI_STATE_BUSY_RX);
}

//static uint8_t abuf[4096];
void W5500_WriteBuff(uint8_t* buff, uint16_t len)
{
	//HAL_StatusTypeDef res;
    //char str[MAX_STR_SIZE] = {'\0'};

    //res =
    		HAL_SPI_Transmit(&hspi6, buff, len, 100);
    //mem2hexstr(buff, len, str, sizeof(str), " ");
    //if (len > 4) ulog_fmt("W=%d"CRLF, len);

	//while(HAL_SPI_GetState(&hspi6) != HAL_SPI_STATE_READY) {};
	//memcpy(abuf, buff, MIN(len, sizeof(abuf)));
	//HAL_SPI_Transmit_IT(&hspi6, abuf, MIN(len, sizeof(abuf)));

	//while(HAL_SPI_GetState(&hspi6) == HAL_SPI_STATE_BUSY_TX);

}

uint8_t W5500_ReadByte(void)
{
    uint8_t byte;
    W5500_ReadBuff(&byte, sizeof(byte));
    return byte;
}

void W5500_WriteByte(uint8_t byte)
{
	//ulog("WB"CRLF);
    W5500_WriteBuff(&byte, sizeof(byte));
	//ulog("WE"CRLF);
}




void net_print_net_info(wiz_NetInfo * pnet_info)
{
    ulog_fmt(CONM"MAC: %02X:%02X:%02X:%02X:%02X:%02X"CRLF""CONM"IP:  %d.%d.%d.%d"CRLF""CONM"GW:  %d.%d.%d.%d"CRLF""CONM"Net: %d.%d.%d.%d"CRLF""CONM"DNS: %d.%d.%d.%d"CRLF""CONM"DHCP: %s"CRLF,
        pnet_info->mac[0], pnet_info->mac[1], pnet_info->mac[2], pnet_info->mac[3], pnet_info->mac[4], pnet_info->mac[5],
        pnet_info->ip[0], pnet_info->ip[1], pnet_info->ip[2], pnet_info->ip[3],
        pnet_info->gw[0], pnet_info->gw[1], pnet_info->gw[2], pnet_info->gw[3],
        pnet_info->sn[0], pnet_info->sn[1], pnet_info->sn[2], pnet_info->sn[3],
        pnet_info->dns[0], pnet_info->dns[1], pnet_info->dns[2], pnet_info->dns[3],
		(pnet_info->dhcp == NETINFO_DHCP ? "DYNAMIC" : "STATIC")
    );

}

void net_init_continue(void)
{


    ulog(CONM"net: Calling wizchip_setnetinfo()..."CRLF);
    wizchip_setnetinfo(&net_info);
    memset(&net_info, 0, sizeof(net_info));
    wizchip_getnetinfo(&net_info);
    net_print_net_info(&net_info);

#ifdef USE_IO_LIBRARY


    httpServer_init(TX_BUF, RX_BUF, MAX_HTTPSOCK, (uint8_t *)socknumlist);		// Tx/Rx buffers (1kB) / The number of W5500 chip H/W sockets in use
    reg_httpServer_cbfunc(NVIC_SystemReset, NULL); 					// Callback: NXP MCU Reset


	reg_httpServer_webContent((uint8_t *)"index.html", (uint8_t *)PAGE_INDEX_HTML);				//стартовая
	reg_httpServer_webContent((uint8_t *)"setting.html", (uint8_t *)PAGE_SETTING_HTML);			//общие настройки
	reg_httpServer_webContent((uint8_t *)"network.html", (uint8_t *)PAGE_NETWORK_HTML);			//сеть
	reg_httpServer_webContent((uint8_t *)"style.css", (uint8_t *)PAGE_STYLE_CSS);				//стиль
	reg_httpServer_webContent((uint8_t *)"params.js", (uint8_t *)PAGE_PARAMS_JS);				//AJAX
    reg_httpServer_webContent((uint8_t *)"control.html", (uint8_t *)PAGE_CONTROL_HTML);			//Контроль линий
    reg_httpServer_webContent((uint8_t *)"mode.html", (uint8_t *)PAGE_MODE_HTML);				//Режимы
    reg_httpServer_webContent((uint8_t *)"filemanager.html", (uint8_t *)PAGE_FILEMANAGER_HTML);	//Файл менеджер
    reg_httpServer_webContent((uint8_t *)"safety.html", (uint8_t *)PAGE_SAFETY_HTML);			//Безопасность
    reg_httpServer_webContent((uint8_t *)"auth.html", (uint8_t *)PAGE_AUTH_HTML);				//Вход в WEB
    reg_httpServer_webContent((uint8_t *)"netfunc.html", (uint8_t *)PAGE_NETFUNC_HTML);			//Сетеывые ф-ции



/*

	// Index page and netinfo / base64 image demo
	reg_httpServer_webContent((uint8_t *)"index.html", (uint8_t *)index_page);				// index.html 		: Main page example
	reg_httpServer_webContent((uint8_t *)"netinfo.html", (uint8_t *)netinfo_page);			// netinfo.html 	: Network information example page
	reg_httpServer_webContent((uint8_t *)"netinfo.js", (uint8_t *)wiz550web_netinfo_js);	// netinfo.js 		: JavaScript for Read Network configuration 	(+ ajax.js)
	reg_httpServer_webContent((uint8_t *)"img.html", (uint8_t *)img_page);					// img.html 		: Base64 Image data example page

	// Example #1
	reg_httpServer_webContent((uint8_t *)"dio.html", (uint8_t *)dio_page);					// dio.html 		: Digital I/O control example page
	reg_httpServer_webContent((uint8_t *)"dio.js", (uint8_t *)wiz550web_dio_js);			// dio.js 			: JavaScript for digital I/O control 	(+ ajax.js)

	// Example #2
	reg_httpServer_webContent((uint8_t *)"ain.html", (uint8_t *)ain_page);					// ain.html 		: Analog input monitor example page
	reg_httpServer_webContent((uint8_t *)"ain.js", (uint8_t *)wiz550web_ain_js);			// ain.js 			: JavaScript for Analog input monitor	(+ ajax.js)

	// Example #3
	reg_httpServer_webContent((uint8_t *)"ain_gauge.html", (uint8_t *)ain_gauge_page);		// ain_gauge.html 	: Analog input monitor example page; using Google Gauge chart
	reg_httpServer_webContent((uint8_t *)"ain_gauge.js", (uint8_t *)ain_gauge_js);			// ain_gauge.js 	: JavaScript for Google Gauge chart		(+ ajax.js)

	// AJAX JavaScript functions
	reg_httpServer_webContent((uint8_t *)"ajax.js", (uint8_t *)wiz550web_ajax_js);			// ajax.js			: JavaScript for AJAX request transfer
*/
	display_reg_webContent_list();
#else
	http_server_init();
	//ws_server_init(4, 8080, 0); // протокольный WS (ctx 0)
    //ws_server_init(5, 8081, 1); // heartbeat WS (ctx 1)
	//init_socket_keepalive(socknumlist, MAX_HTTPSOCK, 1);
#endif



}




void Callback_IPAssigned(void)
{
    ulog_fmt(CONM"net: Callback: IP assigned! Leased time: %d sec"CRLF,  getDHCPLeasetime());
    ip_assigned = TRUE;

    getSHAR(net_info.mac);
    getIPfromDHCP(net_info.ip);
    getGWfromDHCP(net_info.gw);
    getSNfromDHCP(net_info.sn);
    getDNSfromDHCP(net_info.dns);
    net_info.dhcp = NETINFO_DHCP;

    if (dhcp_buffer) {
    	free(dhcp_buffer);
    	dhcp_buffer = NULL;
    }

    net_init_continue();
}

void Callback_IPConflict(void)
{
    ulog(CONM"net: Callback: IP conflict!"CRLF);

    if (dhcp_buffer) {
    	free(dhcp_buffer);
    	dhcp_buffer = NULL;
    }
}


void net_task_execute(void)
{
	uint8_t i;

#ifdef USE_IO_LIBRARY
	for (i = 0; i < MAX_HTTPSOCK; i++) {
		httpServer_run(i); 	// HTTP Server handler
	}
#else
	for (i = 0; i < MAX_HTTPSOCK; i++) {
		http_server_run(socknumlist[i]);
	}
#endif

}

void net_task(void)
{
	if (ip_assigned) {
		net_task_execute();
		//loopback_tcps(SOCK_TCPS, RX_BUF, 5000);
	} else {
		DHCP_run();
	}

}

//void W5500_Critical_Enter(void) {}

//void W5500_Critical_Exit(void) {}

void net_init(void)
{

	//setting.dhcp = FALSE;


	//Далее распределяем память между сокетами. Пусть у каждого сокета будет по 2 Кб на прием и передачу:

	ulog(CONM"net: Registering W5500 callbacks..."CRLF);
	reg_wizchip_cs_cbfunc(W5500_Select, W5500_Unselect);
	reg_wizchip_spi_cbfunc(W5500_ReadByte, W5500_WriteByte);
	reg_wizchip_spiburst_cbfunc(W5500_ReadBuff, W5500_WriteBuff);

	ulog(CONM"net: Calling wizchip_init()..."CRLF);
	//uint8_t rx_tx_buff_sizes[] = {2, 2, 2, 2, 2, 2, 2, 2};
	uint8_t rx_tx_buff_sizes[] = {2, 4, 4, 4, 1, 1, 0, 0};
	if (wizchip_init(rx_tx_buff_sizes, rx_tx_buff_sizes) < 0) {
		ulog(CONM"net: Calling wizchip_init() error"CRLF);
	}
	uint8_t version = getVERSIONR(); // или прямой доступ через ioLibrary
    if (version != 0x04) {
        ulog_fmt(CONM"W5500 not detected! Version: 0x%02X\r\n", version);
        return;
    } else {
		ulog(CONM"net: Wizchip, running..."CRLF);
	}

	HAL_Delay(2000);
	uint8_t phycfgr = getPHYCFGR();
	if(phycfgr & 0x01){
		ulog(CONM"net: PHY Link - ok"CRLF);
	} else {
		ulog(CONM"net: PHY Link - error"CRLF);
	}
	if((phycfgr >> 1) & 0x01){
		ulog(CONM"net: PHY 100 Mb/s "CRLF);
	} else {
		ulog(CONM"net: PHY 10 Mb/s"CRLF);
	}
	if((phycfgr >> 2) & 0x01){
		ulog(CONM"net: PHY full-duplex"CRLF);
	} else {
		ulog(CONM"net: PHY half-duplex"CRLF);
	}
	//reg_wizchip_cris_cbfunc(W5500_Critical_Enter, W5500_Critical_Exit);


	wizchip_setinterruptmask(IK_SOCK_ALL); 

	if (setting.dhcp) {

		ip_assigned = FALSE;


		ulog(CONM"net: Registering DHCP callbacks..."CRLF);
		reg_dhcp_cbfunc(Callback_IPAssigned, Callback_IPAssigned, Callback_IPConflict);

		ulog(CONM"net: Calling DHCP_init()..."CRLF);
		// set MAC address before using DHCP
		setSHAR(setting.mac);
		dhcp_buffer = malloc(548);
		DHCP_init(DHCP_SOCKET, dhcp_buffer);


		ulog(CONM"net: Calling DHCP_run()..."CRLF);
	}
    else {

    	ip_assigned = TRUE;

    	memcpy(net_info.mac, setting.mac, sizeof(net_info.mac));
    	memcpy(net_info.ip, &setting.ip, sizeof(net_info.ip));
    	memcpy(net_info.gw, &setting.gw, sizeof(net_info.gw));
    	memcpy(net_info.sn, &setting.mask, sizeof(net_info.sn));
    	memcpy(net_info.dns, &setting.dns, sizeof(net_info.dns));
        net_info.dhcp = NETINFO_STATIC;


        net_init_continue();
    }

}



//void net_int_change(void)
//{
	//if (PIN_GET(E_INT_SPI)) {
		//net_int_state = 1;
	//} else {
	//	net_int_state = 2;
	//}

//}





