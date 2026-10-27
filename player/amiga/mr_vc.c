/*
 * MintVID - H.264 and MJPEG decoded by the Raspberry Pi's VideoCore through
 * videocodec.library (a PiStorm with Emu68 1.1 and its vchiq stack).
 *
 * The backend for core/mr_hwdec.c: each mr_hwdec_ops call is one library
 * call. open() refuses what the VPU cannot take, and the registry's software
 * decoder for the same fourcc takes over: no library (it is opened at run
 * time), no VPU, an H.264 SPS outside 8-bit 4:2:0 Baseline/Main/High up to
 * 2032 pixels across and 8192 macroblocks (the firmware decodes those into
 * wrong pictures rather than refusing them), an MJPEG larger than the
 * decoder's row.
 *
 * Frames come out as I420 at the stream's size, or, once the player asks
 * through set_output, in a screen format at the display's size: the VPU does
 * the scaling and the colour conversion, and the rows go straight to the
 * screen. The session is opened with an output format and size, so its
 * pictures go through the ISP from the start: a session opened without one
 * gives the decoder's native frames and cannot change format later. The
 * ISP's rows are whole groups of 32 pixels (64 for I420), so a width that is
 * not gets a wider frame with the picture at its left.
 */
#include "../core/mr_hwdec.h"
#include "../core/mr_avc.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <utility/tagitem.h>
#include <libraries/videocodec.h>
#include <proto/exec.h>

static struct Library *VideoCodecBase;
#include <inline/videocodec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VC_MAX_WIDTH  2032          /* wider: wrong pictures, no error     */
#define VC_MAX_MBS    8192
#define VC_REVISION   3             /* videocodec.library 0.3, the first   */
#define VC_INPUTS     4
#define VC_INPUT_SIZE (256 * 1024)
#define VC_OUTPUTS    4

typedef struct {
    struct VCSession   *s;
    BYTE                sig;
    struct MsgPort     *timer_port;
    struct timerequest *timer;
    int                 timer_open;
    int                 width, height;   /* the container's picture        */
} vc_backend;

static int vc_poll(void *be, mr_hwdec_item *item)
{
    vc_backend *vc = (vc_backend *)be;
    struct VCItem it;
    memset(item, 0, sizeof *item);
    while (VC_Poll(vc->s, &it)) {
        if (it.Type == VCIT_PacketReturned) {
            item->kind = MR_HWDEC_PACKET;
            item->packet.data = (uint8_t *)it.u.Packet->Data;
            item->packet.capacity = it.u.Packet->Capacity;
            item->packet.handle = it.u.Packet;
            return 1;
        }
        if (it.Type == VCIT_Frame) {
            struct VCFrame *f = it.u.Frame;
            mr_hwdec_picture *p = &item->picture;
            UBYTE *base = (UBYTE *)f->Data;
            int x = (int)f->Crop.X, y = (int)f->Crop.Y;
            switch (f->Format) {
            case VCF_I420:
                p->fmt = MR_PIX_YUV420P;
                p->plane[0] = base + f->Offset[0] + y * f->Pitch[0] + x;
                p->plane[1] = base + f->Offset[1] + y / 2 * f->Pitch[1] + x / 2;
                p->plane[2] = base + f->Offset[2] + y / 2 * f->Pitch[2] + x / 2;
                break;
            case VCF_RGB24:
            case VCF_BGR24:
                p->fmt = f->Format == VCF_RGB24 ? MR_PIX_RGB24 : MR_PIX_BGR24;
                p->plane[0] = base + f->Offset[0] + y * f->Pitch[0] + x * 3;
                break;
            case VCF_BGRA32:
                p->fmt = MR_PIX_BGRA32;
                p->plane[0] = base + f->Offset[0] + y * f->Pitch[0] + x * 4;
                break;
            case VCF_RGB16:
                p->fmt = MR_PIX_RGB565LE;
                p->plane[0] = base + f->Offset[0] + y * f->Pitch[0] + x * 2;
                break;
            default:                /* nothing asked for this: skip it */
                VC_ReleaseFrame(vc->s, f);
                continue;
            }
            p->width = (int)f->Crop.Width;
            p->height = (int)f->Crop.Height;
            p->pitch[0] = (int)f->Pitch[0];
            p->pitch[1] = (int)f->Pitch[1];
            p->pitch[2] = (int)f->Pitch[2];
            p->has_pts = f->Pts != VC_TIME_UNKNOWN;
            p->pts_us = p->has_pts ? (uint64_t)f->Pts : 0;
            p->handle = f;
            item->kind = MR_HWDEC_PICTURE;
            return 1;
        }
        if (it.Type == VCIT_Event) {
            if (it.u.Event.Event == VCEV_EndOfStream) {
                item->kind = MR_HWDEC_ENDED;
                return 1;
            }
            if (it.u.Event.Event == VCEV_Error) {
                printf("videocore: decoder failed (%ld)\n",
                       (long)it.u.Event.Status);
                item->kind = MR_HWDEC_FAILED;
                return 1;
            }
            /* VCEV_StreamInfo: each frame describes itself. */
        }
    }
    return 0;
}

static mr_status vc_send(void *be, void *packet, uint32_t len, unsigned flags,
                         int has_pts, uint64_t pts_us)
{
    vc_backend *vc = (vc_backend *)be;
    struct VCPacket *p = (struct VCPacket *)packet;
    LONG rc;
    p->Length = len;
    p->Pts = has_pts ? (VCTime)pts_us : VC_TIME_UNKNOWN;
    p->Flags = (flags & MR_HWDEC_START ? VCPF_FrameStart : 0) |
               (flags & MR_HWDEC_END ? VCPF_FrameEnd : 0) |
               (flags & MR_HWDEC_EOS ? VCPF_EndOfStream : 0);
    rc = VC_SendPacket(vc->s, p);
    if (rc) printf("videocore: packet refused (%ld)\n", (long)rc);
    return rc ? MR_ERR : MR_OK;
}

static void vc_release(void *be, void *picture)
{
    VC_ReleaseFrame(((vc_backend *)be)->s, (struct VCFrame *)picture);
}

static mr_status vc_flush(void *be)
{
    return VC_Flush(((vc_backend *)be)->s) ? MR_ERR : MR_OK;
}

/* The session's signal, or the timer: a wake by the signal counts as 1 ms,
 * so a burst of signals cannot make a short wait look like a long one, and
 * a wait never takes no time at all. */
static unsigned vc_wait(void *be, unsigned ms)
{
    vc_backend *vc = (vc_backend *)be;
    ULONG timer_bit = 1UL << vc->timer_port->mp_SigBit;
    unsigned waited = 1;
    vc->timer->tr_node.io_Command = TR_ADDREQUEST;
    vc->timer->tr_time.tv_secs = ms / 1000;
    vc->timer->tr_time.tv_micro = (ms % 1000) * 1000;
    SendIO((struct IORequest *)vc->timer);
    Wait((1UL << vc->sig) | timer_bit);
    if (CheckIO((struct IORequest *)vc->timer)) waited = ms;
    else AbortIO((struct IORequest *)vc->timer);
    WaitIO((struct IORequest *)vc->timer);
    /* A reused timerequest leaves its port's signal set. */
    SetSignal(0, timer_bit);
    return waited;
}

/* The ISP's frame for a w x h picture: rows of whole 32 pixels (64 for
 * I420), an even number of them, the picture at the top left. */
static void vc_geometry(ULONG vcf, int w, int h, ULONG *row, ULONG *rows,
                        struct VCRect *dest)
{
    *row = (ULONG)mr_hwdec_row_pixels(w, vcf == VCF_I420 ? 64 : 32);
    *rows = (ULONG)((h + 1) & ~1);
    dest->X = 0;
    dest->Y = 0;
    dest->Width = w;
    dest->Height = h;
}

static mr_status vc_set_output(void *be, mr_pixfmt fmt, int w, int h)
{
    vc_backend *vc = (vc_backend *)be;
    struct VCRect dest;
    ULONG vcf, row, rows;
    LONG rc;
    switch (fmt) {
    case MR_PIX_YUV420P:  vcf = VCF_I420; break;
    case MR_PIX_RGB24:    vcf = VCF_RGB24; break;
    case MR_PIX_BGR24:    vcf = VCF_BGR24; break;
    case MR_PIX_BGRA32:   vcf = VCF_BGRA32; break;
    case MR_PIX_RGB565LE: vcf = VCF_RGB16; break;
    default:              return MR_EUNSUPPORTED;
    }
    if (!w || !h) {
        w = vc->width;
        h = vc->height;
    }
    vc_geometry(vcf, w, h, &row, &rows, &dest);
    rc = VC_SetAttrsTags(vc->s, VCA_OutputFormat, vcf,
                         VCA_OutputWidth, row,
                         VCA_OutputHeight, rows,
                         VCA_DestRect, (ULONG)&dest, TAG_DONE);
    if (rc) printf("videocore: %s output %dx%d refused (%ld)\n",
                   vcf == VCF_I420 ? "I420" : vcf == VCF_BGR24 ? "BGR24" :
                   vcf == VCF_BGRA32 ? "BGRA32" :
                   vcf == VCF_RGB16 ? "RGB565LE" : "RGB24",
                   w, h, (long)rc);
    return rc ? MR_EUNSUPPORTED : MR_OK;
}

static void vc_close(void *be)
{
    vc_backend *vc = (vc_backend *)be;
    if (vc->s) VC_CloseSession(vc->s);
    if (vc->timer_open) CloseDevice((struct IORequest *)vc->timer);
    if (vc->timer) DeleteIORequest((struct IORequest *)vc->timer);
    if (vc->timer_port) DeleteMsgPort(vc->timer_port);
    if (vc->sig >= 0) FreeSignal(vc->sig);
    free(vc);
    if (VideoCodecBase) {
        CloseLibrary(VideoCodecBase);
        VideoCodecBase = NULL;
    }
}

static const mr_hwdec_ops vc_ops = {
    vc_poll, vc_send, vc_release, vc_flush, vc_wait, vc_set_output, vc_close
};

/* The decoder row the library lists for this codec, if it has one. */
static int vc_row(ULONG codec, struct VCCodecInfo *ci)
{
    ULONG i;
    for (i = 0; ; i++) {
        memset(ci, 0, sizeof *ci);
        ci->Size = sizeof *ci;
        if (VC_QueryCodec(i, ci) != 0) return 0;
        if (ci->Codec == codec && ci->Kind == VCK_Decoder &&
            (ci->Flags & VCCF_Session))
            return 1;
    }
}

static mr_status vc_open(mr_decoder *dec, ULONG codec)
{
    struct VCCodecInfo ci;
    struct VCRect dest;
    ULONG row, rows;
    vc_backend *vc;
    LONG err = 0;

    if (codec == VCC_H264) {
        const uint8_t *nal;
        uint32_t nal_len;
        mr_h264_sps sps;
        if (mr_avcc_first_sps(dec->config, dec->config_len, &nal, &nal_len)
                != MR_OK ||
            mr_h264_sps_parse(nal, nal_len, &sps) != MR_OK)
            return MR_EUNSUPPORTED;
        if (!mr_h264_sps_fits(&sps, VC_MAX_WIDTH, VC_MAX_MBS)) {
            printf("videocore: H.264 profile %u, %u-bit, chroma %u, %ux%u "
                   "is past what the VPU takes\n", sps.profile_idc,
                   sps.bit_depth_luma, sps.chroma_format_idc, sps.width,
                   sps.height);
            return MR_EUNSUPPORTED;
        }
    }
    VideoCodecBase = OpenLibrary((CONST_STRPTR)VIDEOCODEC_NAME, 0);
    if (!VideoCodecBase) return MR_EUNSUPPORTED;
    if (VideoCodecBase->lib_Version == 0 &&
        VideoCodecBase->lib_Revision < VC_REVISION) {
        CloseLibrary(VideoCodecBase);
        VideoCodecBase = NULL;
        return MR_EUNSUPPORTED;
    }
    if (!vc_row(codec, &ci) ||
        (ci.MaxWidth && ci.MaxHeight &&
         (ULONG)dec->width * (ULONG)dec->height > ci.MaxWidth * ci.MaxHeight)) {
        if (codec == VCC_MJPEG)
            printf("videocore: no MJPEG decoder for %dx%d\n", dec->width,
                   dec->height);
        CloseLibrary(VideoCodecBase);
        VideoCodecBase = NULL;
        return MR_EUNSUPPORTED;
    }
    vc = (vc_backend *)calloc(1, sizeof *vc);
    if (!vc) {
        CloseLibrary(VideoCodecBase);
        VideoCodecBase = NULL;
        return MR_ENOMEM;
    }
    vc->width = dec->width;
    vc->height = dec->height;
    vc->sig = AllocSignal(-1);
    vc->timer_port = CreateMsgPort();
    vc->timer = vc->timer_port
        ? (struct timerequest *)CreateIORequest(vc->timer_port,
                                                sizeof(struct timerequest))
        : NULL;
    if (vc->sig < 0 || !vc->timer ||
        OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                   (struct IORequest *)vc->timer, 0) != 0) {
        vc_close(vc);
        return MR_ENOMEM;
    }
    vc->timer_open = 1;
    /* I420 at the container's size, through the ISP, until the player asks
     * for something else (vc_set_output()). */
    vc_geometry(VCF_I420, vc->width, vc->height, &row, &rows, &dest);
    vc->s = VC_OpenSessionTags(VCA_Codec, codec,
                               VCA_Kind, VCK_Decoder,
                               VCA_SigMask, 1UL << vc->sig,
                               VCA_ErrorPtr, (ULONG)&err,
                               VCA_Framed, TRUE,
                               VCA_InputCount, VC_INPUTS,
                               VCA_InputSize, VC_INPUT_SIZE,
                               VCA_OutputCount, VC_OUTPUTS,
                               VCA_OutputFormat, VCF_I420,
                               VCA_OutputWidth, row,
                               VCA_OutputHeight, rows,
                               VCA_DestRect, (ULONG)&dest,
                               TAG_DONE);
    if (!vc->s) {
        /* No VPU (cma or gpu_mem not set up), or it is busy or gone. */
        printf("videocore: no decoder session (%ld)\n", (long)err);
        vc_close(vc);
        return MR_EUNSUPPORTED;
    }
    return mr_hwdec_attach(dec, &vc_ops, vc, codec == VCC_H264, VC_OUTPUTS);
}

static mr_status vc_open_h264(mr_decoder *dec)
{
    return vc_open(dec, VCC_H264);
}

static mr_status vc_open_mjpeg(mr_decoder *dec)
{
    return vc_open(dec, VCC_MJPEG);
}

const mr_codec mr_codec_vc_h264 = {
    "H.264/AVC (VideoCore)",
    { MR_FOURCC('a','v','c','1') },
    vc_open_h264, mr_hwdec_decode, mr_hwdec_close, mr_hwdec_flush
};

const mr_codec mr_codec_vc_mjpeg = {
    "MJPEG (VideoCore)",
    { MR_FOURCC('M','J','P','G'), MR_FOURCC('J','P','E','G') },
    vc_open_mjpeg, mr_hwdec_decode, mr_hwdec_close, mr_hwdec_flush
};
