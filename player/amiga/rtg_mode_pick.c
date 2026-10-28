/*
 * MintVID - shared CyberGraphX "native-or-downscale" private-screen mode
 * picker. See rtg_mode_pick.h for the policy this implements and why -
 * moved here, verbatim in algorithm, from what used to be display_p96.c's
 * own static p96_mode_for_source_at_depth() so display_cgx.c's private
 * screen can use the identical policy instead of always matching the
 * public screen's resolution (see display_cgx.c's cgx_open_private_screen()
 * for the real-hardware cost that gap had).
 */
#include "rtg_mode_pick.h"

#include <graphics/displayinfo.h>
#include <cybergraphx/cybergraphics.h>

#include <proto/cybergraphics.h>
#include <proto/graphics.h>

/* The best mode seen so far, by the policy in rtg_mode_pick.h: one that
 * contains the source (same shape first, then the least spare area, then
 * the closest size), else the closest smaller one. */
typedef struct {
    int   source_w, source_h;
    ULONG fit, fit_area, fit_delta, fit_aspect, fit_depth;
    ULONG any, any_delta, any_aspect, any_depth;
} mode_pick;

static void pick_init(mode_pick *p, int source_w, int source_h)
{
    p->source_w = source_w;
    p->source_h = source_h;
    p->fit = p->any = (ULONG)INVALID_ID;
    p->fit_area = p->fit_delta = p->fit_aspect = ~0UL;
    p->any_delta = p->any_aspect = ~0UL;
    p->fit_depth = p->any_depth = 0;
}

/* Score one mode; depth 0 takes it at any depth. */
static void pick_consider(mode_pick *p, ULONG modeid, ULONG depth)
{
    int source_w = p->source_w, source_h = p->source_h;
    ULONG mode_w, mode_h, mode_depth, delta, aspect_error;
    ULONG aspect_w, aspect_h;

    if (modeid == (ULONG)INVALID_ID || !IsCyberModeID(modeid))
        return;

    mode_w = GetCyberIDAttr(CYBRIDATTR_WIDTH, modeid);
    mode_h = GetCyberIDAttr(CYBRIDATTR_HEIGHT, modeid);
    mode_depth = GetCyberIDAttr(CYBRIDATTR_DEPTH, modeid);
    if (!mode_w || !mode_h || mode_w == ~0UL || mode_h == ~0UL ||
        (depth && mode_depth != depth))
        return;

    delta = (ULONG)((int)mode_w >= source_w ? (int)mode_w - source_w
                                            : source_w - (int)mode_w) +
            (ULONG)((int)mode_h >= source_h ? (int)mode_h - source_h
                                            : source_h - (int)mode_h);

    /*
     * Prefer a screen whose shape matches the video before considering
     * spare area. The old area-first score chose 1024x768 for 854x480
     * even when 1280x720 existed, putting a 16:9 video on a 4:3 scanout.
     * Some P96/monitor setups expand the whole private mode to the panel,
     * so the scanout shape must agree with the video's display aspect.
     */
    aspect_w = mode_w * (ULONG)source_h;
    aspect_h = mode_h * (ULONG)source_w;
    aspect_error = aspect_w >= aspect_h ? aspect_w - aspect_h
                                        : aspect_h - aspect_w;

    if ((int)mode_w >= source_w && (int)mode_h >= source_h) {
        ULONG source_area = (ULONG)source_w * (ULONG)source_h;
        ULONG mode_area = mode_w * mode_h;
        ULONG excess = mode_area >= source_area ? mode_area - source_area
                                                 : 0;
        if (p->fit == (ULONG)INVALID_ID ||
            aspect_error < p->fit_aspect ||
            (aspect_error == p->fit_aspect &&
             (excess < p->fit_area ||
              (excess == p->fit_area && delta < p->fit_delta)))) {
            p->fit = modeid;
            p->fit_area = excess;
            p->fit_delta = delta;
            p->fit_aspect = aspect_error;
            p->fit_depth = mode_depth;
        }
    } else if (p->any == (ULONG)INVALID_ID ||
               aspect_error < p->any_aspect ||
               (aspect_error == p->any_aspect && delta < p->any_delta)) {
        p->any = modeid;
        p->any_delta = delta;
        p->any_aspect = aspect_error;
        p->any_depth = mode_depth;
    }
}

static ULONG pick_result(const mode_pick *p, ULONG *depth_out)
{
    if (p->fit != (ULONG)INVALID_ID) {
        if (depth_out) *depth_out = p->fit_depth;
        return p->fit;
    }
    if (p->any != (ULONG)INVALID_ID && depth_out) *depth_out = p->any_depth;
    return p->any;
}

ULONG mr_rtg_best_mode_for_source(int source_w, int source_h, ULONG depth,
                                  ULONG *depth_out)
{
    static const UWORD common_modes[][2] = {
        { 320, 240 }, { 640, 400 }, { 640, 480 }, { 720, 480 },
        { 800, 600 }, { 1024, 768 }, { 1152, 864 },
        { 1280, 720 }, { 1280, 1024 }, { 1600, 900 },
        { 1600, 1200 }, { 1920, 1080 }
    };
    mode_pick p;
    unsigned i;

    pick_init(&p, source_w, source_h);
    for (i = 0; i <= sizeof common_modes / sizeof common_modes[0]; i++) {
        int request_w = i ? (int)common_modes[i - 1][0] : source_w;
        int request_h = i ? (int)common_modes[i - 1][1] : source_h;
        pick_consider(&p, BestCModeIDTags(
                              CYBRBIDTG_NominalWidth, (ULONG)request_w,
                              CYBRBIDTG_NominalHeight, (ULONG)request_h,
                              CYBRBIDTG_Depth, depth,
                              TAG_END), depth);
    }
    return pick_result(&p, depth_out);
}

ULONG mr_rtg_best_mode_where(int source_w, int source_h,
                             int (*accept)(ULONG modeid, void *ctx),
                             void *ctx, ULONG *depth_out)
{
    mode_pick p;
    ULONG modeid = (ULONG)INVALID_ID;

    pick_init(&p, source_w, source_h);
    while ((modeid = NextDisplayInfo(modeid)) != (ULONG)INVALID_ID)
        if (IsCyberModeID(modeid) && accept(modeid, ctx))
            pick_consider(&p, modeid, 0);
    return pick_result(&p, depth_out);
}
