/* SPDX-License-Identifier: MIT */

#include <stdlib.h>

#ifndef DSP_TEST
// CMSIS
#include "arm_math.h"
#include "arm_const_structs.h"

// FreeRTOS
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#endif

// rig
#include "rig.h"
#ifndef DSP_TEST
#include "ui.h"
#include "ui_parameters.h"
#endif

#include "dsp.h"
#include "dsp_math.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#define AUDIO_MAXLEN 32
#define IQ_MAXLEN (AUDIO_MAXLEN * 2)
// Frequency step of FM modulator
#define MOD_FM_STEP (38.4e6f / (1UL<<18))

extern rig_parameters_t p;

/* Waterfall FFT related things.
 *
 * Disable waterfall when testing because it depends on ARM FFT
 * library and FreeRTOS, making it more complicated to test outside
 * of microcontroller environment.
 * Testing audio processing code is good enough.
 */
#ifndef DSP_TEST
const arm_cfft_instance_f32 *fftS = &arm_cfft_sR_f32_len256;
QueueHandle_t fft_queue;
#endif
#define SIGNALBUFLEN 512
int16_t signalbuf[2*SIGNALBUFLEN];


static inline float clip(float v, float threshold)
{
	if (v < -threshold)
		return -threshold;
	if (v > threshold)
		return threshold;
	return v;
}

// State of a biquad filter
struct biquad_state {
	float s1_i, s1_q, s2_i, s2_q;
};

// State of a biquad filter for a real-valued signal
struct biquad_state_r {
	float s1, s2;
};

// Coefficients of a biquad filter
struct biquad_coeff {
	float a1, a2, b0, b1, b2;
};

/* Apply a biquad filter to a complex signal with real coefficients,
 * i.e. run it separately for the I and Q parts.
 * Write output back to the same buffer.
 * The algorithm used is called transposed direct form II, as shown at
 * https://www.dsprelated.com/freebooks/filters/Transposed_Direct_Forms.html
 *
 * The code could possibly be optimized by unrolling a couple of times
 * or by cascading multiple stages in a single loop.
 * We'll need some benchmarks to test such ideas.
 */
void biquad_filter(struct biquad_state *s, const struct biquad_coeff *c, iq_float_t *buf, unsigned len)
{
	unsigned i;
	const float a1 = -c->a1, a2 = -c->a2, b0 = c->b0, b1 = c->b1, b2 = c->b2;

	float
	s1_i = s->s1_i,
	s1_q = s->s1_q,
	s2_i = s->s2_i,
	s2_q = s->s2_q;

	for (i = 0; i < len; i++) {
		float in_i, in_q, out_i, out_q;
		in_i = buf[i].i;
		in_q = buf[i].q;
		out_i = s1_i + b0 * in_i;
		out_q = s1_q + b0 * in_q;
		s1_i  = s2_i + b1 * in_i + a1 * out_i;
		s1_q  = s2_q + b1 * in_q + a1 * out_q;
		s2_i  =        b2 * in_i + a2 * out_i;
		s2_q  =        b2 * in_q + a2 * out_q;
		buf[i].i = out_i;
		buf[i].q = out_q;
	}

	s->s1_i = s1_i;
	s->s1_q = s1_q;
	s->s2_i = s2_i;
	s->s2_q = s2_q;
}


/* Apply a biquad filter to one real-valued sample. */
static inline float biquad_sample_r(struct biquad_state_r *s, const struct biquad_coeff *c, float in)
{
	float out;
	out   = s->s1 + c->b0 * in;
	s->s1 = s->s2 + c->b1 * in - c->a1 * out;
	s->s2 =         c->b2 * in - c->a2 * out;
	return out;
}

#define BIQUADS_SSB_N 3

/* Demodulator state */
struct demod {
	// Audio gain parameter
	float audiogain;

	/* Phase of the digital down-converter,
	 * i.e. first oscillator used in SSB demodulation */
	float ddc_i, ddc_q;
	// Frequency of the digital down-converter
	float ddcfreq_i, ddcfreq_q;

	// Phase of the second oscillator in SSB demodulation
	float bfo_i, bfo_q;
	// Frequency of the second oscillator in SSB demodulation
	float bfofreq_i, bfofreq_q;

	// Previous sample stored by FM demodulator
	float fm_prev_i, fm_prev_q;

	// Audio filter state
	float audio_lpf, audio_hpf, audio_po;

	// AGC state
	float agc_amp;

	// Squelch state
	float diff_avg, squelch;

	// S-meter state
	uint64_t smeter_acc;
	unsigned smeter_count;

	unsigned signalbufp;

	enum rig_mode mode;

	// Biquad filter states, used in SSB demodulation
	struct biquad_state bq[BIQUADS_SSB_N];

	enum rig_mode prev_mode;
};


static void demod_reset(struct demod *ds)
{
	ds->fm_prev_i = ds->fm_prev_q = 0;
	ds->audio_lpf = ds->audio_hpf = ds->audio_po = 0;
	ds->agc_amp = 0;
	ds->diff_avg = 0;
	ds->bfo_i = 1; ds->bfo_q = 0;
	ds->ddc_i = 1; ds->ddc_q = 0;
	memset(&ds->bq, 0, sizeof(ds->bq));
}


/* Store samples for waterfall FFT, decimating by 2.
 * Also calculate total signal power for S-meter. */
void demod_store(struct demod *ds, iq_in_t *in, unsigned len)
{
	unsigned i, fp = ds->signalbufp;
	uint64_t acc = ds->smeter_acc;
	for (i = 0; i < len; i += 2) {
		int32_t s0i, s0q, s1i, s1q;
		s0i = in[i].i;
		s0q = in[i].q;
		s1i = in[i+1].i;
		s1q = in[i+1].q;
		signalbuf[fp] = s0i + s1i;
		signalbuf[fp+1] = s0q + s1q;
		acc += s0i * s0i + s0q * s0q;
		acc += s1i * s1i + s1q * s1q;
		fp = (fp + 2) & (SIGNALBUFLEN-2);
		if (fp == 0 || fp == 171*2 || fp == 341*2) {
#ifndef DSP_TEST
			uint16_t msg = fp;
			if (!xQueueSend(fft_queue, &msg, 0)) {
				//++diag.fft_overflows;
			}
#endif
		}
	}
	if((ds->smeter_count += len) >= 0x4000) {
		/* Update S-meter value on display */
		rs.smeter = acc / 0x4000;
		acc = 0;
		ds->smeter_count = 0;

#ifndef DSP_TEST
		display_ev.text_changed = 1;
		xSemaphoreGive(display_sem);
#endif
	}
	ds->signalbufp = fp;
	ds->smeter_acc = acc;
}



/* FM demodulate a buffer.
 * Each I/Q sample is multiplied by the conjugate of the previous sample,
 * giving a value whose complex argument is proportional to the frequency.
 *
 * Instead of actually calculating the argument, a very crude approximation
 * for small values is used instead, but it sounds "good enough" since
 * the input signal is somewhat oversampled.
 *
 * The multiplication results in numbers with a big dynamic range, so
 * floating point math is used.
 *
 * The loop is unrolled two times, so we can nicely reuse the previous
 * sample values already loaded and converted without storing them in
 * another variable.
 * Also, the audio output gets decimated by two by just
 * "integrate and dump". Again, sounds good enough given the oversampling.
 *
 * Average amplitude of differentiated signal is used for squelch.
 */
/*static inline*/ void demod_fm(struct demod *ds, iq_in_t *in, float *out, unsigned len)
{
	unsigned i;
	float s0i, s0q, s1i, s1q;
	s0i = ds->fm_prev_i;
	s0q = ds->fm_prev_q;

	float prev_fm = ds->audio_po, diff_amp = 0;

	for (i = 0; i < len; i+=2) {
		float fi, fq, fm;
		s1i = in[i].i;
		s1q = in[i].q;
		fi = s1i * s0i + s1q * s0q;
		fq = s1q * s0i - s1i * s0q;
		fm = fq / (fabsf(fi) + fabsf(fq));

		s0i = in[i+1].i;
		s0q = in[i+1].q;
		fi += s0i * s1i + s0q * s1q;
		fq += s0q * s1i - s0i * s1q;
		fm += fq / (fabsf(fi) + fabsf(fq));

		// Avoid NaN
		if (fm != fm)
			fm = 0;

		out[i/2] = fm;
		diff_amp += fabsf(fm - prev_fm);
		prev_fm = fm;
	}
	ds->fm_prev_i = s0i;
	ds->fm_prev_q = s0q;

	ds->audio_po = prev_fm;
	float diff_avg = ds->diff_avg;
	if (diff_avg != diff_avg) diff_avg = 0;
	ds->diff_avg = diff_avg + (diff_amp - diff_avg) * .02f;
}


/* Demodulate AM.
 * Again, output audio is decimated by 2.
 *
 * An approximation explained here is used:
 * https://dspguru.com/dsp/tricks/magnitude-estimator/
 */
/*static inline*/ void demod_am(struct demod *ds, iq_in_t *in, float *out, unsigned len)
{
	(void)ds;
	unsigned i;
	const float beta = 0.4142f;
	for (i = 0; i < len; i+=2) {
		float ai, aq, o;
		ai = fabsf((float)in[i].i);
		aq = fabsf((float)in[i].q);
		o = (ai >= aq) ? (ai + aq * beta) : (aq + ai * beta);
		ai = fabsf((float)in[i+1].i);
		aq = fabsf((float)in[i+1].q);
		o += (ai >= aq) ? (ai + aq * beta) : (aq + ai * beta);
		out[i/2] = o;
	}
}


/* Digital down-conversion.
 * This is the first mixer in the Weaver method SSB demodulator.
 *
 * Multiply the signal by a complex oscillator
 * and decimate the result by 2.
 *
 * The oscillator is implemented by "rotating" a complex number on
 * each sample by multiplying it with a value on the unit circle.
 * The value is normalized once per block using formula from
 * https://dspguru.com/dsp/howtos/how-to-create-oscillators-in-software/
 *
 * The previous and next oscillator values alternate between variables
 * osc0 and osc1, and the loop is unrolled for 2 input samples.
 * */
void demod_ddc(struct demod *ds, iq_in_t *in, iq_float_t *out, unsigned len)
{
	(void)ds;
	unsigned i;
	float osc1i, osc1q;
	float osc0i = ds->ddc_i, osc0q = ds->ddc_q;
	const float oscfi = ds->ddcfreq_i, oscfq = ds->ddcfreq_q;
	len /= 2;
	for (i = 0; i < len; i++) {
		float oi, oq, ii, iq;
		ii = in->i;
		iq = in->q;
		in++;
		oi    = osc0i * ii    - osc0q * iq;
		oq    = osc0i * iq    + osc0q * ii;

		osc1i = osc0i * oscfi - osc0q * oscfq;
		osc1q = osc0i * oscfq + osc0q * oscfi;

		ii = in->i;
		iq = in->q;
		in++;
		oi   += osc1i * ii    - osc1q * iq;
		oq   += osc1i * iq    + osc1q * ii;

		osc0i = osc1i * oscfi - osc1q * oscfq;
		osc0q = osc1i * oscfq + osc1q * oscfi;

		out[i].i = oi;
		out[i].q = oq;
	}
	float ms = osc0i * osc0i + osc0q * osc0q;
	ms = (3.0f - ms) * 0.5f;
	ds->ddc_i = ms * osc0i;
	ds->ddc_q = ms * osc0q;
}


/* Demodulate DSB with input in floating point format.
 * This is the second mixer in the Weaver SSB demodulator.
 *
 * Multiply the signal by a beat-frequency oscillator and take
 * the real part of the result.
 *
 * The oscillator is implemented by "rotating" a complex number on
 * each sample by multiplying it with a value on the unit circle.
 * The value is normalized once per block using formula from
 * https://dspguru.com/dsp/howtos/how-to-create-oscillators-in-software/
 *
 * The previous and next oscillator values alternate between variables
 * osc0 and osc1, and the loop is unrolled for 2 input samples.
 * */
void demod_dsb_f(struct demod *ds, iq_float_t *in, float *out, unsigned len)
{
	(void)ds;
	unsigned i;
	float osc1i, osc1q;
	float osc0i = ds->bfo_i, osc0q = ds->bfo_q;
	const float oscfi = ds->bfofreq_i, oscfq = ds->bfofreq_q;
	for (i = 0; i < len; i+=2) {
		out[i] = osc0i * in[i].i - osc0q * in[i].q;
		osc1i = osc0i * oscfi - osc0q * oscfq;
		osc1q = osc0i * oscfq + osc0q * oscfi;

		out[i+1] = osc1i * in[i+1].i - osc1q * in[i+1].q;
		osc0i = osc1i * oscfi - osc1q * oscfq;
		osc0q = osc1i * oscfq + osc1q * oscfi;
	}
	float ms = osc0i * osc0i + osc0q * osc0q;
	ms = (3.0f - ms) * 0.5f;
	ds->bfo_i = ms * osc0i;
	ds->bfo_q = ms * osc0q;
}


/* Coefficients from python3:
from scipy import signal
def p(s): print(',\n'.join("\t{%Ef,%Ef,%Ef,%Ef,%Ef}" % (c[4], c[5], c[0], c[1], c[2]) for c in s))

# SSB
p(signal.cheby1(6, 1, 1200, output='sos', fs=24000))
# CW
p(signal.bessel(6, 200, output='sos', fs=24000))
*/

// Biquad coefficients for SSB
static const struct biquad_coeff biquads_ssb[BIQUADS_SSB_N] = {
	{-1.851822E+00f,8.634449E-01f,8.073224E-07f,1.614645E-06f,8.073224E-07f},
	{-1.846798E+00f,8.992076E-01f,1.000000E+00f,2.000000E+00f,1.000000E+00f},
	{-1.867114E+00f,9.622861E-01f,1.000000E+00f,2.000000E+00f,1.000000E+00f}
};

// Biquad coefficients for CW
static const struct biquad_coeff biquads_cw[BIQUADS_SSB_N] = {
	{-1.906874E+00f,9.091286E-01f,2.867042E-10f,5.734084E-10f,2.867042E-10f},
	{-1.917145E+00f,9.196586E-01f,1.000000E+00f,2.000000E+00f,1.000000E+00f},
	{-1.941944E+00f,9.451818E-01f,1.000000E+00f,2.000000E+00f,1.000000E+00f}
};

/* Demodulate SSB.
 * The Weaver method is used.
 */
void demod_ssb(struct demod *ds, iq_in_t *in, float *out, unsigned len)
{
	iq_float_t buf[IQ_MAXLEN];
	const struct biquad_coeff *filter =
		(ds->mode == MODE_CWU || ds->mode == MODE_CWL)
		? biquads_cw : biquads_ssb;

	demod_ddc(ds, in, buf, len);
	len /= 2;
	unsigned n;
	for (n = 0; n < BIQUADS_SSB_N; n++) {
		biquad_filter(&ds->bq[n], &filter[n], buf, len);
	}
	demod_dsb_f(ds, buf, out, len);
}



/* Apply some low-pass filtering to audio for de-emphasis
 * and some high-pass filtering for DC blocking.
 * Store the result in the same buffer.
 *
 * Also calculate the average amplitude which is used for AGC.
 */
/*static inline*/ void demod_audio_filter(struct demod *ds, float *buf, unsigned len)
{
	unsigned i;
	const float lpf_a = 0.1f, hpf_a = 0.001f;
	float
	lpf = ds->audio_lpf,
	hpf = ds->audio_hpf;
	float amp = 0;
	for (i = 0; i < len; i++) {
		lpf += (buf[i] - lpf) * lpf_a;
		hpf += (lpf - hpf) * hpf_a;
		float o = lpf - hpf;
		buf[i] = o;

		amp += fabsf(o);
	}
	ds->audio_lpf = lpf;
	ds->audio_hpf = hpf;

	/* Update AGC values once per block, so most of the AGC code
	 * runs at a lower sample rate. */
	const float agc_attack = 0.1f, agc_decay = 0.01f;
	float agc_amp = ds->agc_amp;
	// Avoid NaN
	if (agc_amp != agc_amp)
		agc_amp = 0;

	float d = amp - agc_amp;
	if (d >= 0)
		ds->agc_amp = agc_amp + d * agc_attack;
	else
		ds->agc_amp = agc_amp + d * agc_decay;
}


/*static inline*/ void demod_convert_audio(float *in, audio_out_t *out, unsigned len, float gain)
{
	unsigned i;
	for (i = 0; i < len; i++) {
		float f = gain * in[i] + (float)AUDIO_MID;
		audio_out_t o;
		if (f <= (float)AUDIO_MIN)
			o = AUDIO_MIN;
		else if(f >= (float)AUDIO_MAX)
			o = AUDIO_MAX;
		else
			o = (audio_out_t)f;
		out[i] = o;
	}
}


struct demod demodstate;

/* Function to convert received IQ to output audio */
int dsp_fast_rx(iq_in_t *in, int in_len, audio_out_t *out, int out_len)
{
	if (out_len * 2 != in_len || out_len > AUDIO_MAXLEN)
		return 0;

	demod_store(&demodstate, in, in_len);

	enum rig_mode mode = demodstate.mode;
	float audio[AUDIO_MAXLEN];
	switch(mode) {
	case MODE_FM:
		demod_fm(&demodstate, in, audio, in_len);
		break;
	case MODE_AM:
		demod_am(&demodstate, in, audio, in_len);
		break;
	case MODE_USB:
	case MODE_LSB:
	case MODE_CWU:
	case MODE_CWL:
		demod_ssb(&demodstate, in, audio, in_len);
		break;
	default:
		break;
	}

	if (demodstate.diff_avg < demodstate.squelch) {
		// Squelch open
		demod_audio_filter(&demodstate, audio, out_len);
		demod_convert_audio(audio, out, out_len, demodstate.audiogain / demodstate.agc_amp);
	} else {
		// Squelch closed
		int i;
		for (i = 0; i < out_len; i++)
			out[i] = AUDIO_MID;
	}

	return out_len;
}


#define BIQUADS_AUDIO_N 3
/* Biquad filters for audio preprocessing.
 *
 * Sample rate: 24000 Hz
 * First stage: Lowpass, 2000 Hz, Q=2, Gain=0 dB
 * Coefficients from https://arachnoid.com/BiQuadDesigner/
 *
 * Second and third stages: Allpass, 500 Hz, Q=2, Gain=0
 * https://www.earlevel.com/main/2021/09/02/biquad-calculator-v3/
 * Note that this calculator swaps naming of a and b.
 * Not sure if this allpass is a good idea but let's give it a try.
 */
static const struct biquad_coeff biquads_audio[BIQUADS_AUDIO_N] = {
{
	.a1 = -1.53960072f,
	.a2 =  0.77777778f,
	.b0 =  0.05954426f,
	.b1 =  0.11908853f,
	.b2 =  0.05954426f
},
{
	.a1 = -1.9202296564369383f,
	.a2 =  0.9367992424471727f,
	.b0 =  0.9367992424471727f,
	.b1 = -1.9202296564369383f,
	.b2 =  1.0f
},
{
	.a1 = -1.9202296564369383f,
	.a2 =  0.9367992424471727f,
	.b0 =  0.9367992424471727f,
	.b1 = -1.9202296564369383f,
	.b2 =  1.0f
},
};

/* Modulator state */
struct modstate {
	// Input audio processing
	float hpf, hpf2, agc_lpf, agc_amp;

	// FM specific processing
	float limitergain, clipint, qerr;

	// CTCSS oscillator
	float ct_i, ct_q;
	// CTCSS oscillator frequency
	float ctfreq_i, ctfreq_q;

	// SSB specific processing

	// Phase accumulator for I/Q to FM conversion
	uint32_t pha;
	// Previous value of frequency modulation
	int32_t fm_prev;

	// Phase of the first oscillator in SSB modulation
	float bfo_i, bfo_q;
	// Frequency of the first oscillator in SSB modulation
	float bfofreq_i, bfofreq_q;
	// SSB power estimate for adding carrier in quiet moments
	float plpf;

	enum rig_mode mode;

	// Audio preprocess biquad filter states
	struct biquad_state_r bqa[BIQUADS_AUDIO_N];
	// SSB biquad filter states
	struct biquad_state bq[BIQUADS_SSB_N];
};

static void mod_reset(struct modstate *m)
{
	m->ct_i  = 1.0f; m->ct_q  = 0.0f;
	m->bfo_i = 1.0f; m->bfo_q = 0.0f;
	memset(&m->bqa, 0, sizeof(m->bqa));
	memset(&m->bq, 0, sizeof(m->bq));
	// TODO: maybe reset everything else too
}

/* Preprocess transmit audio.
 * This includes some filtering and AGC. */
static void mod_process_audio(struct modstate *m, audio_in_t *in, float *out, unsigned len)
{
	const float agc_minimum = 10.0f;
	const float agc_lpf_a = 0.2f;
	const float agc_attack = 0.1f, agc_decay = 0.002f;

	float hpf = m->hpf;
	struct biquad_state_r bqa[BIQUADS_AUDIO_N] = {
		m->bqa[0], m->bqa[1], m->bqa[2]
	};

	float amp = 0.0f;
	unsigned i;
	for (i = 0; i < len; i++) {
		float audio = (float)in[i];
		// DC block, 600 Hz highpass
		hpf += (audio - hpf) * .145f;
		audio -= hpf;

		unsigned n;
		for (n = 0; n < BIQUADS_AUDIO_N; n++) {
			audio = biquad_sample_r(&bqa[n], &biquads_audio[n], audio);
		}

		amp += fabsf(audio);

		out[i] = audio;
	}
	m->hpf = hpf;
	m->bqa[0] = bqa[0];
	m->bqa[1] = bqa[1];
	m->bqa[2] = bqa[2];

	// Update AGC values once per block, so most of the AGC code
	// runs at a lower sample rate.

	amp /= (float)len;

	float agc_lpf = m->agc_lpf, agc_amp = m->agc_amp;
	agc_lpf += (amp - agc_lpf) * agc_lpf_a;
	m->agc_lpf = agc_lpf;
	amp = agc_lpf;

	// Avoid NaN, clamp to a minimum value
	if (agc_amp != agc_amp || agc_amp < agc_minimum)
		agc_amp = agc_minimum;

	float d = amp - agc_amp;
	if (d >= 0)
		agc_amp = agc_amp + d * agc_attack;
	else
		agc_amp = agc_amp + d * agc_decay;

	m->agc_amp = agc_amp;
	float gain = 1.0f / agc_amp;

	for (i = 0; i < len; i++) {
		out[i] *= gain;
	}
}


/* Modulate FM from preprocessed audio */
static void mod_fm(struct modstate *m, float *in, fm_out_t *out, unsigned len)
{
	const float limitergain_min = 0.2f, limitergain_max = 1.0f;
	// CTCSS deviation
	const float ctdev = 650.0f / MOD_FM_STEP;

	float hpf2 = m->hpf2;
	float limitergain = m->limitergain;
	float clipint = m->clipint, qerr = m->qerr;

	float ct_i = m->ct_i, ct_q = m->ct_q;
	const float ctfreq_i = m->ctfreq_i, ctfreq_q = m->ctfreq_q;

	unsigned i;
	for (i = 0; i < len; i++) {
		float audio = in[i] * 200.0f;

		// Preemphasis: 2000 Hz highpass
		hpf2 += (audio - hpf2) * .4f;
		audio -= hpf2;

		// Pre-clip largest peaks, should not happen that often
		audio = clip(audio, 100.0f);

		audio *= limitergain;

		// Avoid producing DC offsets while clipping asymmetric waveforms
		// by integrating the clipped signal and feeding it back into input.
		// This works as a 200 Hz high pass filter while not clipping.
		audio -= clipint * .051f;

		// Also reduce limiter gain when it is getting close to clipping.
		if (fabsf(audio) >= 20.0f) {
			limitergain *= .95f;
		} else {
			limitergain *= 1.002f;
			if(limitergain > limitergain_max)
				limitergain = limitergain_max;
		}
		if (limitergain < limitergain_min)
			limitergain = limitergain_min;

		audio = clip(audio, 25.0f);
		// DC offset integrator
		clipint += audio;

		if (ctfreq_q != 0.0f) {
			audio += ct_q * ctdev;
			float new_i = ct_i * ctfreq_i - ct_q * ctfreq_q;
			ct_q        = ct_i * ctfreq_q + ct_q * ctfreq_i;
			ct_i = new_i;
		}
		audio += 32.0f;

		// Dither using a delta sigma modulator based on
		// quantization error from previous sample.
		audio += qerr;
		fm_out_t quantized = (fm_out_t)audio;
		qerr = audio - (float)quantized;
		out[i] = quantized;
	}

	m->hpf2 = hpf2;
	m->limitergain = limitergain;
	m->clipint = clipint;
	m->qerr = qerr;

	float ms = ct_i * ct_i + ct_q * ct_q;
	ms = (3.0f - ms) * 0.5f;
	m->ct_i = ms * ct_i;
	m->ct_q = ms * ct_q;
}


/* Modulate DSB from preprocessed audio.
 * This works similarly to demod_dsb_f
 * but from real-valued audio to I/Q samples.
 * Carrier is written to a buffer to be used later. */
static void mod_dsb(struct modstate *m, float *in, iq_float_t *out, iq_float_t *carrier, unsigned len)
{
	float osc1i, osc1q;
	float osc0i = m->bfo_i, osc0q = m->bfo_q;
	const float oscfi = m->bfofreq_i, oscfq = m->bfofreq_q;

	unsigned i;
	for (i = 0; i < len; i+=2) {
		float audio = (float)in[i];
		carrier[i  ].i = osc0i;
		carrier[i  ].q = osc0q;
		out[i  ].i = osc0i * audio;
		out[i  ].q = osc0q * audio;
		osc1i = osc0i * oscfi - osc0q * oscfq;
		osc1q = osc0i * oscfq + osc0q * oscfi;

		audio = (float)in[i+1];
		carrier[i+1].i = osc0i;
		carrier[i+1].q = osc0q;
		out[i+1].i = osc1i * audio;
		out[i+1].q = osc1q * audio;
		osc0i = osc1i * oscfi - osc1q * oscfq;
		osc0q = osc1i * oscfq + osc1q * oscfi;
	}
	float ms = osc0i * osc0i + osc0q * osc0q;
	ms = (3.0f - ms) * 0.5f;
	m->bfo_i = ms * osc0i;
	m->bfo_q = ms * osc0q;
}

/* Add some carrier to SSB signal when its power is low.
 * This gives something to transmit when audio is quiet. */
static void mod_ssb_add_carrier(struct modstate *m, iq_float_t *buf, const iq_float_t *carrier, unsigned len)
{
	const float pthreshold = 0.3f, carrier_level = 0.05f;

	float plpf = m->plpf;

	// Estimate power
	float power = 0.0f;
	unsigned i;
	for (i = 0; i < len; i++) {
		float vi = buf[i].i, vq = buf[i].q;
		power += vi * vi + vq * vq;
	}
	// Lowpass filter the estimate
	plpf += (power - plpf) * 0.5f;
	// Amount of carrier to add
	float c = 0.0f;
	if (plpf < pthreshold) {
		c = (1.0f - plpf / pthreshold) * carrier_level;
	}
	// Add carrier
	for (i = 0; i < len; i++) {
		buf[i].i += carrier[i].i * c;
		buf[i].q += carrier[i].q * c;
	}

	m->plpf = plpf;
}


/* Convert I/Q to FM modulation.
 * This uses only the phase angle from I/Q samples and modulates
 * frequency so that resulting phase tracks that of I/Q input. */
static void mod_iq_to_fm(struct modstate *m, iq_float_t *in, fm_out_t *out, unsigned len, int fm_offset)
{
	// Phase accumulator change per sample per FM quantization step.
	// 2**32 * (38.4 MHz / (2**18)) / 24 kHz
	// Multiplied by 2 because
	// filtering of FM modulation doubles the values.
	const int32_t phdev = 26214400 *2;

	// Maximum frequency deviation in steps,
	// divided by 2 because
	// filtering of FM modulation doubles the values.
	const int32_t fm_max = 12 /2;

	uint32_t pha = m->pha;

	int32_t fm_prev = m->fm_prev;

	unsigned i;
	for (i = 0; i < len; i++) {
		// Represent phase as uint32_t so we can avoid computing modulos
		// by letting the numbers wrap around.
		//uint32_t ph = (uint32_t)(atan2f(in[i].q, in[i].i) * 6.8356528e+08f);
		uint32_t ph = approx_angle(in[i].q, in[i].i);

		// Phase difference from current phase accumulator
		int32_t phdiff = (int32_t)(ph - pha);

		// Quantize to FM modulation steps.
		// I think that "ideally", we would divide phdiff by phdev
		// and round the result to find the frequency that gets us
		// closest to the target phase during a sample.
		// Dividing by a slightly higher value, however,
		// seems to make the loop behave more nicely.
		// Exact value is not critical since it is a part of a
		// feedback loop anyway, so we can optimize it a bit by
		// using a power of two, so that division can be
		// implemented as a bit shift.
		// Right shift of negative numbers is implementation
		// defined so handle negative numbers separately
		// to avoid doing that and to also round it correctly.
		int32_t fm = (phdiff >= 0)
			?  (( phdiff + (1<<26)) >> 27)
			: -((-phdiff + (1<<26)) >> 27);

		// Clamp to maximum deviation
		if (fm < -fm_max)
			fm = -fm_max;
		if (fm >  fm_max)
			fm =  fm_max;

		// Filter FM modulation to reduce high frequency noise
		int32_t fm_filtered = fm + fm_prev;
		out[i] = fm_filtered + fm_offset;

		// Output phase does not exactly follow I/Q phase
		// due to frequency clamping and quantization.
		// Make the phase accumulator follow the actual output phase.
		pha += fm * phdev;
		fm_prev = fm;
	}

	m->pha = pha;
	m->fm_prev = fm_prev;
}

// Center frequency for SSB modulation in FM quantization steps
#define MOD_SSB_CENTER 10

/* Modulate SSB from preprocessed audio */
static void mod_ssb(struct modstate *m, float *in, fm_out_t *out, unsigned len)
{
	iq_float_t buf[AUDIO_MAXLEN];
	iq_float_t carrier[AUDIO_MAXLEN];

	mod_dsb(m, in, buf, carrier, len);

	unsigned n;
	for (n = 0; n < BIQUADS_SSB_N; n++) {
		biquad_filter(&m->bq[n], &biquads_ssb[n], buf, len);
	}
	mod_ssb_add_carrier(m, buf, carrier, len);
	mod_iq_to_fm(m, buf, out, len,
		m->mode == MODE_USB ?
		(32+MOD_SSB_CENTER) : (32-MOD_SSB_CENTER));
}


struct modstate modstate;

/* Function to convert input audio to transmit frequency modulation */
int dsp_fast_tx(audio_in_t *in, fm_out_t *out, int len)
{
	struct modstate *m = &modstate;
	float audio[AUDIO_MAXLEN];
	assert (len <= AUDIO_MAXLEN);

	mod_process_audio(m, in, audio, len);

	int i;

	switch (p.mode) {
	case MODE_FM:
		mod_fm(m, audio, out, len);
		break;
	case MODE_USB:
	case MODE_LSB:
		mod_ssb(m, audio, out, len);
		break;
	default:
		// Transmit unmodulated carrier on other modes
		for (i = 0; i < len; i++) {
			out[i] = 32;
		}
	}
	return 0;
}


void dsp_update_params(void)
{
	enum rig_mode mode = p.mode;

	float bfo = 0.0f, ddc_offset = 0.0f;
	float bfo_tx = 0.0f;
	switch (mode) {
	case MODE_USB:
		bfo = 1400.0f;
		ddc_offset = bfo;
		bfo_tx = -146.48438f * MOD_SSB_CENTER;
		break;
	case MODE_LSB:
		bfo = -1400.0f;
		ddc_offset = bfo;
		bfo_tx =  146.48438f * MOD_SSB_CENTER;
		break;
	case MODE_CWU:
		bfo = 698.46f;
		ddc_offset = 0.0f;
		break;
	case MODE_CWL:
		bfo = -698.46f;
		ddc_offset = 0.0f;
		break;
	default:
		break;
	}

	float f;
	f = (6.2831853f * 2.0f / RX_IQ_FS) * bfo;
	demodstate.bfofreq_i = cosf(f);
	demodstate.bfofreq_q = sinf(f);

	f = (-6.2831853f / RX_IQ_FS) * ((float)p.offset_freq + ddc_offset);
	demodstate.ddcfreq_i = cosf(f);
	demodstate.ddcfreq_q = sinf(f);

	f = (6.2831853f / TX_FS) * bfo_tx;
	modstate.bfofreq_i = cosf(f);
	modstate.bfofreq_q = sinf(f);

	float ctcss = p.ctcss;
	if (mode == MODE_FM && ctcss != 0.0f) {
		f = (6.2831853f / TX_FS) * ctcss;
		modstate.ctfreq_i = cosf(f);
		modstate.ctfreq_q = sinf(f);
	} else {
		modstate.ctfreq_i = 1.0f;
		modstate.ctfreq_q = 0.0f;
	}

	unsigned vola = p.volume;
	demodstate.audiogain = ((vola&1) ? (3<<(vola/2)) : (2<<(vola/2))) * 10.0f;

	demodstate.squelch = 1.0f * p.squelch;

	demodstate.mode = mode;
	modstate.mode = mode;
	/* Reset state after mode change */
	if (mode != demodstate.prev_mode) {
		demod_reset(&demodstate);
		mod_reset(&modstate);
		demodstate.prev_mode = mode;
	}
}


#ifndef DSP_TEST
static void calculate_waterfall_line(unsigned sbp)
{
	extern uint8_t displaybuf2[3*(FFT_BIN2-FFT_BIN1)];
	unsigned i;
	float mag_avg = 0;

	/* These are static because so such big arrays would not be allocated from stack.
	 * Not sure if this is a good idea.
	 * If averaging were not used, mag could actually reuse fftdata
	 * with some changes to indexing.
	 */
	static float fftdata[2*FFTLEN], mag[FFTLEN];
	static uint8_t averages = 0;

	/* sbp is the message received from the fast DSP task,
	 * containing the index of the latest sample written by it.
	 * Take one FFT worth of previous samples before it. */
	sbp -= 2*FFTLEN;

	for(i=0; i<2*FFTLEN; i+=2) {
		sbp &= SIGNALBUFLEN-1;
		fftdata[i]   = signalbuf[sbp];
		fftdata[i+1] = signalbuf[sbp+1];
		sbp += 2;
	}

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

	display_ev.waterfall_line = 1;
	xSemaphoreGive(display_sem);
}


/* A task for DSP operations that can take a longer time */
void slow_dsp_task(void *arg) {
	(void)arg;
	for(;;) {
		uint16_t msg;
		if (xQueueReceive(fft_queue, &msg, portMAX_DELAY)) {
			calculate_waterfall_line(msg);
		}
	}
}


void slow_dsp_rtos_init(void)
{
	fft_queue = xQueueCreate(1, sizeof(uint16_t));
}
#endif
