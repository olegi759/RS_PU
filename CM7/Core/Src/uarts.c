/*
 * uart.c
 *
 *  Created on: 6 мар. 2024 г.
 *      Author: Rafael Boltachev
 */


#include "uarts.h"
#include <string.h>
#include <stdio.h>
#include "math_util.h"
#include "stm_util.h"
#include "timer_util.h"
#include "str_util.h"
#include "opus_util.h"
#include "pcm1753.h"
#include "pd.h"
#include "pd_data.h"
#include "playopus.h"
#include "mchs_mic.h"
#include "net_util.h"
#include "pu.h"
#include "cm7core.h"
#include "fault_data.h"
#include "common.h"
#include "playmp3.h"
#include "opus_buffer.h"

//RS1 это UART4

//#define DEBUG_UART4_UART7	1
//#define DEBUG_UART4_UART7_ONLY		  PD_CMD_SPISOK_PU


//время принятие пакета по кольцу
static uint32_t rcv_ring_tick[9]	=	{0};

dynamic_buf_t uarts_rx_buf = DYNAMIC_BUF_STATIC_INIT(60 * 1024) ;

#define MAX_COUNT_PACKET_SIZE	128
#define MAX_SOUND_PACKET_SIZE	(OPUS_MAX_ENCODE_SIZE + 10)

static uint8_t p2_sound_rx_buf[MAX_SOUND_PACKET_SIZE];
static uint8_t p1_sound_rx_buf[MAX_SOUND_PACKET_SIZE];
static uint8_t ext1_count_rx_buf[MAX_COUNT_PACKET_SIZE];
static uint8_t ext1_sound_rx_buf[MAX_SOUND_PACKET_SIZE];
static uint8_t ext2_count_rx_buf[MAX_COUNT_PACKET_SIZE];
static uint8_t ext2_sound_rx_buf[MAX_SOUND_PACKET_SIZE];

static uint8_t p2_sound_tx_buf[MAX_SOUND_PACKET_SIZE];
static uint8_t p1_sound_tx_buf[MAX_SOUND_PACKET_SIZE];
static uint8_t ext1_count_tx_buf[MAX_COUNT_PACKET_SIZE];
static uint8_t ext1_sound_tx_buf[MAX_SOUND_PACKET_SIZE];
static uint8_t ext2_count_tx_buf[MAX_COUNT_PACKET_SIZE];
static uint8_t ext2_sound_tx_buf[MAX_SOUND_PACKET_SIZE];


uint32_t recv_opus_cnt = 0;


static uint16_t sound_tx_id = 0;
static uint16_t sound_rx_id = 0;

uint16_t count_tx_id = 0;
static uint16_t count_rx_id = 0;

//время прихода пакета по AUDIO uart8 или uart5
//static uint32_t uart8_uart5_active_tick = 0;

//через какой uart будет идти комманда
//static UART_HandleTypeDef * uarts_transmit_cmd_huart = &huart4;


static uint32_t uart8_uart5_snd_ff_tick = 0;
static uint32_t uart4_uart7_snd_ff_tick = 0;


typedef struct __huart_param {
	UART_HandleTypeDef *huart;
	uint8_t number;
	uint8_t * rx_buf;
	uint16_t rx_buf_size;
	uint8_t * tx_buf;
	uint16_t tx_buf_size;
} huart_param_t;

const huart_param_t huart_params[6] = {
		{ &huart1, 1, p2_sound_rx_buf, sizeof(p2_sound_rx_buf), p2_sound_tx_buf, sizeof(p2_sound_tx_buf) },
		{ &huart3, 3, p1_sound_rx_buf, sizeof(p1_sound_rx_buf), p1_sound_tx_buf, sizeof(p1_sound_tx_buf) },
		{ &huart4, 4, ext1_count_rx_buf, sizeof(ext1_count_rx_buf), ext1_count_tx_buf, sizeof(ext1_count_tx_buf) },
		{ &huart5, 5, ext1_sound_rx_buf, sizeof(ext1_sound_rx_buf), ext1_sound_tx_buf, sizeof(ext1_sound_tx_buf) },
		{ &huart7, 7, ext2_count_rx_buf, sizeof(ext2_count_rx_buf), ext2_count_tx_buf, sizeof(ext2_count_tx_buf) },
		{ &huart8, 8, ext2_sound_rx_buf, sizeof(ext2_sound_rx_buf), ext2_sound_tx_buf, sizeof(ext2_sound_tx_buf) }
};


const huart_param_t * get_huart_param_by_handle(UART_HandleTypeDef *huart)
{
	int i;

	for (i = 0; i < 6; i++) {
		if (huart_params[i].huart == huart) return &huart_params[i];
	}
	return NULL;
}

const huart_param_t * get_huart_param_by_number(uint8_t number)
{
	int i;

	for (i = 0; i < 6; i++) {
		if (huart_params[i].number == number) return &huart_params[i];
	}
	return NULL;
}

void uarts_abort_huart(UART_HandleTypeDef * huart)
{
	HAL_UART_AbortReceive_IT(huart);
	HAL_UART_AbortReceive(huart);
	HAL_UART_Abort(huart);
}

void UART_flush(UART_HandleTypeDef *huart) {
  // Flush UART RX buffer if RXNE is set
  if READ_BIT(huart->Instance->ISR, UART_FLAG_RXNE) {
    SET_BIT(huart->Instance->RQR, UART_RXDATA_FLUSH_REQUEST);
  }

  // Not available on F030xx devices!
  SET_BIT(huart->Instance->RQR, UART_TXDATA_FLUSH_REQUEST);

  // Clear All Errors (if needed)
  if (READ_BIT(huart->Instance->ISR, UART_FLAG_ORE | UART_FLAG_FE | UART_FLAG_NE)) {

	__HAL_UART_CLEAR_PEFLAG(huart);
	__HAL_UART_CLEAR_OREFLAG(huart);
	__HAL_UART_CLEAR_FEFLAG(huart);
	__HAL_UART_CLEAR_NEFLAG(huart);
	__HAL_UART_CLEAR_IDLEFLAG(huart);
	__HAL_UART_CLEAR_TXFECF(huart);

  }
}

void uarts_ReceiveToIdle_IT_by_param(const huart_param_t * huart_param)
{
	//HAL_UARTEx_ReceiveToIdle_IT(huart_param->huart, huart_param->rx_buf, huart_param->rx_buf_size);

	HAL_StatusTypeDef res;

	//res = HAL_UARTEx_ReceiveToIdle_DMA(huart_param->huart, huart_param->rx_buf, huart_param->rx_buf_size);
	res = HAL_UARTEx_ReceiveToIdle_IT(huart_param->huart, huart_param->rx_buf, huart_param->rx_buf_size);

	if (res != HAL_OK) {
		ulog_fmt(CONM"ERROR HAL_UARTEx_ReceiveToIdle(%d)=%d"CRLF, huart_param->number, res);

		uarts_abort_huart(huart_param->huart);
		UART_flush(huart_param->huart);

		//HAL_UARTEx_ReceiveToIdle_DMA(huart_param->huart, huart_param->rx_buf, huart_param->rx_buf_size);
		HAL_UARTEx_ReceiveToIdle_IT(huart_param->huart, huart_param->rx_buf, huart_param->rx_buf_size);

	}
}




void uarts_transmit_it_by_param(const huart_param_t * huart_param, uint8_t * packet, uint16_t packet_len)
{
	uint16_t len;





	//if (huart_param->huart->gState == HAL_UART_STATE_BUSY_TX) {
	//	ulog_fmt(CONM"uart busy:%d"CRLF,  huart_param->number);
		HAL_UART_AbortTransmit(huart_param->huart);
		//return;
	//}


	len = packet_len < huart_param->tx_buf_size ? packet_len : huart_param->tx_buf_size;
	memcpy(huart_param->tx_buf, packet, len);

	HAL_UART_Transmit_DMA(huart_param->huart, huart_param->tx_buf, len);
	//uarts_transmit_it_tick = HAL_GetTick();


	//if (packet[0] == 0xFF) { //если пакет широковещательный
	//	rcv_ring_tick[huart_param->number] = HAL_GetTickNoZero();
	//}

#ifdef DEBUG_UART4_UART7

	if ((huart_param->number == 4) || (huart_param->number == 7)) {

#ifdef DEBUG_UART4_UART7_ONLY
		if (packet[4] == DEBUG_UART4_UART7_ONLY) {
#endif
			ulog_fmt(CONM"t4_7(%d %d) "CRLF, huart_param->number,  packet_len);
			ulogmem(packet, packet_len, FALSE);
			ulog(CRLF);
#ifdef DEBUG_UART4_UART7_ONLY
		}
#endif
	}
#endif

}

void uarts_transmit_it(UART_HandleTypeDef *huart, uint8_t * packet, uint16_t packet_len)
{
	const huart_param_t * huart_param;

	huart_param = get_huart_param_by_handle(huart);
	if (!huart_param) return;

	uarts_transmit_it_by_param(huart_param, packet, packet_len);
}

void uarts_transmit_it_by_num(uint8_t huart_number, uint8_t * packet, uint16_t packet_len)
{
	const huart_param_t * huart_param;


	huart_param = get_huart_param_by_number(huart_number);
	if (!huart_param) return;

	uarts_transmit_it_by_param(huart_param, packet, packet_len);
}





void pd_check_audio_param(uint8_t cmd)
{
	uint32_t frequency;
	BOOL stereo;

	if (!pd_get_audio_param_from_command(cmd, &frequency, &stereo)) {
		ulog_fmt(CONM"error cmd:%d"CRLF, cmd);
		return;
	}

	if ((pcm1753_GetFrequency() == frequency) && (pcm1753_GetStereo() == stereo)) return;

	ulog_fmt(CONM"pd change audio param cmd:%d f:%lu s:%s"CRLF, cmd, pcm1753_GetFrequency(), bool2str(pcm1753_GetStereo()));
	pu_prepare_audio_play(frequency, stereo);

}

/*
//посылка по протоколу комманды через uart4 или uart7
void uarts_transmit_piv(uint8_t huart_number, uint8_t daddr, uint8_t saddr, uint8_t cmd, uint8_t * cmd_data, uint16_t cmd_data_len)
{
	uint8_t packet[5 + cmd_data_len + 2];
	uint16_t packet_len;
	packet_len = create_piv_packet_cmd(packet, daddr, saddr, cmd, cmd_data, cmd_data_len);
	uarts_transmit_it_by_num(huart_number,  packet, packet_len);
}
*/

void uarts_transmit_piv2(uint8_t huart_number, uint8_t daddr, uint8_t saddr, uint8_t cmd, uint8_t * cmd_data, uint16_t cmd_data_len, uint16_t packet_id)
{
	uint8_t packet[5 + cmd_data_len + 2];
	uint16_t packet_len;
	packet_len = create_piv_packet_cmd2(packet, daddr, saddr, cmd, cmd_data, cmd_data_len, packet_id);
	uarts_transmit_it_by_num(huart_number,  packet, packet_len);
}


void uarts_transmit_piv(uint8_t huart_number, uint8_t daddr, uint8_t saddr, uint8_t cmd, uint8_t * cmd_data, uint16_t cmd_data_len)
{

	if ((huart_number == 5) || (huart_number == 8) ) {
		sound_tx_id++;
		uarts_transmit_piv2(huart_number, daddr, saddr, cmd, cmd_data, cmd_data_len, sound_tx_id);
	}
	else if ((huart_number == 4) || (huart_number == 7) ) {
		count_tx_id++;
		uarts_transmit_piv2(huart_number, daddr, saddr, cmd, cmd_data, cmd_data_len, count_tx_id);
	}
}


void HAL_UARTS_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
	const huart_param_t * huart_param;



	huart_param = get_huart_param_by_handle(huart);
	if (!huart_param) return;

	//ulog_fmt("r(%d:%d)"CRLF, huart_param->number, Size);

	if (Size < 7) return;

	//if (Size < 7) {
	//	if ((huart_param->number == 4) || (huart_param->number == 7)) return; //Комманды от uart4 и uart7 слева и справа
	//	if ((huart_param->number == 8) || (huart_param->number == 5)) return; //Звук от uart8 и uart5 Слева и справа
	//}


	huart_param->rx_buf[Size] = huart_param->number;

	uarts_rx_buf.busy = TRUE;
	dynamic_buf_push(&uarts_rx_buf, huart_param->rx_buf, Size + 1);
	uarts_rx_buf.busy = FALSE;

	uarts_ReceiveToIdle_IT_by_param(huart_param);
}




// ------------------------------------------------------------------------------
// -------------------------------- UART4 AND UART7 -----------------------------
// ------------------------------------------------------------------------------


//время приема пакета по кольцу 0 - UART4, 1 - UART7
//static uint32_t uart4_uart7_rcv_ring_tick[2]	=	{0};



//на какой uart4 или uart7 приходит комманда
//static uint8_t uart4_uart7_rcv_huart_number[26] = { [0 ... 25] = 0 };
//static uint32_t uart4_uart7_rcv_huart_number_tick[26] = { [0 ... 25] = 0 };







static uint8_t pu_cmd_change_mode_state = 0;
static uint8_t pu_cmd_change_mode_addr = 0;



void uart4_uart7_snd(uint8_t * packet, uint16_t packet_len)
{
	uarts_transmit_it_by_num(4, packet, packet_len);
	uarts_transmit_it_by_num(7, packet, packet_len);

	if (packet[0] == 0xFF) uart4_uart7_snd_ff_tick = HAL_GetTickNoZero();
}

void uart4_uart7_snd_piv(uint8_t daddr, uint8_t saddr, uint8_t cmd, uint8_t * cmd_data, uint16_t cmd_data_len)
{

	uint8_t packet[5 + cmd_data_len + 2];
	uint16_t packet_len;

	count_tx_id++;
	packet_len = create_piv_packet_cmd2(packet, daddr, saddr, cmd, cmd_data, cmd_data_len, count_tx_id);

	uart4_uart7_snd(packet, packet_len);
}


void uart4_uart7_recv_change_mode(uint8_t * packet, uint16_t packet_len)
{

	uint32_t lines, flines;
	int i;
	uint8_t saddr_index;
	char file_name[64];
	pu_small_info_t * pu_small_info;


	pu_small_info = (pu_small_info_t *)&packet[5];

	saddr_index = pu_small_info->addr / 10;



	if (pu_small_info->mode != pu_small_infos[saddr_index].mode) {
		ulog_fmt(CONM"PU%d CHANGE MODE %s->%s"CRLF, pu_small_info->addr, pu_mode2str(pu_small_infos[saddr_index].mode), pu_mode2str(pu_small_info->mode));
	}



	switch (pu_small_info->mode) {
		case PU_MODE_VESHANIE_MCHS:
			if (!setting.netfunc_mchs.pu_addr) break;
			if (!setting.netfunc_mchs.sound_lines) break;
			if (setting.netfunc_mchs.pu_addr != pu_small_info->addr) break;
			if (pu_cmd_change_mode_state) break;

			pu_prepare_audio_play(MCHS_MIC_AUDIO_FREQUENCY, FALSE);
			pu_enable_sound_lines(setting.netfunc_mchs.sound_lines);
			pu_cmd_change_mode_state = 1;
			pu_cmd_change_mode_addr = pu_small_info->addr;
			ulog_fmt(CONM"cmd_change_state saddr:%d mode:PU_MODE_VESHANIE_MCHS"CRLF, pu_small_info->addr);

		break;
		case PU_MODE_VESHANIE_POJAR:

			if (pu_cmd_change_mode_state) break;

			flines = pu_small_info->lines;


			lines = 0;
			for (i = 1; i <= 10; i++) {
				if (!setting.netfunc_fire[i].pu_addr || !setting.netfunc_fire[i].fire_line || !setting.netfunc_fire[i].sound_lines || (setting.netfunc_fire[i].file[0] == '\0')) continue;
				if (setting.netfunc_fire[i].pu_addr != pu_small_info->addr) continue;
				if (!bitget(&flines, (setting.netfunc_fire[i].fire_line - 1))) continue;
				strcpy_s(file_name, sizeof(file_name), setting.netfunc_fire[i].file);
				lines |= setting.netfunc_fire[i].sound_lines;
			}

			if (!lines) break;

			if (!cm7core_file_exist(file_name)) break;

			pu_enable_sound_lines(lines);

			pu_play_audio_file(file_name);
			pu_cmd_change_mode_state = 2;
			pu_cmd_change_mode_addr = pu_small_info->addr;
			ulog_fmt("cmd_change_state saddr:%d mode:PU_MODE_VESHANIE_POJAR fire_lines:%lu"CRLF, pu_small_info->addr, flines);

		break;
		default:
			if (pu_cmd_change_mode_state) {

				if (pu_small_info->addr == pu_cmd_change_mode_addr) {

					if (pu_cmd_change_mode_state == 2) {
						playopus_stop();
						playmp3_stop();
					}
					pu_enable_sound_lines(0);
					pu_cmd_change_mode_state = 0;
					pu_cmd_change_mode_addr = 0;
				}
			}
		break;

	}



	pu_small_infos[saddr_index].addr = pu_small_info->addr;
	pu_small_infos[saddr_index].lines_count = pu_small_info->lines_count;
	pu_small_infos[saddr_index].mode = pu_small_info->mode;
	pu_small_infos[saddr_index].lines = pu_small_info->lines;
	pu_small_infos_tick[saddr_index] = HAL_GetTickNoZero();

}


void uart4_uart7_rcv_spisok_pu(uint8_t huart_number, uint8_t * packet, uint16_t packet_len)
{
	uint8_t buf[MAX_PACKET_SIZE];
	int i, j;
	pu_small_info_t  * pu_small_info;
	uint8_t saddr_index;


	i = 5;
	while (i < packet_len - 2) {
		pu_small_info = (pu_small_info_t  *)&packet[i];
		saddr_index = pu_small_info->addr / 10;

		pu_small_infos[saddr_index].addr = pu_small_info->addr;
		pu_small_infos[saddr_index].lines_count = pu_small_info->lines_count;
		pu_small_infos[saddr_index].mode = pu_small_info->mode;
		pu_small_infos[saddr_index].lines = pu_small_info->lines;


		i += sizeof(pu_small_info_t);
	}


	j = 0;
	for (i = 5; i < packet_len - 2; i++) {
		buf[j] = packet[i];
		j++;
	}


	pu_get_local_small_info((pu_small_info_t  *)&buf[j]);
	j += sizeof(pu_small_info_t);


	uarts_transmit_piv( (huart_number == 4 ? 7 : 4), packet[0], packet[1] , PD_CMD_SPISOK_PU, buf, j);


}

/*

void uart4_uart7_ring_check(void)
{
	if (rcv_ring_tick[4] < 20000) { //UART4
		cm7core_fault_unset(FAULT_PU_RING_CMD);
	}
	else if (rcv_ring_tick[7] < 20000) { //UART7
		cm7core_fault_unset(FAULT_PU_RING_CMD);
	}
	else { //разрыв кольца
		cm7core_fault_set(FAULT_PU_RING_CMD);
	}
}

*/


//Комманды от uart4 и uart7 слева и справа
void uart4_uart7_rcv(uint8_t huart_number, uint8_t * packet, uint16_t packet_len)
{

	uint16_t rx_id;

	#ifdef DEBUG_UART4_UART7

#ifdef DEBUG_UART4_UART7_ONLY
	if (packet[4] == DEBUG_UART4_UART7_ONLY) {
#endif
	ulog_fmt("r4_7(%d %d) "CRLF, huart_number,  packet_len);
	ulogmem(packet, packet_len, FALSE);
	ulog(CRLF);
#ifdef DEBUG_UART4_UART7_ONLY
}
#endif

#endif

	if (!is_correct_crc16(packet, packet_len)) return;




	if ( packet[1] == setting.uart_local_addr ) { //пакет от себя
		rcv_ring_tick[huart_number] = HAL_GetTick();
		return;
	}

	if ( (packet[0] == setting.uart_local_addr + 1) || (packet[0] == setting.uart_local_addr + 2) ) { //пакет из сети к моим ПД

		rx_id = (packet[2] << 8) | packet[3];
		if (rx_id) {
			if (rx_id <= count_rx_id) { //дублирующий пакет
				if (count_rx_id - rx_id < 100) return;
			}
		}
		count_rx_id = rx_id;

		packet[0] -= setting.uart_local_addr;
		rx_id = packet_len - 6;
		packet[2] = rx_id >> 8;
		packet[3] = rx_id & 0xFF;
		fill_crc16(packet, packet_len);
		cm7core_pd_send(packet, packet_len); //пересылаем на ПД
		return;
	}

	if ((packet[0] != setting.uart_local_addr) && (packet[0] != 0xFF) ) { //пакет не мне
		uarts_transmit_it_by_num((huart_number == 4 ? 7 : 4), packet, packet_len); //не свой пакет передаем дальше через шлюз
		return;
	}

	//тут пакеты мне или широковещательныу

	rx_id = (packet[2] << 8) | packet[3];
	if (rx_id) {
		if (rx_id <= count_rx_id) { //дублирующий пакет
			if (count_rx_id - rx_id < 100) return;
		}
	} else {
		if (!count_rx_id) return;
	}
	count_rx_id = rx_id;




	//ulog_fmt("rcv_4_7_0xFF"CRLF);
	if (packet[0] == 0xFF) {

		switch (packet[4]) {

			case PU_CMD_CHANGE_MODE:
				uart4_uart7_recv_change_mode(packet, packet_len);
			break;

			case PU_CMD_NET_INFO_UPDATE:
				pu_netinfo[0] = packet[5];
				pu_netinfo[1] = packet[6];
				pu_netinfo[2] = packet[7];
			break;


			//case PD_CMD_PING:
			//	uarts_transmit_piv(huart_number, packet[1], setting.uart_local_addr, PD_CMD_PONG, NULL, 0);
			//	ulog_fmt(CONM"tc(%d)-> PONG"CRLF, huart_number, packet[1]);
			//break;

			//case PD_CMD_SPISOK_PU:

			//	if ( (packet[1] == setting.uart_local_addr + 1) || (packet[1] == setting.uart_local_addr + 2) ) { //пакет из сети к моим ПД
			//		packet[0] = packet[1] - setting.uart_local_addr;
			//		packet[1] = setting.uart_local_addr;
			//		packet[4] = PD_CMD_SPISOK_PU_RES;
			//		fill_crc16(packet, packet_len);
			//		cm7core_pd_send(packet, packet_len);
			//		return;
			//	}

				//uart4_uart7_rcv_spisok_pu(huart_number, packet, packet_len);

			//return;
		}
	}

	if (packet[0] == setting.uart_local_addr) {
		//в pd_rcv входим только с локальным адресом
		pd_rcv(packet, packet_len, huart_number);
	} else {
		uarts_transmit_it_by_num((huart_number == 4 ? 7 : 4), packet, packet_len); //не свой пакет передаем дальше через шлюз
	}

}




void uarts_transmit_dejurniy(uint8_t daddr)
{
	uart4_uart7_snd_piv(daddr, setting.uart_local_addr, PD_CMD_DEJURNIY, NULL, 0);

}
void uarts_transmit_veshanie_pd(uint8_t daddr, uint32_t frequency, BOOL stereo, uint32_t sound_lines)
{
	pd_audio_param_cmd_data_t pd_audio_data;

	pd_audio_data.frequency = HTONL(frequency);
    pd_audio_data.stereo = stereo;
	pd_audio_data.sound_lines = sound_lines;

	uart4_uart7_snd_piv(daddr, setting.uart_local_addr, PD_CMD_VESHANIE_PD, (uint8_t *)&pd_audio_data, sizeof(pd_audio_data));
}


//посылка для netfun сетевых ф-ций PU_CMD_CHANGE_STATE
void uarts_transmit_change_mode(void)
{

	pu_small_info_t small_info;

	//ТАК ПОХОЖЕ НЕЛЬЗЯ memset(&small_info, 0, sizeof(small_info));


	small_info.addr = setting.uart_local_addr;
	small_info.mode = pu_mode;
	small_info.lines_count = setting.lines_count;
	small_info.lines = pu_get_fire_lines();


	uart4_uart7_snd_piv(0xFF, setting.uart_local_addr, PU_CMD_CHANGE_MODE, (uint8_t *)&small_info, sizeof(small_info));
}


void uart4_uart7_ring_check(void)
{
	if (!uart4_uart7_snd_ff_tick) return;
	if (HAL_DiffTick(uart4_uart7_snd_ff_tick) < 1000) return;

	if ((rcv_ring_tick[4] < uart4_uart7_snd_ff_tick) && (rcv_ring_tick[7] < uart4_uart7_snd_ff_tick) ) {
		cm7core_fault_set(FAULT_PU_RING_CMD);
	} else {
		cm7core_fault_unset(FAULT_PU_RING_CMD);
	}
}


// ------------------------------------------------------------------------------
// -------------------------------- UART8 AND UART5 -----------------------------
// ------------------------------------------------------------------------------

//static uint16_t uart8_uart5_snd_packet_id = 0;
//static uint16_t uart8_uart5_rcv_packet_id = 0;



//время приема пакета по кольцу 0 - UART8, 1 - UART5
//static uint32_t uart8_uart5_rcv_ring_tick[2] =	{0};

//static uint16_t last_rx_ids[9] = {0};

//Звук от uart8 и uart5 Слева и справа
void uart8_uart5_rcv(uint8_t huart_number, uint8_t * packet, uint16_t packet_len)
{

	uint16_t rx_id;

	//ulog_fmt(CONM"r8_5(%d %d) ", huart_number,  packet_len);
	//ulogmem(packet, packet_len, FALSE);
	//ulog(CRLF);

	if (!is_correct_crc16(packet, packet_len)) return;


	//if (rx_id == sound_rx_id) return; //дублирующий пакет
	//sound_rx_id = rx_id;

	if (packet[1] == setting.uart_local_addr) { //свой пакет по кругу
		rcv_ring_tick[huart_number] = HAL_GetTick();
		return; //это мой пакет по кольцу
	}


	if ( (packet[0] != 0xFF) &&  (packet[0] != setting.uart_local_addr)) { //не мне
		uarts_transmit_it_by_num((huart_number == 5 ? 8 : 5), packet, packet_len); //не свой пакет передаем дальше через шлюз
		return;
	}

	//тут daddr или мой или широковещательный

	rx_id = (packet[2] << 8) | packet[3];

	//if ((rx_id == last_rx_ids[8]) || (rx_id == last_rx_ids[5])) {
		//last_rx_ids[huart_number] = rx_id;
		//return;
	//}

	//last_rx_ids[huart_number] = rx_id;

	//fulog_fmt("r_%d(%d)"CRLF, huart_number, rx_id);

	//fulog_fmt("r_%d(%d %d)"CRLF, huart_number, rx_id, sound_rx_id);

	if (rx_id) {
		if (rx_id <= sound_rx_id) { //дублирующий пакет
			if (sound_rx_id - rx_id < 100) return;
		}
	} else {
		if (!sound_rx_id) return;
	}
	sound_rx_id = rx_id;

	//fulog_fmt("rok_%d(%d %d)"CRLF, huart_number, rx_id, sound_rx_id);

	recv_opus_cnt++;


	switch (packet[4]) {
		case PD_CMD_OPUS48mono:
		case PD_CMD_OPUS16mono:
		case PD_CMD_OPUS48ster:
		case PD_CMD_OPUS16ster:
			if ((pu_mode == PU_MODE_VESHANIE_PD) || (pu_mode == PU_MODE_RUCHNOI_PUSK_PD) || (pu_cmd_change_mode_state == 1)) {
				pd_check_audio_param(packet[4]);

				if (pcm1753_GetFrequency() > 16000) {
					opus_buffer_put(&packet[5], packet_len - 7);
				} else {
					pu_put_opus_in_pcm1753_now(&packet[5], packet_len - 7);
				}
				//fulog_fmt(CONM"r:%lu"CRLF, HAL_GetTick());
			}
		break;
	}

	if (packet[0] == 0xFF) { //если широковешательный
		uarts_transmit_it_by_num((huart_number == 5 ? 8 : 5), packet, packet_len); //широковещательный пакет передаем дальше через шлюз
	}


}

//#define UART8_UART5_SND_COUNT	10
//static uint8_t uart8_uart5_snd_cnt = UART8_UART5_SND_COUNT;
//static BOOL uart8_uart5_snd_num = FALSE;


//Послать звук в uart8 или в uart5
//В проводах идет вместе: ВЛЕВО-(UART5 и UART4),   ПРАВО-(UART8 и UART7)
void uart8_uart5_snd(uint8_t * packet, uint16_t packet_len)
{

	uarts_transmit_it_by_num(5, packet, packet_len);
	uarts_transmit_it_by_num(8, packet, packet_len);

	if (packet[0] == 0xFF) uart8_uart5_snd_ff_tick = HAL_GetTickNoZero();

}


void uart8_uart5_snd_piv(uint8_t daddr, uint8_t saddr, uint8_t cmd, uint8_t * cmd_data, uint16_t cmd_data_len)
{

	uint8_t packet[5 + cmd_data_len + 2];
	uint16_t packet_len;

	sound_tx_id++;
	packet_len = create_piv_packet_cmd2(packet, daddr, saddr, cmd, cmd_data, cmd_data_len, sound_tx_id);

	uart8_uart5_snd(packet, packet_len);
}


void uart8_uart5_ring_check(void)
{
	if (!uart8_uart5_snd_ff_tick) return;
	if (HAL_DiffTick(uart8_uart5_snd_ff_tick) < 1000) return;

	if ((rcv_ring_tick[5] < uart8_uart5_snd_ff_tick) && (rcv_ring_tick[8] < uart8_uart5_snd_ff_tick) ) {
		cm7core_fault_set(FAULT_PU_RING_AUDIO);
	} else {
		cm7core_fault_unset(FAULT_PU_RING_AUDIO);
	}

}

/*
static uint32_t uart8_uart5_snd_ring_audio_check_tick = 0;

//вызывать раз в секунду
void uart8_uart5_snd_ring_check(void)
{

	if ((pu_mode == PU_MODE_VESHANIE_MCHS) || (pu_mode == PU_MODE_UPRAVLENIE_DRUGIMI_PU)) return;

	if (HAL_DiffTick(uart8_uart5_active_tick) < 1000) return; //видим активность на AUDIO

	if (HAL_DiffTick(uart8_uart5_snd_ring_audio_check_tick) > 1000) { //с момента отправки прошла секунда

		if ( (rcv_ring_tick[5] > 80000) || (rcv_ring_tick[8] > 80000) ){ //UART5 UART8 разрыв кольца
			cm7core_fault_set(FAULT_PU_RING_AUDIO);
		}
		else {
			cm7core_fault_unset(FAULT_PU_RING_AUDIO);
		}
	}


	if (HAL_DiffTick(uart8_uart5_snd_ring_audio_check_tick) < 60000) return;

	uart8_uart5_snd_piv(0xFF, setting.uart_local_addr, PU_CMD_RING_AUDIO_CHECK, NULL, 0);
	uart8_uart5_snd_ring_audio_check_tick = HAL_GetTick();
}
*/




//uint8_t get_opus_cmd_from_pcm1753(void)
//{
//	return pd_get_opus_cmd_from_frequency_stereo(pcm1753_GetFrequency(), pcm1753_GetFrequency());
//}


//послать данные опуса через uart5 uart8
void uart8_uart5_send_opus_data_cmd(uint8_t * opus_data, int opus_data_len, uint8_t daddr, uint8_t cmd)
{

	uart8_uart5_snd_piv(daddr, setting.uart_local_addr, cmd, opus_data, opus_data_len);

	/*
	uint8_t packet[MAX_PACKET_SIZE];
	uint16_t packet_len;

	packet[0] = daddr;
	packet[1] = setting.uart_local_addr;
	packet_len = 1 + opus_data_len;

	sound_tx_id++;
	//packet[2] = packet_len >> 8;
	//packet[3] = packet_len & 0xFF;

	packet[2] = sound_tx_id >> 8;
	packet[3] = sound_tx_id & 0xFF;

	packet[4] = cmd;
	memcpy(&packet[5], opus_data, opus_data_len);
	packet_len = 7 + opus_data_len;
	fill_crc16(packet, packet_len);
	uart8_uart5_snd(packet, packet_len);
	*/
}

void uart8_uart5_send_opus_data(uint8_t * opus_data, int opus_data_len, uint8_t daddr, uint32_t frequency, BOOL stereo)
{
	uart8_uart5_send_opus_data_cmd(opus_data, opus_data_len, daddr, pd_get_opus_cmd_from_frequency_stereo(frequency, stereo));
}




// ------------------------------------------------------------------------------
// -------------------------------- UART1 AND UART3 -----------------------------
// ------------------------------------------------------------------------------


//Звук От ПД1 и ПД2
void uart1_uart3_rcv(uint8_t huart_number, uint8_t * packet, uint16_t  packet_len)
{
	if (!is_correct_crc16(packet, packet_len)) return;

	if ((packet[0] == setting.uart_local_addr) || (packet[0] == 0xFF)) {
		if ((pu_mode == PU_MODE_VESHANIE_PD) || (pu_mode == PU_MODE_RUCHNOI_PUSK_PD)) {


			pd_check_audio_param(packet[4]);
			if (pcm1753_GetFrequency() > 16000) {
				opus_buffer_put(&packet[5], packet_len - 7);
			} else {
				pu_put_opus_in_pcm1753_now(&packet[5], packet_len - 7);
			}


		}
	}


	if (packet[0] != setting.uart_local_addr) { //не ине или широковещательный
		//послвть звук в сеть
		packet[1] = setting.uart_local_addr;
		sound_tx_id++;
		packet[2] = sound_tx_id >> 8;
		packet[3] = sound_tx_id & 0xFF;
		fill_crc16(packet, packet_len);
		uart8_uart5_snd(packet, packet_len);
	}

}

// ------------------------------------------------------------------------------
// -------------------------------- OTHER -----------------------------
// ------------------------------------------------------------------------------


void uarts_rcv_data(uint8_t huart_number, uint8_t * packet, uint16_t packet_len)
{
	//fulog_fmt("r %d %d"CRLF, huart_number, data_len);

	switch (huart_number) {
		case 1: uart1_uart3_rcv(huart_number, packet, packet_len); break;
		case 3: uart1_uart3_rcv(huart_number, packet, packet_len); break;
		case 4: uart4_uart7_rcv(huart_number, packet, packet_len); break;
		case 5: uart8_uart5_rcv(huart_number, packet, packet_len); break;
		case 7: uart4_uart7_rcv(huart_number, packet, packet_len); break;
		case 8: uart8_uart5_rcv(huart_number, packet, packet_len); break;
	}
}



void uarts_task_data_rx(void)
{
	uint8_t buf[MAX_SOUND_PACKET_SIZE];
	size_t len;

	if (!dynamic_buf_pop(&uarts_rx_buf, buf, sizeof(buf), &len)) return;

	uarts_rcv_data(buf[len - 1], buf, len - 1);
}




void uarts_task(void)
{
	if (DYNAMIC_BUF_IS_EMPTY(uarts_rx_buf)) return;
	if (DYNAMIC_BUF_GET_BUSY(uarts_rx_buf)) return;
	uarts_task_data_rx();
}





void uarts_ring_init(void)
{
	int i;

	for (i = 0; i < 9; i++) rcv_ring_tick[i] = HAL_GetTick();
}




void uarts_init_huart(UART_HandleTypeDef * huart)
{
	const huart_param_t * huart_param;

	uarts_abort_huart(huart);
	huart_param = get_huart_param_by_handle(huart);
	if (huart_param) {
		uarts_ReceiveToIdle_IT_by_param(huart_param);
	}
}


void uarts_init(void)
{


	PIN_CLR(UART_P1_SOUND_EN); //с ПД звук только принимаем
	PIN_CLR(UART_P2_SOUND_EN); //с ПД звук только принимаем



	uarts_abort_huart(&huart1);
	uarts_abort_huart(&huart3);
	uarts_abort_huart(&huart4);
	uarts_abort_huart(&huart5);
	uarts_abort_huart(&huart7);
	uarts_abort_huart(&huart8);



	UART_flush(&huart1);
	UART_flush(&huart3);
	UART_flush(&huart4);
	UART_flush(&huart5);
	UART_flush(&huart7);
	UART_flush(&huart8);


	uarts_ReceiveToIdle_IT_by_param(&huart_params[0]); //uart1
	uarts_ReceiveToIdle_IT_by_param(&huart_params[1]); //uart3
	uarts_ReceiveToIdle_IT_by_param(&huart_params[2]); //uart4
	uarts_ReceiveToIdle_IT_by_param(&huart_params[3]); //uart5
	uarts_ReceiveToIdle_IT_by_param(&huart_params[4]); //uart7
	uarts_ReceiveToIdle_IT_by_param(&huart_params[5]); //uart8

	uarts_ring_init();
}


/*

//static BOOL uarts_ring_uart_left_right = FALSE;

void uarts_send_ring(void)
{

	uarts_ring_uart_left_right = !uarts_ring_uart_left_right;

	uarts_transmit_piv(uarts_ring_uart_left_right ? 4 : 7, setting.uart_local_addr, setting.uart_local_addr, PD_CMD_PING, NULL, 0);
	uarts_transmit_piv(uarts_ring_uart_left_right ? 8 : 5, setting.uart_local_addr, setting.uart_local_addr, PD_CMD_PING, NULL, 0);


	if (HAL_DiffTick(uart8_uart5_rcv_ring_tick) > 20000) {
		fault_set(FAULT_PU_RING_AUDIO);
	} else {
		fault_unset(FAULT_PU_RING_AUDIO);
	}

	if (HAL_DiffTick(uart4_uart7_rcv_ring_tick) > 20000) {
		fault_set(FAULT_PU_RING_CMD);
	} else {
		fault_unset(FAULT_PU_RING_CMD);
	}

}

//Комманды от uart4 и uart7 слева и справа
void uart4_uart7_snd(uint8_t * packet, uint16_t packet_len)
{
	int index;

	index = packet[0] / 10; //daddr/10

	if (uart4_uart7_rcv_huart_number[index] && (HAL_DiffTick(uart4_uart7_rcv_huart_number_tick[index]) < 1000) ) {
		uarts_transmit_it_by_num(uart4_uart7_rcv_huart_number[index], packet, packet_len);
	} else { //линия разорвана
		uarts_transmit_it_by_num(4, packet, packet_len);
		uarts_transmit_it_by_num(7, packet, packet_len);
	}
}


//static uint32_t last_pd_snd_tick = 0;
//static uint32_t opus_rcv_cnt = 0;
//uint32_t uarts_transmit_it_tick = 0;

dynamic_buf_t uarts_tx_buf = DYNAMIC_BUF_STATIC_INIT(60 * 1024) ;

void uarts_trans(uint8_t * packet, uint16_t packet_len, uint8_t huart_number, uint8_t mux)
{
	uint8_t buf[1000];

	memcpy(buf, packet, packet_len);
	buf[packet_len] = huart_number;
	buf[packet_len + 1] = mux;
	dynamic_buf_push(&uarts_tx_buf, packet, packet_len + 2);
}

void uarts_trans_task(void)
{
	uint8_t mux;
	const huart_param_t * huart_param;
	size_t data_len;
	uint8_t buf[1000];

	if (DYNAMIC_BUF_IS_EMPTY(uarts_tx_buf)) return;

	huart_param = get_huart_param_by_number(uarts_tx_buf.first->data[uarts_tx_buf.first->data_len - 2]);
	if (!huart_param) {
		dynamic_buf_pop(&uarts_tx_buf, buf, sizeof(buf), &data_len);
		return;
	}

	if (HAL_UART_GetState(huart_param->huart) == HAL_UART_STATE_BUSY_TX) return;

	mux = uarts_tx_buf.first->data[uarts_tx_buf.first->data_len - 1];

	dynamic_buf_pop(&uarts_tx_buf,  huart_param->tx_buf, huart_param->tx_buf_size, &data_len);
	if (data_len < 2) return;

	if (huart_param->number == 6) {
		if (huart6.RxState != HAL_UART_STATE_READY) HAL_UART_AbortReceive_IT(&huart6);
		uart6_set_mux(mux);
		HAL_UARTEx_ReceiveToIdle_IT(&huart6, huart_param->rx_buf, huart_param->rx_buf_size);
	}
	else if (huart_param->number == 2) {
		if (huart2.RxState != HAL_UART_STATE_READY) HAL_UART_AbortReceive_IT(&huart2);
		uart2_set_mux(mux);
		HAL_UARTEx_ReceiveToIdle_IT(&huart2, huart_param->rx_buf, huart_param->rx_buf_size);
	}

	HAL_UART_Transmit_IT(huart_param->huart, huart_param->tx_buf, data_len - 2);



}
*/
