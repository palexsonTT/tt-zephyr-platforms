/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/util.h>
#include "throttler.h"
#include "aiclk_ppm.h"
#include "cm2bm_msg.h"
#include "fw_table.h"
#include "telemetry_internal.h"
#include "telemetry.h"

#define kThrottlerAiclkScaleFactor 500.0F
#define DEFAULT_BOARD_PWR_LIMIT 150

typedef enum {
	kThrottlerTDP,
	kThrottlerFastTDC,
	kThrottlerTDC,
	kThrottlerThm,
	kThrottlerBoardPwr,
	kThrottlerGDDRThm,
	kThrottlerCount,
} ThrottlerId;

typedef struct {
	float min;
	float max;
} ThrottlerLimitRange;

/* This table is used to restrict the throttler limits to reasonable ranges. */
/* They are passed in from the FW table in SPI */
static const ThrottlerLimitRange throttler_limit_ranges[kThrottlerCount] = {
	[kThrottlerTDP] = {

			.min = 50,
			.max = 500,
		},
	[kThrottlerFastTDC] = {

			.min = 50,
			.max = 500,
		},
	[kThrottlerTDC] = {

			.min = 50,
			.max = 400,
		},
	[kThrottlerThm] = {

			.min = 50,
			.max = 100,
		},
	[kThrottlerBoardPwr] = {

			.min = 50,
			.max = 600,
		},
	[kThrottlerGDDRThm] = {
			.min = 50,
			.max = 100,
	}
};

typedef struct {
	float alpha_filter;
	float p_gain;
	float d_gain;
} ThrottlerParams;

typedef struct {
	const AiclkArbMax arb_max; /* The arbiter associated with this throttler */

	const ThrottlerParams params;
	float limit;
	float value;
	float error;
	float prev_error;
	float output;
} Throttler;

static Throttler throttler[kThrottlerCount] = {
	[kThrottlerTDP] = {

			.arb_max = kAiclkArbMaxTDP,
			.params = {

					.alpha_filter = 1.0,
					.p_gain = 0.2,
					.d_gain = 0,
				},
		},
	[kThrottlerFastTDC] = {

			.arb_max = kAiclkArbMaxFastTDC,
			.params = {

					.alpha_filter = 1.0,
					.p_gain = 0.5,
					.d_gain = 0,
				},
		},
	[kThrottlerTDC] = {

			.arb_max = kAiclkArbMaxTDC,
			.params = {

					.alpha_filter = 0.1,
					.p_gain = 0.2,
					.d_gain = 0,
				},
		},
	[kThrottlerThm] = {

			.arb_max = kAiclkArbMaxThm,
			.params = {

					.alpha_filter = 1.0,
					.p_gain = 0.2,
					.d_gain = 0,
				},
		},
	[kThrottlerBoardPwr] = {
			.arb_max = kAiclkArbMaxBoardPwr,
			.params = {

					.alpha_filter = 1.0,
					.p_gain = 0.2,
					.d_gain = 0,
			}
	},
	[kThrottlerGDDRThm] = {
			.arb_max = kAiclkArbMaxGDDRThm,
			.params = {

					.alpha_filter = 1.0,
					.p_gain = 0.2,
					.d_gain = 0,
				},
	}
};

static void SetThrottlerLimit(ThrottlerId id, float limit)
{
	throttler[id].limit =
		CLAMP(limit, throttler_limit_ranges[id].min, throttler_limit_ranges[id].max);
}

void InitThrottlers(void)
{
	SetThrottlerLimit(kThrottlerTDP, get_fw_table()->chip_limits.tdp_limit);
	SetThrottlerLimit(kThrottlerFastTDC, get_fw_table()->chip_limits.tdc_fast_limit);
	SetThrottlerLimit(kThrottlerTDC, get_fw_table()->chip_limits.tdc_limit);
	SetThrottlerLimit(kThrottlerThm, get_fw_table()->chip_limits.thm_limit);
	SetThrottlerLimit(kThrottlerBoardPwr, DEFAULT_BOARD_PWR_LIMIT);
	SetThrottlerLimit(kThrottlerGDDRThm, get_fw_table()->chip_limits.gddr_thm_limit);
}

static void UpdateThrottler(ThrottlerId id, float value)
{
	Throttler *t = &throttler[id];

	t->value = t->params.alpha_filter * value + (1 - t->params.alpha_filter) * t->value;
	t->error = (t->limit - t->value) / t->limit;
	t->output = t->params.p_gain * t->error + t->params.d_gain * (t->error - t->prev_error);
	t->prev_error = t->error;
}

static void UpdateThrottlerArb(ThrottlerId id)
{
	Throttler *t = &throttler[id];

	float arb_val = aiclk_ppm.arbiter_max[t->arb_max];

	arb_val += t->output * kThrottlerAiclkScaleFactor;

	SetAiclkArbMax(t->arb_max, arb_val);
}

void CalculateThrottlers(void)
{
	TelemetryInternalData telemetry_internal_data;

	ReadTelemetryInternal(1, &telemetry_internal_data);

	UpdateThrottler(kThrottlerTDP, telemetry_internal_data.vcore_power);
	UpdateThrottler(kThrottlerFastTDC, telemetry_internal_data.vcore_current);
	UpdateThrottler(kThrottlerTDC, telemetry_internal_data.vcore_current);
	UpdateThrottler(kThrottlerThm, telemetry_internal_data.asic_temperature);
	UpdateThrottler(kThrottlerBoardPwr, 12 * ConvertTelemetryToFloat(GetInputCurrent()));
	UpdateThrottler(kThrottlerGDDRThm, GetMaxGDDRTemp());

	for (ThrottlerId i = 0; i < kThrottlerCount; i++) {
		UpdateThrottlerArb(i);
	}
}

int32_t Bm2CmSetBoardPwrLimit(const uint8_t *data, uint8_t size)
{
	if (size != 2) {
		return -1;
	}

	uint16_t pwr_limit = *(uint16_t *)data;

	SetThrottlerLimit(kThrottlerBoardPwr, pwr_limit);

	return 0;
}
