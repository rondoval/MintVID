/*
 * MintVID - software gain for volume above 100%, with a peak limiter.
 *
 * Up to 100% the output backends set the volume themselves (audio.device and
 * ahi.device both take one). Above it the samples are amplified here, and a
 * peak limiter shared by all channels keeps the result at or under
 * MR_LIMITER_CEILING: where a sample would go over, the gain drops at once to
 * what that sample allows, then recovers at about 20-25 dB per second. Loud
 * passages are held back instead of clipped, quiet ones get the whole gain.
 * There is no look-ahead: a multiply per sample, a divide only where one
 * would go over.
 */
#ifndef MR_LIMITER_H
#define MR_LIMITER_H

#include <stdint.h>

#define MR_LIMITER_CEILING 29204          /* -1 dBFS: room for resampling */
#define MR_LIMITER_UNITY   (1UL << 24)    /* gains are Q24 */
#define MR_LIMITER_MAX     (4UL << 24)    /* +12 dB */

typedef struct mr_limiter {
    uint32_t target;            /* gain asked for; MR_LIMITER_UNITY is off */
    uint32_t gain;              /* gain applied now, <= target */
    unsigned release_shift;     /* gain grows by gain >> shift per frame */
} mr_limiter;

/* Unity gain (off), recovering at about 20-25 dB/s for PCM at this rate. */
void mr_limiter_init(mr_limiter *l, unsigned rate);

/* Ask for a gain of num/den, held to unity..MR_LIMITER_MAX. A lower gain
 * applies at once, a higher one is reached at the recovery rate, from unity
 * when the limiter was off. */
void mr_limiter_set_gain(mr_limiter *l, unsigned num, unsigned den);

/* Nonzero when mr_limiter_run() changes samples, i.e. above unity. */
int mr_limiter_active(const mr_limiter *l);

/* Amplify frames of interleaved signed 16-bit PCM from src into dst (which
 * may be src) and limit them. */
void mr_limiter_run(mr_limiter *l, short *dst, const short *src,
                    unsigned frames, int channels);

#endif /* MR_LIMITER_H */
