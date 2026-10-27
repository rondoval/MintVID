/*
 * Registry order, hardware switch and the generic decoder hooks
 * (mr_codec.h): the hardware decoders come first, mr_codec_find_next()
 * reaches the software one for the same fourcc, mr_codec_set_hw_enabled(0)
 * hides them, a failed open falls through, every decoder field starts
 * defined, the packet's timestamp reaches the frame unless the codec knows
 * better, and an accepted output request survives both kinds of reset.
 * Built with MR_HAVE_VIDEOCORE against fake descriptors: the real hardware
 * codec is Amiga-only.
 */
#include "../core/mr_codec.h"
#include <stdio.h>
#include <string.h>

#define STUB(n) const mr_codec mr_codec_##n={#n,{0},0,0,0,0}
STUB(cinepak); STUB(mpeg2); STUB(mpeg4); STUB(msmpeg4v2); STUB(wmv1);
STUB(wmv2); STUB(h263); STUB(msvideo1); STUB(rle); STUB(rawvideo); STUB(dv);

static int hw_open_fails, opens, set_outputs, resets;
static mr_pixfmt last_fmt;
static int last_w, last_h;
static uint8_t pixel[4];

static mr_status hw_set_output(mr_decoder *d, mr_pixfmt f, int w, int h);
static mr_status hw_reset(mr_decoder *d);
static mr_status fake_open(mr_decoder *d) { (void)d; opens++; return MR_OK; }
/* Installs set_output; and reset, when it is the MJPEG one. */
static mr_status hw_open(mr_decoder *d)
{ opens++; if (hw_open_fails) return MR_EUNSUPPORTED;
  d->set_output = hw_set_output;
  if (d->codec == &mr_codec_vc_mjpeg) d->reset = hw_reset;
  return MR_OK; }
static void fake_close(mr_decoder *d) { (void)d; }
static mr_status sw_decode(mr_decoder *d, const uint8_t *p, uint32_t n)
{ (void)p; (void)n; d->frame.data = pixel; return MR_OK; }
/* A pipelined decoder: what comes out belongs to an earlier packet. */
static mr_status hw_decode(mr_decoder *d, const uint8_t *p, uint32_t n)
{ (void)p; (void)n; d->frame.data = pixel; d->frame.has_pts = 1;
  d->frame.pts_us = 999; return MR_OK; }
static mr_status hw_set_output(mr_decoder *d, mr_pixfmt f, int w, int h)
{ (void)d; set_outputs++; if (f == MR_PIX_INDEX8) return MR_EUNSUPPORTED;
  last_fmt = f; last_w = w; last_h = h; return MR_OK; }
static mr_status hw_reset(mr_decoder *d) { (void)d; resets++; return MR_OK; }

const mr_codec mr_codec_vc_h264 = {"vc-h264", {MR_FOURCC('a','v','c','1')},
    hw_open, hw_decode, fake_close, 0};
const mr_codec mr_codec_vc_mjpeg = {"vc-mjpeg", {MR_FOURCC('M','J','P','G')},
    hw_open, hw_decode, fake_close, 0};
const mr_codec mr_codec_h264 = {"h264", {MR_FOURCC('a','v','c','1')},
    fake_open, sw_decode, fake_close, 0};
const mr_codec mr_codec_mjpeg = {"mjpeg", {MR_FOURCC('M','J','P','G')},
    fake_open, sw_decode, fake_close, 0};

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", \
    __FILE__, __LINE__, #c); fail = 1; } } while (0)

int main(void)
{
    const uint32_t avc1 = MR_FOURCC('a','v','c','1');
    const uint32_t mjpg = MR_FOURCC('m','j','p','g');
    const mr_codec *c;
    mr_decoder d;
    int fail = 0;

    CHECK(mr_codec_find(avc1) == &mr_codec_vc_h264);
    CHECK(mr_codec_find_next(avc1, &mr_codec_vc_h264) == &mr_codec_h264);
    CHECK(mr_codec_find_next(avc1, &mr_codec_h264) == NULL);
    CHECK(mr_codec_find(mjpg) == &mr_codec_vc_mjpeg);
    CHECK(mr_codec_find_next(mjpg, &mr_codec_vc_mjpeg) == &mr_codec_mjpeg);
    mr_codec_set_hw_enabled(0);
    CHECK(mr_codec_find(avc1) == &mr_codec_h264);
    CHECK(mr_codec_find(mjpg) == &mr_codec_mjpeg);
    mr_codec_set_hw_enabled(1);

    /* The player's loop: the first decoder that opens wins. */
    hw_open_fails = 1;
    for (c = mr_codec_find(avc1); c; c = mr_codec_find_next(avc1, c))
        if (mr_decoder_open_config(&d, c, 320, 240, NULL, 0) == MR_OK) break;
    CHECK(c == &mr_codec_h264 && d.codec == &mr_codec_h264);
    hw_open_fails = 0;

    /* Stack garbage must not leak into the new fields. */
    memset(&d, 0xA5, sizeof d);
    CHECK(mr_decoder_open(&d, &mr_codec_h264, 320, 240) == MR_OK);
    CHECK(!d.in_has_pts && !d.in_annexb && !d.out_set && !d.drain &&
          !d.set_output && !d.reset && !d.frame.has_pts && !d.frame.data);

    /* The packet's timestamp reaches the frame of a codec that does not
     * track its own... */
    mr_decoder_set_input(&d, 1, 1234, 1);
    CHECK(d.in_annexb);
    CHECK(mr_decoder_decode(&d, pixel, 1) == MR_OK);
    CHECK(d.frame.has_pts && d.frame.pts_us == 1234);
    mr_decoder_set_input(&d, 0, 0, 0);
    CHECK(mr_decoder_decode(&d, pixel, 1) == MR_OK && !d.frame.has_pts);
    /* ...and no output request is taken by one without set_output(). */
    CHECK(mr_decoder_set_output(&d, MR_PIX_BGR24, 640, 360) == MR_EUNSUPPORTED);
    CHECK(!d.out_set);
    mr_decoder_close(&d);

    /* A pipelined codec's own timestamp wins. */
    CHECK(mr_decoder_open(&d, &mr_codec_vc_h264, 320, 240) == MR_OK);
    mr_decoder_set_input(&d, 1, 1234, 0);
    CHECK(mr_decoder_decode(&d, pixel, 1) == MR_OK && d.frame.pts_us == 999);

    /* An accepted request is kept, a refused one changes nothing, and a
     * close-and-reopen reset applies the kept one again. */
    CHECK(mr_decoder_set_output(&d, MR_PIX_BGR24, 640, 360) == MR_OK);
    CHECK(mr_decoder_set_output(&d, MR_PIX_INDEX8, 0, 0) == MR_EUNSUPPORTED);
    CHECK(d.out_set && d.out_fmt == MR_PIX_BGR24 && d.out_w == 640);
    opens = set_outputs = 0;
    last_w = 0;
    CHECK(mr_decoder_reset(&d) == MR_OK);
    CHECK(opens == 1 && set_outputs == 1 && last_fmt == MR_PIX_BGR24 &&
          last_w == 640 && last_h == 360 && d.out_set);
    mr_decoder_close(&d);

    /* A codec with its own reset() keeps its session: no reopen. */
    CHECK(mr_decoder_open(&d, &mr_codec_vc_mjpeg, 320, 240) == MR_OK);
    CHECK(mr_decoder_set_output(&d, MR_PIX_YUV420P, 0, 0) == MR_OK);
    opens = resets = 0;
    CHECK(mr_decoder_reset(&d) == MR_OK);
    CHECK(resets == 1 && opens == 0 && d.out_set &&
          d.out_fmt == MR_PIX_YUV420P);
    mr_decoder_close(&d);

    puts(fail ? "codec fallback check failed"
              : "codec fallback, hardware switch and decoder hooks: ok");
    return fail;
}
