/*
 * MintVID - a decoder that runs on other hardware (mr_hwdec.h).
 */
#include "mr_hwdec.h"
#include "mr_avc.h"

#include <stdlib.h>
#include <string.h>

#define MAX_PACKETS     16
#define MAX_READY       16
#define WAIT_STEP_MS    10
#define STUCK_MS        100   /* no input buffer back: give a picture back  */
#define PACKET_WAIT_MS  2000  /* none after this: the hardware is gone      */
#define DRAIN_IDLE_MS   500   /* end of stream: nothing new for this long   */

typedef struct {
    const mr_hwdec_ops *ops;
    void               *be;
    int                 h264;
    unsigned            output_count;
    int                 nal_len_size;
    uint8_t            *config;          /* Annex-B SPS/PPS                */
    uint32_t            config_len;
    int                 config_pending;  /* goes in front of the next unit */
    mr_hwdec_packet     free_pkt[MAX_PACKETS];
    unsigned            free_count;
    mr_hwdec_picture    ready[MAX_READY];
    unsigned            ready_head, ready_count;
    mr_hwdec_picture    held;            /* the one in dec->frame          */
    int                 have_held;
    uint8_t            *scratch;         /* only for a unit split in parts */
    uint32_t            scratch_cap;
    int                 failed, ended, eos_sent;
} hwdec;

static void drop_oldest_ready(hwdec *h)
{
    if (!h->ready_count) return;
    h->ops->release(h->be, h->ready[h->ready_head].handle);
    h->ready_head = (h->ready_head + 1) % MAX_READY;
    h->ready_count--;
}

static void give_back_all(mr_decoder *dec, hwdec *h, int ready_too)
{
    if (h->have_held) {
        h->ops->release(h->be, h->held.handle);
        h->have_held = 0;
    }
    dec->frame.data = NULL;
    while (ready_too && h->ready_count) drop_oldest_ready(h);
}

/* Collect what the backend has: input buffers, pictures, the end mark, a
 * failure. Never holds more pictures than leave the backend one to write
 * into. */
static void pump(hwdec *h)
{
    mr_hwdec_item item;
    unsigned limit = h->output_count > 1 ? h->output_count - 1 : 1;
    while (h->ops->poll(h->be, &item)) {
        switch (item.kind) {
        case MR_HWDEC_PACKET:
            if (h->free_count < MAX_PACKETS)
                h->free_pkt[h->free_count++] = item.packet;
            break;
        case MR_HWDEC_PICTURE:
            if ((unsigned)h->have_held + h->ready_count >= limit ||
                h->ready_count == MAX_READY)
                drop_oldest_ready(h);
            if ((unsigned)h->have_held + h->ready_count >= limit ||
                h->ready_count == MAX_READY) {
                h->ops->release(h->be, item.picture.handle);
                break;
            }
            h->ready[(h->ready_head + h->ready_count) % MAX_READY] =
                item.picture;
            h->ready_count++;
            break;
        case MR_HWDEC_ENDED:
            h->ended = 1;
            break;
        case MR_HWDEC_FAILED:
            /* A backend that has failed may say so on every poll. */
            h->failed = 1;
            return;
        default:
            break;
        }
    }
}

/* An input buffer, waiting for one if need be. While the backend keeps them
 * all it may be waiting for somewhere to put a picture: give the oldest
 * undrained one back. */
static int get_packet(hwdec *h, mr_hwdec_packet *out)
{
    unsigned waited = 0;
    for (;;) {
        pump(h);
        if (h->failed) return 0;
        if (h->free_count) {
            *out = h->free_pkt[--h->free_count];
            return 1;
        }
        if (waited >= STUCK_MS) drop_oldest_ready(h);
        if (waited >= PACKET_WAIT_MS) {
            h->failed = 1;
            return 0;
        }
        h->ops->wait(h->be, WAIT_STEP_MS);
        waited += WAIT_STEP_MS;
    }
}

static void put_back_packet(hwdec *h, const mr_hwdec_packet *pkt)
{
    if (h->free_count < MAX_PACKETS) h->free_pkt[h->free_count++] = *pkt;
}

static mr_status send(hwdec *h, const mr_hwdec_packet *pkt, uint32_t len,
                      unsigned flags, int has_pts, uint64_t pts_us)
{
    mr_status st = h->ops->send(h->be, pkt->handle, len, flags, has_pts,
                                pts_us);
    if (st != MR_OK) h->failed = 1;
    return st;
}

/* One access unit (H.264) or picture (MJPEG) to the backend: Annex-B, with
 * the parameter sets in front when they are due, in one input buffer when it
 * fits and split across several when it does not. */
static mr_status send_unit(hwdec *h, const uint8_t *data, uint32_t len,
                           int annexb, int has_pts, uint64_t pts_us)
{
    int convert = h->h264 && !annexb;
    uint32_t pre = h->config_pending ? h->config_len : 0, body = len, total;
    uint32_t off;
    mr_hwdec_packet pkt;
    mr_status st;

    if (!get_packet(h, &pkt)) return MR_ERR;
    if (pre < pkt.capacity) {
        if (convert) {
            st = mr_avcc_sample_to_annexb(data, len, h->nal_len_size,
                                          pkt.data + pre, pkt.capacity - pre,
                                          &body);
            if (st == MR_EFORMAT) {
                put_back_packet(h, &pkt);
                return st;
            }
        }
        if (pre + body <= pkt.capacity) {
            if (pre) memcpy(pkt.data, h->config, pre);
            if (!convert) memcpy(pkt.data + pre, data, len);
            st = send(h, &pkt, pre + body, MR_HWDEC_START | MR_HWDEC_END,
                      has_pts, pts_us);
            if (st == MR_OK) h->config_pending = 0;
            return st;
        }
    } else if (convert &&
               mr_avcc_sample_to_annexb(data, len, h->nal_len_size, NULL, 0,
                                        &body) == MR_EFORMAT) {
        put_back_packet(h, &pkt);
        return MR_EFORMAT;
    }
    /* Larger than one buffer: build it whole, then send it in parts. */
    total = pre + body;
    if (total > h->scratch_cap) {
        uint8_t *grown = (uint8_t *)realloc(h->scratch, total);
        if (!grown) {
            put_back_packet(h, &pkt);
            return MR_ENOMEM;
        }
        h->scratch = grown;
        h->scratch_cap = total;
    }
    if (pre) memcpy(h->scratch, h->config, pre);
    if (convert)
        mr_avcc_sample_to_annexb(data, len, h->nal_len_size, h->scratch + pre,
                                 body, &body);
    else
        memcpy(h->scratch + pre, data, len);
    for (off = 0; off < total; ) {
        uint32_t n = total - off;
        unsigned flags = off ? 0 : MR_HWDEC_START;
        if (off && !get_packet(h, &pkt)) return MR_ERR;
        if (n > pkt.capacity) n = pkt.capacity;
        if (off + n == total) flags |= MR_HWDEC_END;
        memcpy(pkt.data, h->scratch + off, n);
        st = send(h, &pkt, n, flags, has_pts, pts_us);
        if (st != MR_OK) return st;
        off += n;
    }
    h->config_pending = 0;
    return MR_OK;
}

/* The oldest ready picture into dec->frame, valid until the next call. */
static mr_status hand_out(mr_decoder *dec, hwdec *h)
{
    mr_hwdec_picture *p;
    if (!h->ready_count) return MR_EAGAIN;
    h->held = h->ready[h->ready_head];
    h->ready_head = (h->ready_head + 1) % MAX_READY;
    h->ready_count--;
    h->have_held = 1;
    p = &h->held;
    dec->frame.width = p->width;
    dec->frame.height = p->height;
    dec->frame.fmt = p->fmt;
    dec->frame.data = p->plane[0];
    dec->frame.stride = p->pitch[0];
    dec->frame.u_data = p->plane[1];
    dec->frame.v_data = p->plane[2];
    dec->frame.u_stride = p->pitch[1];
    dec->frame.v_stride = p->pitch[2];
    dec->frame.dirty_y0 = 0;
    dec->frame.dirty_y1 = p->height;
    dec->frame.has_pts = p->has_pts;
    dec->frame.pts_us = p->pts_us;
    return MR_OK;
}

mr_status mr_hwdec_decode(mr_decoder *dec, const uint8_t *data, uint32_t len)
{
    hwdec *h = (hwdec *)dec->priv;
    mr_status st;
    if (!h || h->failed) return MR_ERR;
    give_back_all(dec, h, 0);
    if (len) {
        st = send_unit(h, data, len, dec->in_annexb, dec->in_has_pts,
                       dec->in_pts_us);
        if (st != MR_OK) return st;
    }
    pump(h);
    if (h->failed) return MR_ERR;
    return hand_out(dec, h);
}

static mr_status hwdec_drain(mr_decoder *dec)
{
    hwdec *h = (hwdec *)dec->priv;
    if (!h || h->failed) return MR_ERR;
    give_back_all(dec, h, 0);
    pump(h);
    if (h->failed) return MR_ERR;
    return hand_out(dec, h);
}

mr_status mr_hwdec_flush(mr_decoder *dec)
{
    hwdec *h = (hwdec *)dec->priv;
    unsigned idle = 0;
    mr_hwdec_packet pkt;
    if (!h) return MR_EAGAIN;
    give_back_all(dec, h, 0);
    if (h->failed) return MR_EAGAIN;
    if (!h->eos_sent) {
        if (!get_packet(h, &pkt) ||
            send(h, &pkt, 0, MR_HWDEC_EOS, 0, 0) != MR_OK)
            return MR_EAGAIN;
        h->eos_sent = 1;
    }
    for (;;) {
        pump(h);
        if (h->ready_count) return hand_out(dec, h);
        if (h->ended || h->failed || idle >= DRAIN_IDLE_MS) return MR_EAGAIN;
        h->ops->wait(h->be, WAIT_STEP_MS);
        idle += WAIT_STEP_MS;
    }
}

static mr_status hwdec_reset(mr_decoder *dec)
{
    hwdec *h = (hwdec *)dec->priv;
    mr_status st;
    if (!h) return MR_ERR;
    give_back_all(dec, h, 1);
    if (h->failed) return MR_ERR;
    st = h->ops->flush(h->be);
    if (st != MR_OK) {
        h->failed = 1;
        return st;
    }
    pump(h);
    while (h->ready_count) drop_oldest_ready(h);
    h->config_pending = h->config_len != 0;
    h->ended = h->eos_sent = 0;
    return MR_OK;
}

static mr_status hwdec_set_output(mr_decoder *dec, mr_pixfmt fmt, int w,
                                  int ht)
{
    hwdec *h = (hwdec *)dec->priv;
    if (!h || h->failed) return MR_ERR;
    /* Pictures already out are in the old geometry's buffers. */
    give_back_all(dec, h, 1);
    return h->ops->set_output(h->be, fmt, w, ht);
}

mr_status mr_hwdec_attach(mr_decoder *dec, const mr_hwdec_ops *ops,
                          void *backend, int h264, unsigned output_count)
{
    hwdec *h = (hwdec *)calloc(1, sizeof *h);
    mr_status st = MR_ENOMEM;
    if (!h) goto fail;
    h->ops = ops;
    h->be = backend;
    h->h264 = h264;
    h->output_count = output_count ? output_count : 1;
    if (h264) {
        uint32_t need = 0;
        st = mr_avcc_config_to_annexb(dec->config, dec->config_len, NULL, 0,
                                      &need, &h->nal_len_size);
        if (st != MR_OK && st != MR_ENOMEM) goto fail;
        if (need) {
            st = MR_ENOMEM;
            h->config = (uint8_t *)malloc(need);
            if (!h->config) goto fail;
            mr_avcc_config_to_annexb(dec->config, dec->config_len, h->config,
                                     need, &h->config_len, &h->nal_len_size);
            h->config_pending = 1;
        }
    }
    dec->priv = h;
    dec->drain = hwdec_drain;
    dec->reset = hwdec_reset;
    dec->set_output = hwdec_set_output;
    return MR_OK;
fail:
    free(h);
    ops->close(backend);
    return st;
}

void mr_hwdec_close(mr_decoder *dec)
{
    hwdec *h = (hwdec *)dec->priv;
    if (!h) return;
    give_back_all(dec, h, 1);
    h->ops->close(h->be);
    free(h->config);
    free(h->scratch);
    free(h);
    dec->priv = NULL;
}
