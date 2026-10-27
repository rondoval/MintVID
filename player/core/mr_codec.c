/*
 * MintVID - decoder registry and lifecycle glue.
 */
#include "mr_codec.h"

#include <string.h>

/* Decoders that run on other hardware: searched first, unless
 * mr_codec_set_hw_enabled(0). Their open() refuses what the hardware cannot
 * take, and the software decoder for the same fourcc follows. */
static const mr_codec *const g_hw_codecs[] = {
#ifdef MR_HAVE_VIDEOCORE
    &mr_codec_vc_h264,
    &mr_codec_vc_mjpeg,
#endif
    NULL
};

static const mr_codec *const g_codecs[] = {
    &mr_codec_cinepak,
    &mr_codec_mjpeg,
    &mr_codec_mpeg2,
    &mr_codec_mpeg4,
    &mr_codec_msmpeg4v2,
    &mr_codec_wmv1,
    &mr_codec_wmv2,
    &mr_codec_h263,
    &mr_codec_msvideo1,
    &mr_codec_rle,
    &mr_codec_rawvideo,
    &mr_codec_dv,
#ifdef MR_HAVE_H264
    &mr_codec_h264,
#endif
};

/* Fold the ASCII letters of a fourcc to lower case. Muxers stamp the same
 * codec tag in whatever case they feel like ('MRLE' vs 'mrle', 'DIVX' vs
 * 'divx'), and AVI's fccHandler is a particularly bad offender, so matching
 * case-insensitively beats making every decoder enumerate the variants - the
 * one nobody thinks of is the one that reports "no decoder for this fourcc"
 * on a file that is perfectly supported. Bytes outside A-Z are untouched, so
 * numeric BI_* tags (BI_RLE8 is 1) still compare exactly. */
static uint32_t fourcc_fold(uint32_t f)
{
    uint32_t out = 0;
    int i;
    for (i = 0; i < 4; i++) {
        uint32_t b = (f >> (i * 8)) & 0xffu;
        if (b >= 'A' && b <= 'Z') b += (uint32_t)('a' - 'A');
        out |= b << (i * 8);
    }
    return out;
}

static int g_hw_enabled = 1;

void mr_codec_set_hw_enabled(int enabled)
{
    g_hw_enabled = enabled != 0;
}

static int codec_claims(const mr_codec *c, uint32_t want)
{
    size_t j;
    for (j = 0; j < sizeof(c->fourcc) / sizeof(c->fourcc[0]); j++)
        if (c->fourcc[j] && fourcc_fold(c->fourcc[j]) == want)
            return 1;
    return 0;
}

/* The search order: the hardware decoders while enabled, then the rest. */
static const mr_codec *codec_at(size_t i)
{
    size_t hw = sizeof(g_hw_codecs) / sizeof(g_hw_codecs[0]) - 1;
    if (!g_hw_enabled) hw = 0;
    if (i < hw) return g_hw_codecs[i];
    i -= hw;
    return i < sizeof(g_codecs) / sizeof(g_codecs[0]) ? g_codecs[i] : NULL;
}

const mr_codec *mr_codec_find_next(uint32_t fourcc, const mr_codec *after)
{
    const mr_codec *c;
    size_t i = 0;
    uint32_t want = fourcc_fold(fourcc);
    if (after) {
        while ((c = codec_at(i)) && c != after) i++;
        if (!c) return NULL;
        i++;
    }
    for (; (c = codec_at(i)); i++)
        if (codec_claims(c, want))
            return c;
    return NULL;
}

const mr_codec *mr_codec_find(uint32_t fourcc)
{
    return mr_codec_find_next(fourcc, NULL);
}

mr_status mr_decoder_open(mr_decoder *dec, const mr_codec *codec,
                          int width, int height)
{
    return mr_decoder_open_config(dec, codec, width, height, NULL, 0);
}

mr_status mr_decoder_open_config(mr_decoder *dec, const mr_codec *codec,
                                 int width, int height,
                                 const uint8_t *config, uint32_t config_len)
{
    if (!dec || !codec || width <= 0 || height <= 0)
        return MR_ERR;
    dec->codec  = codec;
    dec->width  = width;
    dec->height = height;
    dec->config = config;
    dec->config_len = config_len;
    dec->priv   = NULL;
    dec->drain  = NULL;
    dec->set_output = NULL;
    dec->reset  = NULL;
    /* The player's decoder lives on its stack: every field a codec or this
     * file reads starts defined. */
    memset(&dec->frame, 0, sizeof dec->frame);
    dec->in_has_pts = 0;
    dec->in_pts_us = 0;
    dec->in_annexb = 0;
    dec->out_set = 0;
    dec->out_fmt = MR_PIX_RGB24;
    dec->out_w = dec->out_h = 0;
    return codec->open(dec);
}

mr_status mr_decoder_decode(mr_decoder *dec, const uint8_t *data, uint32_t len)
{
    if (!dec || !dec->codec)
        return MR_ERR;
    dec->frame.has_pts = dec->in_has_pts;
    dec->frame.pts_us = dec->in_pts_us;
    return dec->codec->decode(dec, data, len);
}

mr_status mr_decoder_drain(mr_decoder *dec)
{
    if (!dec || !dec->codec || !dec->drain)
        return MR_EAGAIN;
    return dec->drain(dec);
}

mr_status mr_decoder_flush(mr_decoder *dec)
{
    if (!dec || !dec->codec || !dec->codec->flush)
        return MR_EAGAIN;
    return dec->codec->flush(dec);
}

mr_status mr_decoder_reset(mr_decoder *dec)
{
    const mr_codec *codec;
    int width, height, out_set, out_w, out_h;
    mr_pixfmt out_fmt;
    const uint8_t *config;
    uint32_t config_len;
    mr_status st;
    if (!dec || !dec->codec) return MR_ERR;
    if (dec->reset) return dec->reset(dec);
    codec = dec->codec;
    width = dec->width; height = dec->height;
    config = dec->config; config_len = dec->config_len;
    out_set = dec->out_set; out_fmt = dec->out_fmt;
    out_w = dec->out_w; out_h = dec->out_h;
    if (codec->close) codec->close(dec);
    st = mr_decoder_open_config(dec, codec, width, height, config, config_len);
    if (st == MR_OK && out_set)
        st = mr_decoder_set_output(dec, out_fmt, out_w, out_h);
    return st;
}

void mr_decoder_set_input(mr_decoder *dec, int has_pts, uint64_t pts_us,
                          int annexb)
{
    if (!dec) return;
    dec->in_has_pts = has_pts != 0;
    dec->in_pts_us = pts_us;
    dec->in_annexb = annexb != 0;
}

mr_status mr_decoder_set_output(mr_decoder *dec, mr_pixfmt fmt, int w, int h)
{
    mr_status st;
    if (!dec || !dec->codec || !dec->set_output || w < 0 || h < 0)
        return MR_EUNSUPPORTED;
    st = dec->set_output(dec, fmt, w, h);
    if (st == MR_OK) {
        dec->out_set = 1;
        dec->out_fmt = fmt;
        dec->out_w = w;
        dec->out_h = h;
    }
    return st;
}

void mr_decoder_close(mr_decoder *dec)
{
    if (dec && dec->codec && dec->codec->close)
        dec->codec->close(dec);
}
