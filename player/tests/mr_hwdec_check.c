/*
 * core/mr_hwdec.c against a fake hardware backend: pipeline delay, picture
 * order and timestamps, the parameter sets in front of the first unit after
 * attach and reset, units split across input buffers, a player that never
 * drains, a backend stalled for output room, flush and reset, failure, and
 * that the backend always keeps a buffer to write into.
 */
#include "../core/mr_hwdec.h"
#include "../core/mr_avc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STUB(n) const mr_codec mr_codec_##n={#n,{0},0,0,0,0}
STUB(cinepak); STUB(mjpeg); STUB(mpeg2); STUB(mpeg4); STUB(msmpeg4v2);
STUB(wmv1); STUB(wmv2); STUB(h263); STUB(msvideo1); STUB(rle);
STUB(rawvideo); STUB(dv); STUB(h264);

static int fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", \
    __FILE__, __LINE__, #c); fail = 1; } } while (0)

/* ---- the fake backend ------------------------------------------------ */

#define FK_PKTS 4
#define FK_OUTS 8
#define FK_PEND 64
#define FK_LOG  64

typedef struct {
    /* behaviour */
    uint32_t capacity;
    unsigned out_count;
    unsigned latency;     /* pictures it keeps before the first comes out  */
    int      reorder;     /* out in timestamp order within the window      */
    unsigned burst;       /* out only in groups of this many (0: as ready) */
    unsigned need_free;   /* free outputs it needs before emitting (>= 1)  */
    unsigned in_need_free;/* free outputs it needs to take input           */
    unsigned npkts;       /* input buffers it lends (0: FK_PKTS)           */
    int      bursting;
    int      fail_after;  /* sends before it fails (0: never)              */
    /* state */
    uint8_t  pkt_buf[FK_PKTS][4096];
    int      pkt_lent[FK_PKTS];
    int      pkt_back[FK_PKTS];   /* sent, waiting to come back            */
    uint8_t  au[8192];
    uint32_t au_len;
    int      au_has_pts;
    uint64_t au_pts;
    struct { uint64_t pts; int has_pts; } pend[FK_PEND];
    unsigned npend;
    int      out_lent[FK_OUTS];
    uint8_t  out_pix[FK_OUTS][16];
    int      eos, ended_told, failed, sends;
    unsigned flushes, set_outputs;
    /* what went in: whole units, and the flags of each buffer */
    uint8_t  log[FK_LOG][8192];
    uint32_t log_len[FK_LOG];
    unsigned nlog;
    unsigned flags_log[256];
    unsigned nflags;
    unsigned waited_ms;
} fake;

static fake fk;

static unsigned outs_lent(void)
{
    unsigned i, n = 0;
    for (i = 0; i < fk.out_count; i++) n += fk.out_lent[i];
    return n;
}

static int fk_poll(void *be, mr_hwdec_item *item)
{
    unsigned i;
    (void)be;
    memset(item, 0, sizeof *item);
    if (fk.failed) {
        item->kind = MR_HWDEC_FAILED;
        return 1;
    }
    /* Input buffers it has taken in: only while it has room to decode. */
    for (i = 0; i < (fk.npkts ? fk.npkts : FK_PKTS); i++) {
        if (!fk.pkt_lent[i] && (fk.pkt_back[i] || fk.nlog == 0) &&
            fk.npend < fk.latency + fk.out_count &&
            fk.out_count - outs_lent() >= fk.in_need_free) {
            fk.pkt_lent[i] = 1;
            fk.pkt_back[i] = 0;
            item->kind = MR_HWDEC_PACKET;
            item->packet.data = fk.pkt_buf[i];
            item->packet.capacity = fk.capacity;
            item->packet.handle = (void *)(size_t)(i + 1);
            return 1;
        }
    }
    if (fk.burst && fk.npend >= fk.burst + fk.latency) fk.bursting = 1;
    if (fk.npend <= fk.latency) fk.bursting = 0;
    if (fk.npend && (fk.eos || fk.npend > fk.latency) &&
        (!fk.burst || fk.eos || fk.bursting) &&
        fk.out_count - outs_lent() >= (fk.need_free ? fk.need_free : 1)) {
        unsigned pick = 0, window = fk.npend < fk.latency + 1 ? fk.npend
                                                              : fk.latency + 1;
        for (i = 0; fk.reorder && i < window; i++)
            if (fk.pend[i].pts < fk.pend[pick].pts) pick = i;
        for (i = 0; i < fk.out_count && fk.out_lent[i]; i++) {}
        fk.out_lent[i] = 1;
        item->kind = MR_HWDEC_PICTURE;
        item->picture.fmt = MR_PIX_BGR24;
        item->picture.width = 4;
        item->picture.height = 1;
        item->picture.plane[0] = fk.out_pix[i];
        item->picture.pitch[0] = 16;
        item->picture.has_pts = fk.pend[pick].has_pts;
        item->picture.pts_us = fk.pend[pick].pts;
        item->picture.handle = (void *)(size_t)(i + 1);
        memmove(&fk.pend[pick], &fk.pend[pick + 1],
                (fk.npend - pick - 1) * sizeof fk.pend[0]);
        fk.npend--;
        return 1;
    }
    if (fk.eos && !fk.npend && !fk.ended_told) {
        fk.ended_told = 1;
        item->kind = MR_HWDEC_ENDED;
        return 1;
    }
    return 0;
}

static mr_status fk_send(void *be, void *packet, uint32_t len, unsigned flags,
                         int has_pts, uint64_t pts_us)
{
    unsigned i = (unsigned)(size_t)packet - 1;
    (void)be;
    if (i >= FK_PKTS || !fk.pkt_lent[i] || len > fk.capacity) {
        fprintf(stderr, "send of a buffer the player does not have\n");
        fail = 1;
        return MR_ERR;
    }
    fk.pkt_lent[i] = 0;
    fk.pkt_back[i] = 1;
    if (fk.nflags < 256) fk.flags_log[fk.nflags++] = flags;
    if (fk.fail_after && ++fk.sends >= fk.fail_after) fk.failed = 1;
    if (flags & MR_HWDEC_EOS) {
        fk.eos = 1;
        return MR_OK;
    }
    if (flags & MR_HWDEC_START) fk.au_len = 0;
    memcpy(fk.au + fk.au_len, fk.pkt_buf[i], len);
    fk.au_len += len;
    fk.au_has_pts = has_pts;
    fk.au_pts = pts_us;
    if (flags & MR_HWDEC_END) {
        if (fk.nlog < FK_LOG) {
            memcpy(fk.log[fk.nlog], fk.au, fk.au_len);
            fk.log_len[fk.nlog] = fk.au_len;
        }
        fk.nlog++;
        fk.pend[fk.npend].pts = fk.au_pts;
        fk.pend[fk.npend].has_pts = fk.au_has_pts;
        fk.npend++;
    }
    return MR_OK;
}

static void fk_release(void *be, void *picture)
{
    unsigned i = (unsigned)(size_t)picture - 1;
    (void)be;
    if (i >= fk.out_count || !fk.out_lent[i]) {
        fprintf(stderr, "release of a picture the player does not have\n");
        fail = 1;
        return;
    }
    fk.out_lent[i] = 0;
}

static mr_status fk_flush(void *be)
{
    unsigned i;
    (void)be;
    fk.flushes++;
    fk.npend = 0;
    fk.eos = fk.ended_told = 0;
    for (i = 0; i < FK_PKTS; i++)
        if (!fk.pkt_lent[i]) fk.pkt_back[i] = 1;
    return MR_OK;
}

static unsigned fk_wait(void *be, unsigned ms)
{ (void)be; fk.waited_ms += ms; return ms; }

static mr_status fk_set_output(void *be, mr_pixfmt fmt, int w, int h)
{
    (void)be; (void)fmt; (void)w; (void)h;
    fk.set_outputs++;
    return outs_lent() ? MR_ERR : MR_OK;     /* nothing may still be out */
}

static int closed;
static void fk_close(void *be) { (void)be; closed++; }

static const mr_hwdec_ops fk_ops = {
    fk_poll, fk_send, fk_release, fk_flush, fk_wait, fk_set_output, fk_close
};

static int open_h264 = 1;
static mr_status fk_open(mr_decoder *d)
{
    return mr_hwdec_attach(d, &fk_ops, &fk, open_h264, fk.out_count);
}

static const mr_codec fk_codec = { "fake", {0}, fk_open, mr_hwdec_decode,
                                   mr_hwdec_close, mr_hwdec_flush };

static void reset_fake(uint32_t capacity, unsigned outs, unsigned latency)
{
    memset(&fk, 0, sizeof fk);
    fk.capacity = capacity;
    fk.out_count = outs;
    fk.latency = latency;
}

/* ---- the stream ------------------------------------------------------ */

static const uint8_t sps[] = { 0x67, 0x42, 0xc0, 0x1e, 0xd9, 0x00, 0xa0 };
static const uint8_t pps[] = { 0x68, 0xce, 0x3c, 0x80 };
static uint8_t avcc[64];
static uint32_t avcc_len;

static void make_avcc(void)
{
    uint32_t n = 0;
    avcc[n++] = 1; avcc[n++] = 0x42; avcc[n++] = 0xc0; avcc[n++] = 0x1e;
    avcc[n++] = 0xff; avcc[n++] = 0xe1;
    avcc[n++] = 0; avcc[n++] = sizeof sps; memcpy(avcc + n, sps, sizeof sps);
    n += sizeof sps;
    avcc[n++] = 1;
    avcc[n++] = 0; avcc[n++] = sizeof pps; memcpy(avcc + n, pps, sizeof pps);
    n += sizeof pps;
    avcc_len = n;
}

/* A length-prefixed sample of one slice of `size` bytes, tagged with i. */
static uint32_t make_sample(uint8_t *out, unsigned i, uint32_t size)
{
    uint32_t k;
    out[0] = (uint8_t)(size >> 24); out[1] = (uint8_t)(size >> 16);
    out[2] = (uint8_t)(size >> 8); out[3] = (uint8_t)size;
    out[4] = 0x41;
    for (k = 1; k < size; k++) out[4 + k] = (uint8_t)(i + k);
    return size + 4;
}

static int open_dec(mr_decoder *d)
{
    memset(d, 0x5A, sizeof *d);
    return mr_decoder_open_config(d, &fk_codec, 320, 240, avcc, avcc_len);
}

/* Feed n units with timestamps pts(i), draining after each; collect the
 * timestamps that come out. */
static unsigned run(mr_decoder *d, unsigned first, unsigned n,
                    uint64_t (*pts)(unsigned), uint64_t *got, unsigned cap,
                    int drain, unsigned *eagain)
{
    uint8_t sample[300];
    unsigned i, out = 0;
    for (i = first; i < first + n; i++) {
        uint32_t len = make_sample(sample, i, 40);
        mr_status st;
        mr_decoder_set_input(d, 1, pts(i), 0);
        st = mr_decoder_decode(d, sample, len);
        if (st == MR_EAGAIN && eagain) (*eagain)++;
        while (st == MR_OK) {
            CHECK(d->frame.data && d->frame.fmt == MR_PIX_BGR24 &&
                  d->frame.has_pts);
            if (out < cap) got[out] = d->frame.pts_us;
            out++;
            st = drain ? mr_decoder_drain(d) : MR_EAGAIN;
        }
        CHECK(st == MR_EAGAIN);
        CHECK(outs_lent() + 1 <= fk.out_count || fk.out_count == 1);
    }
    return out;
}

static unsigned flush_all(mr_decoder *d, uint64_t *got, unsigned cap)
{
    unsigned out = 0;
    while (mr_decoder_flush(d) == MR_OK) {
        if (out < cap) got[out] = d->frame.pts_us;
        out++;
    }
    return out;
}

static uint64_t pts_linear(unsigned i) { return (uint64_t)i * 40000; }
/* Decode order of an I P B B group: display 0 3 1 2. */
static uint64_t pts_bframes(unsigned i)
{
    static const unsigned order[4] = { 0, 3, 1, 2 };
    return ((uint64_t)(i / 4) * 4 + order[i % 4]) * 40000;
}
static uint64_t pts_after_seek(unsigned i) { return 1000000 + (uint64_t)i * 40000; }

int main(void)
{
    mr_decoder d;
    uint64_t got[512];
    unsigned n, eagain, i;
    uint8_t expect[400];

    make_avcc();

    /* Pipeline delay: MR_EAGAIN while it fills, then one picture per unit,
     * the rest at end of stream; every timestamp, in order. */
    reset_fake(1024, 4, 2);
    CHECK(open_dec(&d) == MR_OK && d.drain && d.reset && d.set_output);
    eagain = 0;
    n = run(&d, 0, 30, pts_linear, got, 512, 1, &eagain);
    CHECK(eagain == 2 && n == 28);
    n += flush_all(&d, got + n, 512 - n);
    CHECK(n == 30);
    for (i = 0; i < 30 && i < n; i++) CHECK(got[i] == pts_linear(i));
    /* The parameter sets go in front of the first unit only. */
    {
        uint32_t k = 0;
        memcpy(expect + k, "\0\0\0\1", 4); k += 4;
        memcpy(expect + k, sps, sizeof sps); k += sizeof sps;
        memcpy(expect + k, "\0\0\0\1", 4); k += 4;
        memcpy(expect + k, pps, sizeof pps); k += sizeof pps;
        memcpy(expect + k, "\0\0\0\1", 4); k += 4;
        make_sample(expect + k - 4, 0, 40);
        memcpy(expect + k - 4, "\0\0\0\1", 4);
        k += 40;
        CHECK(fk.log_len[0] == k && !memcmp(fk.log[0], expect, k));
        CHECK(fk.log_len[1] == 44 && !memcmp(fk.log[1], "\0\0\0\1\x41", 5));
    }
    mr_decoder_close(&d);
    CHECK(closed == 1 && outs_lent() == 0);

    /* B-frames: decode order in, display order out, each with its own
     * timestamp. */
    reset_fake(1024, 4, 2);
    fk.reorder = 1;
    CHECK(open_dec(&d) == MR_OK);
    n = run(&d, 0, 32, pts_bframes, got, 512, 1, NULL);
    n += flush_all(&d, got + n, 512 - n);
    CHECK(n == 32);
    for (i = 0; i < n && i < 32; i++) CHECK(got[i] == pts_linear(i));
    mr_decoder_close(&d);

    /* A unit bigger than one input buffer goes in parts, start and end
     * marked, and arrives whole. */
    reset_fake(64, 4, 0);
    CHECK(open_dec(&d) == MR_OK);
    {
        uint8_t big[300];
        uint32_t len = make_sample(big, 7, 250), k, out_len;
        mr_decoder_set_input(&d, 1, 0, 0);
        CHECK(mr_decoder_decode(&d, big, len) == MR_OK);
        CHECK(fk.nflags == 5 && fk.flags_log[0] == MR_HWDEC_START &&
              fk.flags_log[1] == 0 && fk.flags_log[4] == MR_HWDEC_END);
        k = 8 + sizeof sps + sizeof pps;
        CHECK(fk.log_len[0] == k + 4 + 250);
        mr_avcc_sample_to_annexb(big, len, 4, expect, sizeof expect, &out_len);
        CHECK(!memcmp(fk.log[0] + k, expect, out_len));
    }
    mr_decoder_close(&d);

    /* A player that never drains (the live catch-up): no deadlock, and the
     * backend always keeps an output to write into, even when it emits in
     * bursts. */
    reset_fake(1024, 4, 1);
    fk.burst = 3;
    CHECK(open_dec(&d) == MR_OK);
    n = run(&d, 0, 200, pts_linear, got, 512, 0, NULL);
    CHECK(n > 150 && !fk.failed);
    mr_decoder_close(&d);

    /* The safety net: a backend with one input buffer that emits in bursts
     * and takes no input while any output is out. Giving the held picture
     * back each call is not enough there; once it keeps the buffer, the
     * player gives back its oldest undrained picture. (Any backend that
     * takes input with one output out never gets this far.) */
    reset_fake(1024, 4, 0);
    fk.in_need_free = 4;
    fk.burst = 3;
    fk.npkts = 1;
    CHECK(open_dec(&d) == MR_OK);
    n = run(&d, 0, 100, pts_linear, got, 512, 0, NULL);
    CHECK(n > 50 && !fk.failed && fk.waited_ms < 2000 * 100);
    CHECK(fk.waited_ms >= 100);    /* it did stall, and recovered */
    printf("stalled backend: %u pictures from 100 units, %u ms waited\n", n,
           fk.waited_ms);
    mr_decoder_close(&d);

    /* Reset (seek): nothing from before it comes out after it, and the
     * parameter sets go in front again. */
    reset_fake(1024, 4, 3);
    CHECK(open_dec(&d) == MR_OK);
    run(&d, 0, 10, pts_linear, got, 512, 1, NULL);
    CHECK(mr_decoder_reset(&d) == MR_OK && fk.flushes == 1);
    n = run(&d, 0, 8, pts_after_seek, got, 512, 1, NULL);
    n += flush_all(&d, got + n, 512 - n);
    CHECK(n == 8);
    for (i = 0; i < n && i < 512; i++) CHECK(got[i] >= 1000000);
    CHECK(fk.log_len[10] > 44 && !memcmp(fk.log[10] + 4, sps, sizeof sps));
    CHECK(fk.log_len[11] == 44);
    mr_decoder_close(&d);

    /* An output change gives every picture back first, and a picture still
     * in the old format after it is given back too, not handed out (the
     * fake writes BGR24 whatever it is asked). */
    reset_fake(1024, 4, 0);
    CHECK(open_dec(&d) == MR_OK);
    run(&d, 0, 3, pts_linear, got, 512, 0, NULL);
    CHECK(mr_decoder_set_output(&d, MR_PIX_BGR24, 640, 360) == MR_OK);
    CHECK(fk.set_outputs == 1 && outs_lent() == 0 && !d.frame.data);
    CHECK(run(&d, 3, 3, pts_linear, got, 512, 1, NULL) == 3);
    CHECK(mr_decoder_set_output(&d, MR_PIX_YUV420P, 0, 0) == MR_OK);
    CHECK(run(&d, 6, 5, pts_linear, got, 512, 1, NULL) == 0 &&
          outs_lent() == 0);
    mr_decoder_close(&d);

    /* The hardware gives up: an error now, at once later, and no reset. */
    reset_fake(1024, 4, 1);
    fk.fail_after = 5;
    CHECK(open_dec(&d) == MR_OK);
    {
        uint8_t sample[64];
        uint32_t len = make_sample(sample, 0, 40);
        mr_status st = MR_OK;
        int sends;
        for (i = 0; i < 10 && st != MR_ERR; i++) {
            mr_decoder_set_input(&d, 1, i, 0);
            st = mr_decoder_decode(&d, sample, len);
        }
        CHECK(st == MR_ERR && i == 5);
        sends = fk.sends;
        CHECK(mr_decoder_decode(&d, sample, len) == MR_ERR &&
              fk.sends == sends);
        CHECK(mr_decoder_reset(&d) != MR_OK);
        CHECK(mr_decoder_flush(&d) == MR_EAGAIN);
    }
    mr_decoder_close(&d);

    /* Annex-B input (MPEG-TS) goes in as it is, after the parameter
     * sets. */
    reset_fake(1024, 4, 0);
    CHECK(open_dec(&d) == MR_OK);
    {
        static const uint8_t unit[] = { 0, 0, 1, 0x65, 0x88, 0x80, 0x10 };
        uint32_t k = 8 + sizeof sps + sizeof pps;
        mr_decoder_set_input(&d, 1, 0, 1);
        CHECK(mr_decoder_decode(&d, unit, sizeof unit) == MR_OK);
        CHECK(fk.log_len[0] == k + sizeof unit &&
              !memcmp(fk.log[0] + k, unit, sizeof unit));
    }
    mr_decoder_close(&d);

    /* MJPEG: no parameter sets, the picture as it is. */
    reset_fake(1024, 4, 0);
    open_h264 = 0;
    CHECK(open_dec(&d) == MR_OK);
    {
        static const uint8_t jpeg[] = { 0xff, 0xd8, 0xff, 0xdb, 0, 0x43, 0xff,
                                        0xd9 };
        mr_decoder_set_input(&d, 0, 0, 0);
        CHECK(mr_decoder_decode(&d, jpeg, sizeof jpeg) == MR_OK);
        CHECK(fk.log_len[0] == sizeof jpeg &&
              !memcmp(fk.log[0], jpeg, sizeof jpeg) && !d.frame.has_pts);
    }
    mr_decoder_close(&d);
    open_h264 = 1;

    /* A malformed avcC is refused at open, and the backend closed. */
    reset_fake(1024, 4, 0);
    closed = 0;
    {
        mr_decoder bad;
        avcc[0] = 2;
        CHECK(mr_decoder_open_config(&bad, &fk_codec, 320, 240, avcc,
                                     avcc_len) == MR_EFORMAT && closed == 1);
        avcc[0] = 1;
    }

    CHECK(mr_hwdec_row_pixels(854, 32) == 864);
    CHECK(mr_hwdec_row_pixels(1920, 32) == 1920);
    CHECK(mr_hwdec_row_pixels(1366, 64) == 1408);

    puts(fail ? "hardware decoder check failed"
              : "hardware decoder: delay, order, splitting, stalls, reset, "
                "failure ok");
    return fail;
}
