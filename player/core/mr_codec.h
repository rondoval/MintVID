/*
 * MintVID - video decoder plugin interface
 *
 * The whole point of this project is a *codec-agnostic* player: one demux +
 * sync + render skeleton, with decoders that plug in behind this vtable. That
 * is what lets the same player scale from Cinepak on a bare A600/AGA up to
 * heavier codecs on a fast PiStorm/RTG machine - you add a decoder, not a
 * player.
 */
#ifndef MR_CODEC_H
#define MR_CODEC_H

#include "mr_types.h"

typedef struct mr_decoder mr_decoder;

typedef struct mr_codec {
    const char *name;
    /* AVI fourcc(s) this decoder claims (BITMAPINFOHEADER biCompression).
     * Unused slots are 0. Codecs like MPEG-4 have many aliases. */
    uint32_t    fourcc[24];
    /* Allocate decoder state for a w*h stream. */
    mr_status (*open)(mr_decoder *dec);
    /* Decode one compressed frame into dec->frame. May reuse/patch the
     * previous frame buffer (inter frames), so the buffer persists across
     * calls until close(). */
    mr_status (*decode)(mr_decoder *dec, const uint8_t *data, uint32_t len);
    void      (*close)(mr_decoder *dec);
    /* Optional (may be NULL): emit one buffered/reordered frame at end of
     * stream. Returns MR_OK while frames remain, MR_EAGAIN when drained.
     * Used by codecs with display reordering (MPEG-4 B-VOPs). */
    mr_status (*flush)(mr_decoder *dec);
} mr_codec;

struct mr_decoder {
    const mr_codec *codec;
    int             width;
    int             height;
    const uint8_t  *config;  /* borrowed container decoder setup (e.g. avcC) */
    uint32_t        config_len;
    mr_frame        frame;   /* filled in by decode()                       */
    void           *priv;    /* decoder-private state                       */
    /* Optional codec-installed callback for extra frames made ready by the
     * last decode(), without signalling end of stream. */
    mr_status     (*drain)(mr_decoder *dec);
    /* Optional codec-installed hooks, like drain. set_output: emit frames as
     * fmt at w x h (0 x 0: the stream's own size) from now on, or refuse with
     * MR_EUNSUPPORTED and leave the output as it was - called through
     * mr_decoder_set_output(). reset: forget the pictures in flight and the
     * reference frames but keep the decoder (seek, loop, reconnect); without
     * it mr_decoder_reset() closes and reopens. */
    mr_status     (*set_output)(mr_decoder *dec, mr_pixfmt fmt, int w, int h);
    mr_status     (*reset)(mr_decoder *dec);
    /* The next packet's container timestamp, and whether it is an Annex-B
     * byte stream rather than length-prefixed (MPEG-TS H.264): see
     * mr_decoder_set_input(). */
    int             in_has_pts;
    uint64_t        in_pts_us;
    int             in_annexb;
    /* The last output request the codec accepted, re-applied after a
     * close-and-reopen reset. */
    int             out_set;
    mr_pixfmt       out_fmt;
    int             out_w;
    int             out_h;
};

/* Registry: decoders self-select by fourcc. Decoders that run on other
 * hardware come first; when one's open() refuses the stream,
 * mr_codec_find_next() gives the next decoder for the same fourcc (NULL when
 * none is left). */
const mr_codec *mr_codec_find(uint32_t fourcc);
const mr_codec *mr_codec_find_next(uint32_t fourcc, const mr_codec *after);
/* 0 hides the hardware decoders from the two calls above (--no-videocore). */
void            mr_codec_set_hw_enabled(int enabled);
/* 1 when codec is one of the hardware decoders. */
int             mr_codec_is_hw(const mr_codec *codec);

mr_status mr_decoder_open(mr_decoder *dec, const mr_codec *codec,
                          int width, int height);
mr_status mr_decoder_open_config(mr_decoder *dec, const mr_codec *codec,
                                 int width, int height,
                                 const uint8_t *config, uint32_t config_len);
mr_status mr_decoder_decode(mr_decoder *dec, const uint8_t *data, uint32_t len);
/* Drain one additional frame made ready by the last decode() call. */
mr_status mr_decoder_drain(mr_decoder *dec);
/* Drain one reordered frame at end of stream (MR_EAGAIN when none left). */
mr_status mr_decoder_flush(mr_decoder *dec);
/* Re-open the codec with the same stream setup, discarding reference frames
 * (through the codec's reset() when it has one), and re-apply the output
 * request. */
mr_status mr_decoder_reset(mr_decoder *dec);
/* Describe the packet the next decode() gets: its container timestamp and
 * whether it is Annex-B. */
void      mr_decoder_set_input(mr_decoder *dec, int has_pts, uint64_t pts_us,
                               int annexb);
/* Ask for frames as fmt at w x h (0 x 0: the stream's size): MR_OK when the
 * codec will, MR_EUNSUPPORTED (the default) when it only emits its own. */
mr_status mr_decoder_set_output(mr_decoder *dec, mr_pixfmt fmt, int w, int h);
void      mr_decoder_close(mr_decoder *dec);

/* Individual codec descriptors (defined in their .c files). */
extern const mr_codec mr_codec_cinepak;
extern const mr_codec mr_codec_mjpeg;
extern const mr_codec mr_codec_mpeg2;
extern const mr_codec mr_codec_mpeg4;
extern const mr_codec mr_codec_msmpeg4v2;
extern const mr_codec mr_codec_wmv1;
extern const mr_codec mr_codec_wmv2;
extern const mr_codec mr_codec_h263;
extern const mr_codec mr_codec_h263;
extern const mr_codec mr_codec_msvideo1;
extern const mr_codec mr_codec_rle;
extern const mr_codec mr_codec_rawvideo;
extern const mr_codec mr_codec_dv;
#ifdef MR_HAVE_H264
extern const mr_codec mr_codec_h264;
#endif
#ifdef MR_HAVE_VIDEOCORE
extern const mr_codec mr_codec_vc_h264;
extern const mr_codec mr_codec_vc_mjpeg;
#endif

#endif /* MR_CODEC_H */
