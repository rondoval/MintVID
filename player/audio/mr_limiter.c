#include "mr_limiter.h"

void mr_limiter_init(mr_limiter *l, unsigned rate)
{
    unsigned shift = 0;
    /* Growing by 1 + 2^-shift per frame is 8.7 * rate / 2^shift dB/s:
     * 17-25 dB/s with 2^shift the first power of two >= rate / 3. */
    while (shift < 20 && (1UL << shift) < rate / 3) shift++;
    l->release_shift = shift;
    l->target = l->gain = MR_LIMITER_UNITY;
}

void mr_limiter_set_gain(mr_limiter *l, unsigned num, unsigned den)
{
    uint32_t target = MR_LIMITER_UNITY;
    if (den && num > den)
        target = num / den >= MR_LIMITER_MAX >> 24 ? MR_LIMITER_MAX
               : ((uint32_t)(num / den) << 24) +
                 ((((uint32_t)(num % den) << 16) / den) << 8);
    if (target == MR_LIMITER_UNITY || l->gain > target) l->gain = target;
    l->target = target;
}

int mr_limiter_active(const mr_limiter *l)
{
    return l->target > MR_LIMITER_UNITY;
}

void mr_limiter_run(mr_limiter *l, short *dst, const short *src,
                    unsigned frames, int channels)
{
    const uint32_t limit = (uint32_t)MR_LIMITER_CEILING << 12;
    uint32_t gain = l->gain;
    for (unsigned i = 0; i < frames; i++) {
        uint32_t peak = 0;
        for (int c = 0; c < channels; c++) {
            int s = src[c];
            uint32_t m = (uint32_t)(s < 0 ? -s : s);
            if (m > peak) peak = m;
        }
        /* The products in Q12: samples are at most 2^15 and the gain at
         * most 2^14, so they fit in 32 bits. */
        uint32_t g = gain >> 12;
        if (peak * g > limit) {
            g = limit / peak;
            gain = g << 12;
        }
        for (int c = 0; c < channels; c++)
            dst[c] = (short)(((int32_t)src[c] * (int32_t)g) >> 12);
        src += channels;
        dst += channels;
        gain += gain >> l->release_shift;
        if (gain > l->target) gain = l->target;
    }
    l->gain = gain;
}
