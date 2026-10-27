/*
 * MintVID - H.264 stream setup for decoders that take Annex-B (hardware).
 *
 * MP4 and MKV carry H.264 as length-prefixed NAL units with an avcC record
 * (ISO/IEC 14496-15) holding the SPS and PPS; MPEG-TS carries an Annex-B byte
 * stream and its demuxer makes an avcC up from the first SPS/PPS. Decoders
 * such as the VideoCore's take Annex-B only, so these turn avcC and
 * length-prefixed samples into start-code form, and read the SPS fields that
 * decide whether such a decoder can take the stream at all. Portable, no
 * libavc: libavc keeps its own copies in mr_h264.c.
 */
#ifndef MR_AVC_H
#define MR_AVC_H

#include "mr_types.h"

/* The SPS fields a decoder's limits are about (ITU-T H.264 7.3.2.1.1). */
typedef struct {
    unsigned profile_idc;
    unsigned level_idc;
    unsigned chroma_format_idc;   /* 0 mono, 1 4:2:0, 2 4:2:2, 3 4:4:4     */
    unsigned bit_depth_luma;      /* 8..14                                 */
    unsigned bit_depth_chroma;
    unsigned mb_width;            /* in macroblocks                        */
    unsigned mb_height;           /* of the frame, field pairs included    */
    int      frame_mbs_only;
    unsigned width;               /* the visible picture, after cropping   */
    unsigned height;
} mr_h264_sps;

/* The SPS and PPS of an avcC record as Annex-B (four-byte start codes), into
 * dst. *nal_len_size gets the length-prefix size (1, 2 or 4) of the samples
 * that go with it. MR_EFORMAT for a malformed record, MR_ENOMEM when cap is
 * short (*out_len is then the size needed). */
mr_status mr_avcc_config_to_annexb(const uint8_t *avcc, uint32_t len,
                                   uint8_t *dst, uint32_t cap,
                                   uint32_t *out_len, int *nal_len_size);

/* The first SPS NAL unit inside an avcC record, header byte included. */
mr_status mr_avcc_first_sps(const uint8_t *avcc, uint32_t len,
                            const uint8_t **sps, uint32_t *sps_len);

/* One sample of length-prefixed NAL units as Annex-B, into dst. MR_EFORMAT
 * when a length runs past the sample or is zero, MR_ENOMEM when cap is short
 * (*out_len is then the size needed; nothing past cap is written). */
mr_status mr_avcc_sample_to_annexb(const uint8_t *src, uint32_t len,
                                   int nal_len_size, uint8_t *dst,
                                   uint32_t cap, uint32_t *out_len);

/* Parse an SPS NAL unit (header byte included, emulation prevention bytes
 * still in). MR_EFORMAT when it is not an SPS or is cut short. */
mr_status mr_h264_sps_parse(const uint8_t *nal, uint32_t len,
                            mr_h264_sps *sps);

/* Whether a decoder of Baseline, Main and High (8-bit 4:2:0) frames up to
 * max_width coded pixels across and max_mbs macroblocks takes this stream. */
int mr_h264_sps_fits(const mr_h264_sps *sps, unsigned max_width,
                     unsigned max_mbs);

#endif /* MR_AVC_H */
