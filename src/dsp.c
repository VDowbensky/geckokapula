/*
 * dsp.c
 *
 *  Created on: Mar 26, 2018
 *      Author: Tatu
 */

#include <stdlib.h>

// CMSIS
#include "arm_math.h"
#include "arm_const_structs.h"

// emlib
#include "rail.h"
#include "em_timer.h"
#include "em_adc.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "task.h"

// rig
#include "rig.h"
#include "ui.h"
#include "ui_parameters.h"
#include "dsp_parameters.h"

#define RXBUFL 2
typedef int16_t iqsample_t[2];

const arm_cfft_instance_f32 *fftS = &arm_cfft_sR_f32_len256;
#define SIGNALBUFLEN 512
int16_t signalbuf[2*SIGNALBUFLEN];
volatile int signalbufp = 0;

extern rig_parameters_t p;

/* Interrupt for DSP operations that take a short time and need low latency */
void dsp_rx(iqsample_t *input, uint8_t *output) {
	int i;
	static int asdf=0;
	for(i=0; i<PWMBLOCKLEN; i++) {
		output[i] = (asdf++)/4;
	}
#if 0 // TODO
	unsigned nread, i;
	static int psi=0, psq=0;
	static int agc_level=0;
	nread = RAIL_ReadRxFifo((uint8_t*)rxbuf, 4*RXBUFL);
	nread /= 4;
	int ssi=0, ssq=0, audioout = 0;
	static unsigned smeter_count = 0;
	static uint64_t smeter_acc = 0;
	static int audio_lpf = 0, audio_hpf = 0;
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
		case MODE_AM:
		case MODE_DSB: {
			int agc_1, agc_diff;
			int raw_audio;
			if(p.mode == MODE_AM)
				raw_audio = abs(si) + abs(sq);
			else
				raw_audio = si*2;
			audio_lpf += (raw_audio*64 - audio_lpf)/16;
			audio_hpf += (audio_lpf - audio_hpf) / 512; // DC block
			fi = (audio_lpf-audio_hpf)/128; // TODO: SSB filter

			// AGC
			agc_1 = abs(fi) * 0x100; // rectify
			agc_diff = agc_1 - agc_level;
			if(agc_diff > 0)
				agc_level += agc_diff/256;
			else
				agc_level += agc_diff/2048;

			audioout += 0x1000 * fi / (agc_level/0x100);

			break; }
		default: {
			audioout = 0;
			break;
		}
		}

		psi = si; psq = sq;
		ssi += si; ssq += sq;

		smeter_acc += si*si + sq*sq;
	}

	int fp = signalbufp;
	signalbuf[fp]   = ssi;
	signalbuf[fp+1] = ssq;
	fp+=2;
	if(fp >= SIGNALBUFLEN) fp = 0;
	signalbufp = fp;

	audioout = (p.volume2 * audioout / 0x1000) + 100;
	if(audioout < 0) audioout = 0;
	if(audioout > 200) audioout = 200;
	TIMER_CompareBufSet(TIMER0, 0, audioout);
	//USART_Tx(USART0, 'r');

	smeter_count += nread;
	if(smeter_count >= 0x4000) {
		rs.smeter = smeter_acc / 0x4000;
		smeter_acc = 0;
		smeter_count = 0;
	}
#endif
}

#if 0
extern int testnumber;
void ADC0_IRQHandler() {
	int audioout;
	static int hpf, lpf, agc_level=0;
	//testnumber++;

	int audioin = ADC0->SINGLEDATA;
	ADC_IntClear(ADC0, ADC_IF_SINGLE);

	if(!p.keyed) {
		synth_set_channel(p.channel);
		return;
	}

	// DC block:
	hpf += (audioin - hpf) / 32;
	audioin -= hpf;
	// lowpass
	lpf += (audioin - lpf) / 2;
	audioin = lpf;

	// Just copied AGC code from RX for now
	int agc_1, agc_diff;
	agc_1 = abs(audioin) * 0x100; // rectify
	agc_diff = agc_1 - agc_level;
	if(agc_diff > 0)
		agc_level += agc_diff/0x100;
	else
		agc_level += agc_diff/0x1000;

	audioout = 32 + 20 * audioin / (agc_level/0x100);
	if(audioout <= 0) audioout = 0;
	if(audioout >= 63) audioout = 63;

	synth_set_channel(audioout);
}
#endif

void dsp_tx(uint8_t *input, uint8_t *output) {
	int i;
	static int asdf=0;
	for(i=0; i<TXBLOCKLEN; i++) {
		output[i] = asdf & 63;
		asdf++;
	}
}

static void calculate_waterfall_line() {
	extern uint8_t displaybuf2[3*(FFT_BIN2-FFT_BIN1)];
	extern char fftline_ready; // TODO: semaphore or something?
	unsigned i;
	float mag_avg = 0;

	/* These are static because so such big arrays would not be allocated from stack.
	 * Not sure if this is a good idea.
	 * If averaging were not used, mag could actually reuse fftdata
	 * with some changes to indexing.
	 */
	static float fftdata[2*FFTLEN], mag[FFTLEN];
	static uint8_t averages = 0;

	//if(fftline_ready) return;

	int sbp = signalbufp;
	const float scaling = 1.0f / (RXBUFL*0x8000);
	for(i=0; i<2*FFTLEN; i+=2) {
		sbp &= SIGNALBUFLEN-1;
		fftdata[i]   = scaling*signalbuf[sbp];
		fftdata[i+1] = scaling*signalbuf[sbp+1];
		sbp += 2;
	}

	// to see how many samples are between FFTs
	/*static int debug_last_sbp = 0;
	testnumber = (sbp - debug_last_sbp) & (SIGNALBUFLEN-1);
	debug_last_sbp = sbp;*/

	arm_cfft_f32(fftS, fftdata, 0, 1);

	if(averages == 0)
		for(i=0;i<FFTLEN;i++) mag[i] = 0;
	for(i=0;i<FFTLEN;i++) {
		float fft_i = fftdata[2*i], fft_q = fftdata[2*i+1];
		mag_avg +=
		mag[i ^ (FFTLEN/2)] += fft_i*fft_i + fft_q*fft_q;
	}
	averages++;
	if(averages < p.waterfall_averages)
		return;
	averages = 0;
	mag_avg = (130.0f*FFTLEN) / mag_avg;

	uint8_t *bufp = displaybuf2;
	for(i=FFT_BIN1;i<FFT_BIN2;i++) {
		unsigned v = mag[i] * mag_avg;
		if(v < 0x100) {  // black to blue
			bufp[0] = v / 2;
			bufp[1] = 0;
			bufp[2] = v;
		} else if(v < 0x200) { // blue to yellow
			bufp[0] = v / 2;
			bufp[1] = v - 0x100;
			bufp[2] = 0x1FF - v;
		} else if(v < 0x300) { // yellow to white
			bufp[0] = 0xFF;
			bufp[1] = 0xFF;
			bufp[2] = v - 0x200;
		} else { // white
			bufp[0] = 0xFF;
			bufp[1] = 0xFF;
			bufp[2] = 0xFF;
		}
		bufp += 3;
	}

	fftline_ready = 1;
}

/* A task for DSP operations that can take a longer time */
void dsp_task() {
	start_tx_dsp();
	start_rx_dsp();
	for(;;) {
		// TODO: semaphore?

		//calculate_waterfall_line();

		// delay can be commented out to see how often FFTs can be calculated
		vTaskDelay(1);
	}
}

