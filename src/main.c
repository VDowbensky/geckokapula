/***************************************************************************//**
 * @file main.c
 * @brief Simple RAIL application which includes hal
 * @copyright Copyright 2017 Silicon Laboratories, Inc. http://www.silabs.com
 ******************************************************************************/
#include "rail.h"
#include "hal_common.h"
#include "rail_config.h"
#include "pa.h"

#include "em_chip.h"
#include "em_usart.h"
#include "em_gpio.h"
#include "em_timer.h"
#include "em_wdog.h"
#include "em_adc.h"

#include "InitDevice.h"

#include <stdint.h>
#include <stdio.h>
#include "arm_math.h"
#include "arm_const_structs.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "task.h"

#include "display.h"
#include "ui.h"
#include "rig.h"

rig_parameters_t p = {0,1,1, 2395000000, 0 };


uint8_t nollaa[300] = {255,255,0};

void startrx() {
	RAIL_RfIdleExt(RAIL_IDLE, true);
	RAIL_ResetFifo(false, true);
	RAIL_SetRxFifoThreshold(100); //FIFO size is 512B
	RAIL_EnableRxFifoThreshold();
	RAIL_RxStart(p.channel);
}

void starttx() {
	RAIL_RfIdleExt(RAIL_IDLE_ABORT, true);
	RAIL_ResetFifo(true, false);
	RAIL_SetTxFifoThreshold(100);
	RAIL_WriteTxFifo(nollaa, 300);
	RAIL_TxStart(p.channel, NULL, NULL);
}

void transmit_something() {
	RAIL_TxData_t txstuff = { nollaa, 200 };
	RAIL_RfIdleExt(RAIL_IDLE_ABORT, true);
	RAIL_ResetFifo(true, false);
	RAIL_TxDataLoad(&txstuff);
	RAIL_TxStart(p.channel, NULL, NULL);
}

RAIL_ChannelConfigEntry_t channelconfigs[] = {{ 0, 20, 1000, 2395000000 }};
const RAIL_ChannelConfig_t channelConfig = { channelconfigs, 1 };
void config_channel() {

	RAIL_RfIdleExt(RAIL_IDLE_ABORT, true);

	channelconfigs[0].baseFrequency = p.frequency;
	RAIL_ChannelConfig(&channelConfig);

	/*RAIL_DataConfig_t dataConfig = { TX_PACKET_DATA, RX_IQDATA_FILTLSB, FIFO_MODE, FIFO_MODE };
	RAIL_DataConfig(&dataConfig);*/

}

void initRadio() {
  RAIL_Init_t railInitParams = {
    256,
    RADIO_CONFIG_XTAL_FREQUENCY,
    RAIL_CAL_ALL,
  };
  RADIO_PA_Init(&(RADIO_PAInit_t){
	    PA_SEL_2P4_HP,    /* Power Amplifier mode */
	    PA_VOLTMODE_VBAT, /* Power Amplifier vPA Voltage mode */
	    190,              /* Desired output power in dBm * 10 */
	    0,                /* Output power offset in dBm * 10 */
	    10,               /* Desired ramp time in us */
  });

  //halInit();
  RAIL_RfInit(&railInitParams);
  USART_Tx(USART0, '1');
  RAIL_RfIdleExt(RAIL_IDLE, true);
  USART_Tx(USART0, '2');

  RAIL_CalInit_t calInit = {
    RAIL_CAL_ALL,
    irCalConfig,
  };
  RAIL_CalInit(&calInit);
  USART_Tx(USART0, '3');

  RAIL_PacketLengthConfigFrameType(frameTypeConfigList[0]);
  USART_Tx(USART0, '4');
  if (RAIL_RadioConfig((void*)configList[0])) {
    //while (1) ;
	  USART_Tx(USART0, 'f');
  }
  USART_Tx(USART0, '5');

  RAIL_ChannelConfig(channelConfigs[0]);
  USART_Tx(USART0, '6');

  RAIL_DataConfig_t dataConfig = { TX_PACKET_DATA, RX_IQDATA_FILTLSB, FIFO_MODE /*PACKET_MODE*/, FIFO_MODE };
  RAIL_DataConfig(&dataConfig);
  USART_Tx(USART0, '7');
}

#define FFTLEN 128
const arm_cfft_instance_f32 *fftS = &arm_cfft_sR_f32_len128;
float fftbuf[2*FFTLEN];
volatile int fftbufp = 0;

#define RXBUFL 2
typedef int16_t iqsample_t[2];
iqsample_t rxbuf[RXBUFL];
void RAILCb_RxFifoAlmostFull(uint16_t bytesAvailable) {
	unsigned nread, i;
	static int psi=0, psq=0;
	static int agc_level=0;
	GPIO_PortOutToggle(gpioPortF, 4);
	nread = RAIL_ReadRxFifo((uint8_t*)rxbuf, 4*RXBUFL);
	nread /= 4;
	int ssi=0, ssq=0, audioout = 0;
	static unsigned smeter_count = 0;
	static uint64_t smeter_acc = 0;
	static int audio_lpf = 0;
 	for(i=0; i<nread; i++) {
		int si=rxbuf[i][0], sq=rxbuf[i][1];
		int fi, fq;
		switch(p.mode) {
		case MODE_FM: {
			int fm;
			// multiply by conjugate
			fi = si * psi + sq * psq;
			fq = sq * psi - si * psq;
			/* Scale maximum absolute value to 0x7FFF.
			 * This can be done because FM demod doesn't care about amplitude.
			 */
			while(fi > 0x7FFF || fi < -0x7FFF || fq > 0x7FFF || fq < -0x7FFF) {
				fi /= 0x100; fq /= 0x100;
			}
			// very crude approximation...
			fm = 0x8000 * fq / ((fi>=0?fi:-fi) + (fq>=0?fq:-fq));
			audio_lpf += (fm*128 - audio_lpf)/16;
			audioout = audio_lpf/128;
			break; }
		case MODE_DSB: {
			int agc_1, agc_diff;
			audio_lpf += (si*128 - audio_lpf)/16;
			fi = audio_lpf/128; // TODO: SSB filter

			// AGC
			agc_1 = (fi>=0?fi:-fi) * 0x100; // rectify
			agc_diff = agc_1 - agc_level;
			if(agc_diff > 0)
				agc_level += agc_diff/64;
			else
				agc_level += agc_diff/256;

			audioout += 0x1000 * fi / (agc_level/0x100);

			break; }
		}

		psi = si; psq = sq;
		ssi += si; ssq += sq;

		smeter_acc += si*si + sq*sq;
	}

	int fp = fftbufp;
	if(fp < 2*FFTLEN) {
		const float scaling = 1.0f / (RXBUFL*0x8000);
		fftbuf[fp]   = scaling*ssi;
		fftbuf[fp+1] = scaling*ssq;
		fftbufp = fp+2;
	}

	audioout = (audioout / 0x100) + 100;
	if(audioout < 0) audioout = 0;
	if(audioout > 200) audioout = 200;
	TIMER_CompareBufSet(TIMER0, 0, audioout);
	//USART_Tx(USART0, 'r');

	smeter_count += nread;
	if(smeter_count >= 0x4000) {
		p.smeter = smeter_acc / 0x4000;
		smeter_acc = 0;
		smeter_count = 0;
	}

}

void RAILCb_TxFifoAlmostEmpty(uint16_t bytes) {
	GPIO_PortOutToggle(gpioPortF, 4);
	RAIL_WriteTxFifo(nollaa, 100);
	USART_Tx(USART0, 'e');
}

int testnumber=73;

void task1() {
	for(;;) {
		unsigned keyed = !GPIO_PinInGet(PTT_PORT, PTT_PIN);
		WDOG_Feed();
		if(p.channel_changed) {
			config_channel();
		}
		WDOG_Feed();
		if(keyed && (RAIL_RfStateGet() != RAIL_RF_STATE_TX || p.channel_changed)) {
			p.channel_changed = 0;
			RAIL_RfIdleExt(RAIL_IDLE_ABORT, false);
			RAIL_TxToneStart(p.channel);
		}
		if((!keyed) && (RAIL_RfStateGet() != RAIL_RF_STATE_RX || p.channel_changed)) {
			p.channel_changed = 0;
			RAIL_TxToneStop();
			startrx();
		}

		WDOG_Feed();
		if(fftbufp >= 2*FFTLEN) {
			arm_cfft_f32(fftS, fftbuf, 0, 1);
			ui_fft_line(fftbuf);
			fftbufp = 0;
		}

		WDOG_Feed();
		//testnumber = ADC_DataSingleGet(ADC0);
		ui_loop();
		GPIO_PortOutSet(gpioPortF, 5);
		GPIO_PortOutClear(gpioPortF, 5);
	}
}

void task2() {
	for(;;) {
		testnumber++;
		taskYIELD();
	}
}

TaskHandle_t task1h, task2h;

int main(void) {
	enter_DefaultMode_from_RESET();
	USART_Tx(USART0, 'a');
	WDOG_Feed();
	initRadio();
 	USART_Tx(USART0, 'b');

 	TIMER_TopSet(TIMER0, 200);
 	TIMER_CompareBufSet(TIMER0, 0, 33);

 	ADC_Start(ADC0, adcStartSingle);

 	xTaskCreate(task1, "task1", 512, NULL, 1, &task1h);
 	xTaskCreate(task2, "task2", 128,  NULL, 1, &task2h);
 	vTaskStartScheduler();
	return 0;
}
