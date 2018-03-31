/*
 * display.c
 *
 *  Created on: Sep 24, 2017
 *      Author: Tatu
 */

/*
 * P1 PC6  DATA
 * P3 PC7  CS
 * P5 PC8  CLK
 * P7 PC9  DC
 */

#include <stdint.h>
#include "em_usart.h"
#include "em_gpio.h"
#include "em_ldma.h"
#include "em_timer.h"
#include "InitDevice.h"
#include "rail.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

// rig
#include "ui_parameters.h"
#include "hw.h"

static char display_initialized = 0, display_doing_dma = 0;

#define GPIO_PortOutSet(g, p) GPIO->P[g].DOUT |= (1<<(p));
#define GPIO_PortOutClear(g, p) GPIO->P[g].DOUT &= ~(1<<(p));

void display_start() {
	while (!(USART1->STATUS & USART_STATUS_TXC));
	GPIO_PortOutSet(TFT_CS_PORT, TFT_CS_PIN);
	GPIO_PortOutSet(TFT_DC_PORT, TFT_DC_PIN);
	GPIO_PortOutClear(TFT_CS_PORT, TFT_CS_PIN);
}

void display_end() {
	while (!(USART1->STATUS & USART_STATUS_TXC));
	GPIO_PortOutSet(TFT_CS_PORT, TFT_CS_PIN);
}

static void writedata(uint8_t d) {
	//display_start();
	GPIO_PortOutSet(TFT_DC_PORT, TFT_DC_PIN);
	USART_SpiTransfer(USART1, d);
	//display_end();
}

static void writecommand(uint8_t d) {
	GPIO_PortOutSet(TFT_CS_PORT, TFT_CS_PIN);
	GPIO_PortOutClear(TFT_DC_PORT, TFT_DC_PIN);
	GPIO_PortOutClear(TFT_CS_PORT, TFT_CS_PIN);
	USART_SpiTransfer(USART1, d);
	//GPIO_PortOutSet(TFT_CS_PORT, TFT_CS_PIN);
}

void display_pixel(uint8_t r, uint8_t g, uint8_t b) {
	USART_Tx(USART1, r);
	USART_Tx(USART1, g);
	USART_Tx(USART1, b);
}

extern int testnumber;
#ifdef BLOCK_UNTIL_DMA_READY
static TaskHandle_t myhandle;

void LDMA_IRQHandler() {
	//testnumber++;
	uint32_t pending = LDMA_IntGetEnabled();
	if(pending & (1<<DMA_CH_DISPLAY)) {
		LDMA->IFC = 1<<DMA_CH_DISPLAY;

		BaseType_t xHigherPriorityTaskWoken;
		/* xHigherPriorityTaskWoken must be initialised to pdFALSE.  If calling
		vTaskNotifyGiveFromISR() unblocks the handling task, and the priority of
		the handling task is higher than the priority of the currently running task,
		then xHigherPriorityTaskWoken will automatically get set to pdTRUE. */
		xHigherPriorityTaskWoken = pdFALSE;

		/* Unblock the handling task so the task can perform any processing necessitated
		by the interrupt.  xHandlingTask is the task's handle, which was obtained
		when the task was created. */
		vTaskNotifyGiveFromISR(myhandle, &xHigherPriorityTaskWoken);
		//xSemaphoreGiveFromISR(dma_semaphore, &xHigherPriorityTaskWoken);

		/* Force a context switch if xHigherPriorityTaskWoken is now set to pdTRUE.
		The macro used to do this is dependent on the port and may be called
		portEND_SWITCHING_ISR. */
		portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
	}
}
#endif

void display_transfer(uint8_t *dmadata, int dmalen) {
	LDMA_TransferCfg_t tr =
			LDMA_TRANSFER_CFG_PERIPHERAL(ldmaPeripheralSignal_USART1_TXBL);
	LDMA_Descriptor_t desc =
			LDMA_DESCRIPTOR_SINGLE_M2P_BYTE(dmadata, &USART1->TXDATA, dmalen);
	//LDMA_IntEnable(1<<DMA_CH_DISPLAY);
	display_doing_dma = 1;
#ifdef BLOCK_UNTIL_DMA_READY
	myhandle = xTaskGetCurrentTaskHandle();
#endif
	LDMA_StartTransfer(DMA_CH_DISPLAY, &tr, &desc);
#ifdef BLOCK_UNTIL_DMA_READY
	ulTaskNotifyTake(pdFALSE, 100);
#endif
}

void display_area(int x1,int y1,int x2,int y2) {
	writecommand(0x2A); // column address set
	writedata(0);
	writedata(x1);
	writedata(0);
	writedata(x2);
	writecommand(0x2B); // row address set
	writedata(0);
	writedata(y1);
	writedata(0);
	writedata(y2);
	writecommand(0x2C); // memory write
}

int display_ready() {
	if(display_doing_dma) {
		if(LDMA_TransferDone(DMA_CH_DISPLAY))
			display_doing_dma = 0;
		else return 0;
	}
	return display_initialized;
}

// minimum delay between display init commands (us)
#define DISPLAY_INIT_DELAY_US 20000
#define CMD(x) ((x)|0x100)
void display_init_loop() {
	static int di_i = 0;
	static uint32_t next_time = 0;
	const uint16_t display_init_commands[] = {
			CMD(0x01), CMD(0x01), CMD(0x11), CMD(0x11), CMD(0x29), CMD(0x29),
			CMD(0x33), // vertical scrolling definition
			  0, FFT_ROW1, 0, FFT_ROW2+1-FFT_ROW1, 0, 0
	};

	uint32_t time = RAIL_GetTime();
	if(di_i != 0 && next_time - time >= 0x80000000UL) return;
	next_time = time + DISPLAY_INIT_DELAY_US;

	if(di_i <  sizeof(display_init_commands)/sizeof(display_init_commands[0])) {
		unsigned c = display_init_commands[di_i];
		if(c & 0x100) writecommand(c & 0xFF);
		else writedata(c);
		di_i++;
	} else {
		display_initialized = 1;
	}
}

void display_scroll(unsigned y) {
	writecommand(0x37);
	writedata(y>>8);
	writedata(y);
}

void display_backlight(int b) {
	if(b < 0) b = 0;
	if(b > TIMER0_PERIOD) b = TIMER0_PERIOD;
 	TIMER_CompareBufSet(TIMER0, 1, b);
}
