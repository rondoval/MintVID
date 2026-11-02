/*
 * Volume above 100%: audio/mr_limiter.c's gain and peak limiter. Checks the
 * gains asked for, that quiet PCM gets the whole gain, that nothing ever goes
 * past the ceiling (full-scale input, -32768, at the highest gain), that the
 * channels share one gain, that a lower volume applies at once, and how fast
 * the gain recovers after a loud passage.
 */
#include "../audio/mr_limiter.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI                        /* not in strict C99's <math.h> */
#define M_PI 3.14159265358979323846
#endif

#define RATE 48000

static int failures;

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "limiter check failed: %s\n", message);
        failures++;
    }
}

/* Stereo sine of this amplitude, right channel at right_scale of the left. */
static void sine(short *pcm, unsigned frames, double amplitude,
                 double right_scale, unsigned *phase)
{
    for (unsigned i = 0; i < frames; i++, (*phase)++) {
        double v = amplitude * sin(2.0 * M_PI * 440.0 * *phase / RATE);
        pcm[2 * i] = (short)lrint(v);
        pcm[2 * i + 1] = (short)lrint(v * right_scale);
    }
}

static int peak(const short *pcm, unsigned n)
{
    int p = 0;
    for (unsigned i = 0; i < n; i++)
        if (abs(pcm[i]) > p) p = abs(pcm[i]);
    return p;
}

int main(void)
{
    enum { BLOCK = 4800 };              /* 100 ms */
    static short in[2 * BLOCK], out[2 * BLOCK];
    mr_limiter l;
    unsigned phase = 0;

    mr_limiter_init(&l, RATE);
    expect(l.release_shift == 14, "48 kHz recovers by gain >> 14 a frame");
    expect(!mr_limiter_active(&l), "off after init");
    mr_limiter_set_gain(&l, 64, 64);
    expect(!mr_limiter_active(&l), "100% is off");
    mr_limiter_set_gain(&l, 96, 64);
    expect(l.target == (3UL << 23), "150% is 1.5");
    expect(l.gain == MR_LIMITER_UNITY, "a higher gain starts from unity");
    mr_limiter_set_gain(&l, 128, 64);
    expect(l.target == (2UL << 24), "200% is 2.0");
    mr_limiter_set_gain(&l, 1000, 64);
    expect(l.target == MR_LIMITER_MAX, "held to MR_LIMITER_MAX");

    /* -24 dBFS at 400%: the whole gain once recovered, nothing limited. */
    mr_limiter_set_gain(&l, 256, 64);
    for (int b = 0; b < 10; b++) {
        sine(in, BLOCK, 2065.0, 1.0, &phase);
        mr_limiter_run(&l, out, in, BLOCK, 2);
    }
    expect(l.gain == MR_LIMITER_MAX, "reaches 4.0 within a second");
    sine(in, BLOCK, 2065.0, 1.0, &phase);
    mr_limiter_run(&l, out, in, BLOCK, 2);
    {
        int worst = 0;
        for (unsigned i = 0; i < 2 * BLOCK; i++) {
            int d = abs(out[i] - in[i] * 4);
            if (d > worst) worst = d;
        }
        expect(worst <= 1, "quiet PCM gets exactly 4x");
    }

    /* Full scale at 400%: never past the ceiling, and close to it. */
    for (int b = 0; b < 5; b++) {
        sine(in, BLOCK, 32767.0, 1.0, &phase);
        mr_limiter_run(&l, out, in, BLOCK, 2);
        expect(peak(out, 2 * BLOCK) <= MR_LIMITER_CEILING, "full scale held");
    }
    expect(peak(out, 2 * BLOCK) >= MR_LIMITER_CEILING * 9 / 10,
           "full scale limited to near the ceiling, not far below");

    /* -32768 right after silence, at the highest gain. */
    for (unsigned i = 0; i < 2 * BLOCK; i++) in[i] = 0;
    for (int b = 0; b < 10; b++) mr_limiter_run(&l, out, in, BLOCK, 2);
    in[0] = -32768; in[1] = 32767; in[2] = -32768; in[3] = -32768;
    mr_limiter_run(&l, out, in, 2, 2);
    expect(peak(out, 4) <= MR_LIMITER_CEILING, "-32768 held");
    expect(out[0] < 0 && out[1] > 0, "signs kept");

    /* Recovery: from about -1 dB back to +12 dB takes about half a second. */
    for (unsigned i = 0; i < 2 * BLOCK; i++) in[i] = 0;
    {
        unsigned frames = 0;
        while (l.gain < MR_LIMITER_MAX && frames < 4 * RATE) {
            mr_limiter_run(&l, out, in, 100, 2);
            frames += 100;
        }
        expect(frames >= RATE * 3 / 10 && frames <= RATE * 8 / 10,
               "recovers 13 dB in 0.3-0.8 s");
    }

    /* One gain for both channels: a loud left pulls the right down too. */
    sine(in, BLOCK, 32767.0, 0.1, &phase);
    mr_limiter_run(&l, out, in, BLOCK, 2);
    {
        int ok = 1;
        for (unsigned i = BLOCK / 2; i < BLOCK; i++) {
            long l4 = (long)out[2 * i] * in[2 * i + 1];
            long r4 = (long)out[2 * i + 1] * in[2 * i];
            if (in[2 * i] && labs(l4 - r4) > 2L * abs(in[2 * i])) ok = 0;
        }
        expect(ok, "channels share the gain");
    }

    /* A lower volume applies at once; 100% switches it off. */
    mr_limiter_set_gain(&l, 128, 64);
    expect(l.gain <= (2UL << 24), "a lower gain applies at once");
    mr_limiter_set_gain(&l, 64, 64);
    expect(!mr_limiter_active(&l) && l.gain == MR_LIMITER_UNITY,
           "back to 100% is off");

    /* In place, mono. */
    mr_limiter_init(&l, 22050);
    mr_limiter_set_gain(&l, 128, 64);
    l.gain = l.target;
    for (unsigned i = 0; i < BLOCK; i++) in[i] = (short)(i & 1 ? -1000 : 1000);
    mr_limiter_run(&l, in, in, BLOCK, 1);
    expect(in[0] == 2000 && in[1] == -2000, "in place, mono, 2x");

    if (failures) return 1;
    printf("limiter: all checks passed\n");
    return 0;
}
