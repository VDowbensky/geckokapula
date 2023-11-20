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
				uint64_t vco = (uint64_t)f * d1 * d2 * d3;
				// RAIL only accepts VCO frequencies in the range 2.3-2.9 GHz.
				// If it's within the range, use this combination of dividers.
				// Use goto to break from all 3 nested loops.
				if (vco > 2300000000ULL && vco < 2900000000ULL)
					goto divider_found;
			}
		}
	}
	// Divider combination not found
	return 0;
divider_found:
	*ratio = d1 * d2 * d3;
	if (d1 == 1) d1 = 0;
	if (d2 == 1) d2 = 0;
	return (d1 << 6) | (d2 << 3) | d3;
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


/* RAIL 2 allows implementing an assert failed function, so it doesn't only
 * get stuck in a infinite loop inside RAIL anymore.
 * This, however, still doesn't let us tune outside the "allowed" range,
 * so wrapping the internal assert may still be needed. Even that, however,
 * doesn't seem to help anymore! */

/* Skip RAIL asserts to extend the tuning range.
 * Needs linker parameter -Wl,--wrap=RAILINT_999bd22c50df2f99ce048cba68f11c3a */
uint32_t __wrap_RAILINT_999bd22c50df2f99ce048cba68f11c3a(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3)
{
	printf("RAILInt_Assert: %08lx %08lx %08lx %08lx\n", r0, r1, r2, r3);
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
