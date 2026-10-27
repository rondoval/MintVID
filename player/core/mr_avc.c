/*
 * MintVID - H.264 stream setup for decoders that take Annex-B (mr_avc.h).
 */
#include "mr_avc.h"

#include <string.h>

static const uint8_t start_code[4] = { 0, 0, 0, 1 };

mr_status mr_avcc_config_to_annexb(const uint8_t *avcc, uint32_t len,
                                   uint8_t *dst, uint32_t cap,
                                   uint32_t *out_len, int *nal_len_size)
{
    uint32_t pos = 6, out = 0;
    int pass;
    if (!avcc || len < 7 || avcc[0] != 1) return MR_EFORMAT;
    if ((avcc[4] & 3) == 2) return MR_EFORMAT;      /* no 3-byte lengths */
    if (nal_len_size) *nal_len_size = (avcc[4] & 3) + 1;
    /* Pass 0 copies the SPS list (count in byte 5), pass 1 the PPS list
     * (count in the byte after the SPS list). */
    for (pass = 0; pass < 2; pass++) {
        unsigned count, i;
        if (pass == 0) {
            count = avcc[5] & 0x1f;
        } else {
            if (pos >= len) return MR_EFORMAT;
            count = avcc[pos++];
        }
        for (i = 0; i < count; i++) {
            uint32_t n;
            if (pos + 2 > len) return MR_EFORMAT;
            n = mr_rb16(avcc + pos);
            pos += 2;
            if (!n || n > len - pos) return MR_EFORMAT;
            if (dst && out + 4 + n <= cap) {
                memcpy(dst + out, start_code, 4);
                memcpy(dst + out + 4, avcc + pos, n);
            }
            out += 4 + n;
            pos += n;
        }
    }
    if (out_len) *out_len = out;
    return out <= cap ? MR_OK : MR_ENOMEM;
}

mr_status mr_avcc_first_sps(const uint8_t *avcc, uint32_t len,
                            const uint8_t **sps, uint32_t *sps_len)
{
    uint32_t n;
    if (!avcc || len < 8 || avcc[0] != 1 || !(avcc[5] & 0x1f))
        return MR_EFORMAT;
    n = mr_rb16(avcc + 6);
    if (!n || n > len - 8) return MR_EFORMAT;
    *sps = avcc + 8;
    *sps_len = n;
    return MR_OK;
}

mr_status mr_avcc_sample_to_annexb(const uint8_t *src, uint32_t len,
                                   int nal_len_size, uint8_t *dst,
                                   uint32_t cap, uint32_t *out_len)
{
    uint32_t pos = 0, out = 0;
    if (!src || (nal_len_size != 1 && nal_len_size != 2 && nal_len_size != 4))
        return MR_EFORMAT;
    while (pos < len) {
        uint32_t n = 0;
        int i;
        if ((uint32_t)nal_len_size > len - pos) return MR_EFORMAT;
        for (i = 0; i < nal_len_size; i++) n = (n << 8) | src[pos++];
        if (!n || n > len - pos) return MR_EFORMAT;
        if (dst && out + 4 + n <= cap) {
            memcpy(dst + out, start_code, 4);
            memcpy(dst + out + 4, src + pos, n);
        }
        out += 4 + n;
        pos += n;
    }
    if (out_len) *out_len = out;
    return out <= cap ? MR_OK : MR_ENOMEM;
}

/* ---- SPS ------------------------------------------------------------- */

/* Reads an RBSP bit by bit, dropping each emulation prevention byte (the
 * 0x03 after two zero bytes) on the way. Past the end it reads zeros and
 * sets overrun. */
typedef struct {
    const uint8_t *p, *end;
    unsigned byte;
    int      bits;      /* bits left in byte                                  */
    int      zeros;     /* zero bytes just read, for emulation prevention     */
    int      overrun;
} bit_reader;

static int next_bit(bit_reader *b)
{
    if (!b->bits) {
        if (b->p < b->end && b->zeros >= 2 && *b->p == 3) {
            b->p++;
            b->zeros = 0;
        }
        if (b->p >= b->end) {
            b->overrun = 1;
            return 0;
        }
        b->byte = *b->p++;
        b->zeros = b->byte ? 0 : b->zeros + 1;
        b->bits = 8;
    }
    b->bits--;
    return (int)((b->byte >> b->bits) & 1);
}

static uint32_t get_bits(bit_reader *b, int n)
{
    uint32_t v = 0;
    while (n--) v = (v << 1) | (uint32_t)next_bit(b);
    return v;
}

static uint32_t get_ue(bit_reader *b)
{
    int zeros = 0;
    while (!next_bit(b)) {
        if (++zeros > 31 || b->overrun) {
            b->overrun = 1;
            return 0;
        }
    }
    return ((1u << zeros) - 1) + get_bits(b, zeros);
}

static int32_t get_se(bit_reader *b)
{
    uint32_t v = get_ue(b);
    return (v & 1) ? (int32_t)((v + 1) / 2) : -(int32_t)(v / 2);
}

static void skip_scaling_list(bit_reader *b, int size)
{
    int j, last = 8, next = 8;
    for (j = 0; j < size && !b->overrun; j++) {
        if (next) next = (last + get_se(b) + 256) % 256;
        if (next) last = next;
    }
}

mr_status mr_h264_sps_parse(const uint8_t *nal, uint32_t len,
                            mr_h264_sps *sps)
{
    bit_reader b;
    unsigned separate_planes = 0, map_units, crop_x, crop_y, i;
    uint32_t left = 0, right = 0, top = 0, bottom = 0, poc_type;
    if (!nal || len < 2 || (nal[0] & 0x1f) != 7 || !sps) return MR_EFORMAT;
    memset(sps, 0, sizeof *sps);
    memset(&b, 0, sizeof b);
    b.p = nal + 1;
    b.end = nal + len;
    sps->profile_idc = get_bits(&b, 8);
    get_bits(&b, 8);                            /* constraint flags */
    sps->level_idc = get_bits(&b, 8);
    get_ue(&b);                                 /* seq_parameter_set_id */
    sps->chroma_format_idc = 1;
    sps->bit_depth_luma = sps->bit_depth_chroma = 8;
    switch (sps->profile_idc) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86:
    case 118: case 128: case 138: case 139: case 134: case 135:
        sps->chroma_format_idc = get_ue(&b);
        if (sps->chroma_format_idc == 3) separate_planes = get_bits(&b, 1);
        sps->bit_depth_luma = 8 + get_ue(&b);
        sps->bit_depth_chroma = 8 + get_ue(&b);
        get_bits(&b, 1);                        /* qpprime_y_zero_bypass */
        if (get_bits(&b, 1)) {                  /* seq_scaling_matrix */
            for (i = 0; i < (sps->chroma_format_idc != 3 ? 8u : 12u); i++)
                if (get_bits(&b, 1)) skip_scaling_list(&b, i < 6 ? 16 : 64);
        }
        break;
    default:
        break;
    }
    get_ue(&b);                                 /* log2_max_frame_num */
    poc_type = get_ue(&b);
    if (poc_type == 0) {
        get_ue(&b);                             /* log2_max_poc_lsb */
    } else if (poc_type == 1) {
        uint32_t cycle;
        get_bits(&b, 1);                        /* delta_pic_order_always_zero */
        get_se(&b);                             /* offset_for_non_ref_pic */
        get_se(&b);                             /* offset_for_top_to_bottom */
        cycle = get_ue(&b);
        if (cycle > 255) return MR_EFORMAT;
        for (i = 0; i < cycle; i++) get_se(&b);
    }
    get_ue(&b);                                 /* max_num_ref_frames */
    get_bits(&b, 1);                            /* gaps_in_frame_num_allowed */
    sps->mb_width = get_ue(&b) + 1;
    map_units = get_ue(&b) + 1;
    sps->frame_mbs_only = (int)get_bits(&b, 1);
    if (!sps->frame_mbs_only) get_bits(&b, 1);  /* mb_adaptive_frame_field */
    get_bits(&b, 1);                            /* direct_8x8_inference */
    if (get_bits(&b, 1)) {                      /* frame_cropping */
        left = get_ue(&b);
        right = get_ue(&b);
        top = get_ue(&b);
        bottom = get_ue(&b);
    }
    if (b.overrun || sps->mb_width > 1024 || map_units > 1024)
        return MR_EFORMAT;
    sps->mb_height = map_units * (sps->frame_mbs_only ? 1 : 2);
    /* Crop units (7.4.2.1.1): chroma samples, and field pairs. */
    if (separate_planes || sps->chroma_format_idc == 0) {
        crop_x = 1;
        crop_y = 1;
    } else {
        crop_x = sps->chroma_format_idc == 3 ? 1 : 2;
        crop_y = sps->chroma_format_idc == 1 ? 2 : 1;
    }
    crop_y *= sps->frame_mbs_only ? 1 : 2;
    if ((left + right) * crop_x >= sps->mb_width * 16 ||
        (top + bottom) * crop_y >= sps->mb_height * 16)
        return MR_EFORMAT;
    sps->width = sps->mb_width * 16 - (left + right) * crop_x;
    sps->height = sps->mb_height * 16 - (top + bottom) * crop_y;
    return MR_OK;
}

int mr_h264_sps_fits(const mr_h264_sps *sps, unsigned max_width,
                     unsigned max_mbs)
{
    if (!sps) return 0;
    if (sps->profile_idc != 66 && sps->profile_idc != 77 &&
        sps->profile_idc != 100)
        return 0;
    return sps->chroma_format_idc == 1 && sps->bit_depth_luma == 8 &&
           sps->bit_depth_chroma == 8 &&
           sps->mb_width * 16 <= max_width &&
           sps->mb_width * sps->mb_height <= max_mbs;
}
