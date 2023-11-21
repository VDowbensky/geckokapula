/* SPDX-License-Identifier: MIT */

// RAIL
#include "rail.h"
#include "rail_config.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "task.h"

// rig
#include "rig.h"
#include "railtask.h"
#include "dsp_driver.h"
#include "config.h"

#include <stdlib.h>
#include <stdio.h>

#define CHANNELSPACING 147 // 38.4 MHz / 2^18
#define MIDDLECHANNEL 32

RAIL_Handle_t rail;
xSemaphoreHandle railtask_sem;

struct railtask_state {
	// Latest configured frequency
	uint32_t frequency;
	// 1 if frequency is within range that can be tuned
	char config_ok;
};
struct railtask_state railtask;

extern RAIL_ChannelConfigEntryAttr_t generated_entryAttr;

RAIL_ChannelConfigEntry_t channelconfig_entry[] = {
	{
		.phyConfigDeltaAdd = NULL,
		.baseFrequency = RIG_DEFAULT_FREQUENCY,
		.channelSpacing = CHANNELSPACING,
		.physicalChannelOffset = 0,
		.channelNumberStart = 0,
		.channelNumberEnd = 63,
		.maxPower = RAIL_TX_POWER_MAX,
		.attr = &generated_entryAttr
	}
};

extern uint32_t generated_phyInfo[];
extern uint32_t generated[];
const RAIL_ChannelConfig_t channelConfig = {
	generated,
	NULL,
	channelconfig_entry,
	1,
	0
};


/* Find suitable VCO frequency dividers for a given frequency.
 * Return 0 if no possible combination was found. */
static inline uint32_t find_divider(uint32_t f, uint32_t *ratio)
{
	// Find a divider values that gets VCO frequency closest to
	// the approximate middle of its tuning range, vco_mid.
	const long long vco_mid = 2600000000LL;
	// Smallest distance from vco_mid found.
	// Initial value determines the maximum allowed distance.
	// If no divider values getting closer than that are found,
	// d1m, d2m and d3m will stay 0 and the function will return 0.
	long long dmin = 600000001LL;
	// Divider values from the combination that achieves dmin.
	uint32_t d1m = 0, d2m = 0, d3m = 0;
	uint32_t d1, d2, d3;
#ifdef KAPULA_v2
	// Try all the possible combinations
	for (d1 = 1; d1 <= 5; d1++) {
#else
	// v1 seems to crash on some frequencies below 23 MHz.
	// It's mostly useless on lower frequencies anyway,
	// so just limit the tuning range by not allowing d1=5.
	for (d1 = 1; d1 <= 4; d1++) {
#endif
		for (d2 = 1; d2 <= 5; d2++) {
#ifdef KAPULA_v2
			// Try values 1, 2, 3, 4, 5, 7 for d3
			for (d3 = 1; d3 <= 6; d3++) {
				if (d3 == 6) d3 = 7;
#else
			// 7 isn't supported by the older chip.
			for (d3 = 1; d3 <= 5; d3++) {
#endif
				// VCO frequency with these divider values
				long long vco = (long long)f * d1 * d2 * d3;
				// Distance from middle of VCO tuning range
				long long d = llabs(vco - vco_mid);
				if (d < dmin) {
					dmin = d;
					d1m = d1;
					d2m = d2;
					d3m = d3;
				}
			}
		}
	}
	*ratio = d1m * d2m * d3m;
	if (d1m == 1) d1m = 0;
	if (d2m == 1) d2m = 0;
	return (d1m << 6) | (d2m << 3) | d3m;
}


void railtask_config_channel(uint32_t freq)
{
	unsigned r;
	RAIL_Idle(rail, RAIL_IDLE_ABORT, true);

	uint32_t basefreq = freq - MIDDLECHANNEL*CHANNELSPACING;

	uint32_t ratio;
	uint32_t divider = find_divider(basefreq, &ratio);
	if (!divider) {
		// This frequency isn't possible.
		railtask.config_ok = 0;
		return;
	}
	railtask.config_ok = 1;
	// Modify the frequency divider register in radio configuration
	generated[39] = divider;
	// and the IF register.
	// If you change the IF, remember to modify this as well.
	uint32_t iffreq = ratio << 11;
	generated[38] = 0x00100000UL | iffreq;

	// I don't exactly know what these values are doing
	// but looks like they should all be proportional
	// to the division ratio being used.
	generated_phyInfo[1] = 111848UL * ratio;
	// The lowest 16 (or more?) bits of this are also
	// proportional to the intermediate frequency.
	generated_phyInfo[10] = (ratio << 25) | iffreq;

	// Then the normal RAIL configuration
	channelconfig_entry[0].baseFrequency = basefreq;
	r = RAIL_ConfigChannels(rail, &channelConfig, NULL);
	printf("RAIL_ConfigChannels (2): %u\n", r);
	railtask.frequency = freq;

	RAIL_DataConfig_t dataConfig = { TX_PACKET_DATA, RX_IQDATA_FILTLSB, FIFO_MODE, FIFO_MODE };
	r = RAIL_ConfigData(rail, &dataConfig);
	printf("RAIL_ConfigData: %u\n", r);

	// 2.4 GHz needs different PA configuration
	RAIL_TxPowerConfig_t txPowerConfig = {
		.mode = divider == 1 ?
			RAIL_TX_POWER_MODE_2P4GIG_HP :
			RAIL_TX_POWER_MODE_SUBGIG,
		.voltage = 3300,
		.rampTime = 10,
	};
	r = RAIL_ConfigTxPower(rail, &txPowerConfig);
	printf("RAIL_ConfigTxPower: %u\n", r);
	r = RAIL_SetTxPower(rail, RAIL_TX_POWER_LEVEL_HP_MAX);
	printf("RAIL_SetTxPower: %u\n", r);
}


void rail_callback(RAIL_Handle_t rail, RAIL_Events_t events);

static RAIL_Config_t railCfg = {
	.eventsCallback = &rail_callback,
};

void railtask_init_radio(void)
{
	unsigned r;
	rail = RAIL_Init(&railCfg, NULL);
	r = RAIL_ConfigCal(rail, RAIL_CAL_ALL);
	printf("RAIL_ConfigCal: %u\n", r);
	r = RAIL_ConfigEvents(rail, RAIL_EVENTS_ALL, RAIL_EVENT_RX_FIFO_ALMOST_FULL);
	printf("RAIL_ConfigEvents: %u\n", r);
}


// Extend tuning range by skipping VCO range checks, allowing tuning a bit
// outside of the supported range. Of course the real tuning range will be
// limited by the real tuning range of the VCO, but it is slightly wider
// than the RAIL limits, so this extends it a bit.
// This is done by replacing RAIL internal SYNTH_VcoRangeIsValid function,
// named RAILINT_e1b152b40e799f9ebf7071a91afb3afe in the library.
// Needs linker flag -Wl,-z,muldefs to prevent error
// from multiple definitions of a function with the same name.
uint32_t RAILINT_e1b152b40e799f9ebf7071a91afb3afe(unsigned r0, unsigned r1)
{
	(void)r0; (void)r1;
	//printf("SYNTH_VcoRangeIsValid: %u %u\n", r0, r1);
	// Always return 1 to pretend VCO range is always valid.
	return 1;
}

const char *const rail_assert_errors[] = RAIL_ASSERT_ERROR_MESSAGES;

void RAILCb_AssertFailed(RAIL_Handle_t railHandle, RAIL_AssertErrorCodes_t errorCode)
{
	(void)railHandle;
	printf("RAIL assert failed: %s\n", rail_assert_errors[errorCode]);
}


void railtask_main(void *arg)
{
	(void)arg;
	railtask_init_radio();
	for(;;) {
		bool keyed = p.keyed;
		uint32_t frequency = p.frequency;
		uint32_t split = p.split_freq;

		if (keyed) {
			frequency += split;
		}

		if (frequency != railtask.frequency) {
			railtask_config_channel(frequency);
		}

		RAIL_RadioState_t rs = RAIL_GetRadioState(rail);
		//printf("RAIL_GetRadioState: %08x\n", rs);

		if (keyed
			&& ((rs & RAIL_RF_STATE_TX) == 0)
			&& railtask.config_ok
			&& tx_freq_allowed(railtask.frequency)
		) {
			start_tx_dsp(rail);
		} else if ((!keyed)
			&& ((rs & RAIL_RF_STATE_RX) == 0)
			&& railtask.config_ok
		) {
			if (rs & RAIL_RF_STATE_TX)
				RAIL_StopTxStream(rail);
			start_rx_dsp(rail);
		}
		xSemaphoreTake(railtask_sem, portMAX_DELAY);
	}
}

void railtask_rtos_init(void)
{
	railtask_sem = xSemaphoreCreateBinary();
}
