// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * chumby-alarm-tone: the alarm sound. Beeps (four short pulses, then a
 * pause) on the default ALSA device, getting louder over the first 30 s,
 * until it is killed. The shell starts and stops it. If the device is busy
 * or not there yet it keeps retrying, so a ring is never lost.
 */

#include <alsa/asoundlib.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>

#define RATE		22050
#define SLOT_MS		100	/* pattern step */
#define FREQ		1760.0	/* A6: cuts through on the small speaker */
#define RAMP_S		30

static volatile sig_atomic_t quit;

static void on_term(int sig)
{
	(void)sig;
	quit = 1;
}

/* 14 slots of 100 ms: beep, gap, beep, gap, beep, gap, beep, 700 ms rest. */
static int slot_on(unsigned int n)
{
	n %= 14;
	return n < 8 && !(n & 1);
}

int main(void)
{
	static short buf[RATE * SLOT_MS / 1000];
	const int frames = sizeof(buf) / sizeof(buf[0]);
	unsigned int slot = 0;
	double phase = 0;

	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);

	while (!quit) {
		snd_pcm_t *pcm;

		if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
			sleep(1);
			continue;
		}
		if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
				       1, RATE, 1, 300000) < 0) {
			snd_pcm_close(pcm);
			sleep(1);
			continue;
		}
		while (!quit) {
			double t = slot * SLOT_MS / 1000.0;
			double amp = t >= RAMP_S ? 0.9 : 0.25 + 0.65 * t / RAMP_S;
			snd_pcm_sframes_t w;
			int i;

			for (i = 0; i < frames; i++) {
				double env = 1.0;

				/* 5 ms fade in/out: no clicks. */
				if (i < RATE / 200)
					env = i / (RATE / 200.0);
				else if (i > frames - RATE / 200)
					env = (frames - i) / (RATE / 200.0);
				buf[i] = slot_on(slot) ?
					(short)(32767 * amp * env * sin(phase)) : 0;
				phase += 2 * M_PI * FREQ / RATE;
				if (phase > 2 * M_PI)
					phase -= 2 * M_PI;
			}
			slot++;
			w = snd_pcm_writei(pcm, buf, frames);
			if (w < 0 && snd_pcm_recover(pcm, w, 1) < 0)
				break;	/* device went away: reopen */
		}
		snd_pcm_drop(pcm);
		snd_pcm_close(pcm);
	}
	return 0;
}
