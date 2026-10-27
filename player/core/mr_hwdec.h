/*
 * MintVID - a decoder that runs on other hardware, behind mr_codec.h.
 *
 * Hardware decoders work asynchronously: packets go in, the hardware hands
 * the buffers back as it takes them, and pictures come out later, from
 * buffers the hardware owns. This file keeps that state and presents it as
 * an ordinary mr_codec: decode() sends one access unit and returns a picture
 * when one is ready (MR_EAGAIN until the pipeline fills, as with any codec's
 * reordering delay), drain() hands over further ready pictures, flush() sends
 * the end mark and returns what is left, and the reset/set_output hooks map
 * to the backend's flush and output geometry.
 *
 * Rules it keeps, whatever the player does:
 *  - the picture handed out in dec->frame stays valid until the next call;
 *  - never more pictures held than the backend can spare: while it waits for
 *    an input buffer it gives the oldest undrained picture back, so the
 *    hardware always has somewhere to write;
 *  - H.264 goes in as Annex-B, with the stream's SPS/PPS in front of the
 *    first access unit after attach and after every reset (the MOV demuxer
 *    strips in-band parameter sets); an access unit larger than one input
 *    buffer is split, marked start and end.
 *
 * A backend implements mr_hwdec_ops; amiga/mr_vc.c is the VideoCore one, and
 * tests/mr_hwdec_check.c drives this file with a fake.
 */
#ifndef MR_HWDEC_H
#define MR_HWDEC_H

#include "mr_codec.h"

/* send() flags. */
#define MR_HWDEC_START 1u      /* first buffer of an access unit             */
#define MR_HWDEC_END   2u      /* last buffer of an access unit              */
#define MR_HWDEC_EOS   4u      /* end of stream; the buffer is empty         */

/* An input buffer the backend lends out. */
typedef struct {
    uint8_t  *data;
    uint32_t  capacity;
    void     *handle;
} mr_hwdec_packet;

/* A decoded picture in a buffer the backend owns: the visible picture,
 * planes, pitches in bytes. */
typedef struct {
    mr_pixfmt fmt;
    int       width, height;
    uint8_t  *plane[3];
    int       pitch[3];
    int       has_pts;
    uint64_t  pts_us;
    void     *handle;
} mr_hwdec_picture;

enum {
    MR_HWDEC_NONE = 0,
    MR_HWDEC_PACKET,           /* an input buffer to fill                    */
    MR_HWDEC_PICTURE,          /* a decoded picture to release when done     */
    MR_HWDEC_ENDED,            /* everything before the end mark is out      */
    MR_HWDEC_FAILED            /* the hardware gave up: only close is left   */
};

typedef struct {
    int              kind;
    mr_hwdec_packet  packet;
    mr_hwdec_picture picture;
} mr_hwdec_item;

typedef struct mr_hwdec_ops {
    /* Fill *item with the next thing the backend has; 0 when it has none. */
    int       (*poll)(void *backend, mr_hwdec_item *item);
    /* Send a filled input buffer (len bytes, MR_HWDEC_* flags). */
    mr_status (*send)(void *backend, void *packet, uint32_t len,
                      unsigned flags, int has_pts, uint64_t pts_us);
    void      (*release)(void *backend, void *picture);
    /* Drop everything in flight: input buffers come back through poll(),
     * no picture from before it comes out after it. */
    mr_status (*flush)(void *backend);
    /* Sleep until the backend has something for poll(), or ms pass; how
     * long it slept (the backend's signal can end it early). */
    unsigned  (*wait)(void *backend, unsigned ms);
    mr_status (*set_output)(void *backend, mr_pixfmt fmt, int w, int h);
    void      (*close)(void *backend);
} mr_hwdec_ops;

/* Take over a decoder whose backend the codec's open() has just started:
 * output_count is how many pictures the backend can have out at once. For
 * H.264, dec->config (avcC) supplies the parameter sets. On failure the
 * backend is closed. */
mr_status mr_hwdec_attach(mr_decoder *dec, const mr_hwdec_ops *ops,
                          void *backend, int h264, unsigned output_count);

/* The mr_codec entry points of an attached decoder. */
mr_status mr_hwdec_decode(mr_decoder *dec, const uint8_t *data, uint32_t len);
mr_status mr_hwdec_flush(mr_decoder *dec);
void      mr_hwdec_close(mr_decoder *dec);

/* Row width in pixels of a backend whose rows come in whole groups of align
 * pixels. */
static inline int mr_hwdec_row_pixels(int width, int align)
{
    return (width + align - 1) / align * align;
}

#endif /* MR_HWDEC_H */
