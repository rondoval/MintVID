/*
 * MintVID - local-file and generic source ownership.
 */
#include "mr_source.h"
#include "mr_hls.h"
#include "mr_http.h"
#include "mr_youtube.h"
#include "mr_alloc.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MR_SOURCE_NAME_MAX 1024
#define MR_SOURCE_ERROR_MAX 192
/* A local file larger than its buffer is read through a window: the most
 * recently read bytes of the file, kept in a ring. Demuxers read forward
 * with short steps back (Matroska peeks at an element header, then reads
 * from just after its ID; MP4 alternates between nearby audio and video
 * chunks), and the window serves those from memory. A read just past its
 * end extends it by a step that doubles from MR_LOCAL_READ_MIN to
 * MR_LOCAL_READ_MAX while reading stays sequential, so a long run of packets
 * costs few AmigaDOS reads and no single read blocks playback for long; any
 * other read restarts the window at the new offset with the smallest step.
 *
 * The file is read with read() and lseek(), with no stdio buffer
 * underneath: libnix's fseek() throws its whole buffer away, and the next
 * small read refills all of it, which is why stdio buffering cannot stand
 * in for the window. */
#define MR_LOCAL_READ_MIN   (64u * 1024u)
#define MR_LOCAL_READ_MAX   (512u * 1024u)
/* The window without a Fast buffer: room for two minimum steps, so an
 * extension still keeps the step before it. */
#define MR_LOCAL_WINDOW_MIN (2u * MR_LOCAL_READ_MIN)

struct mr_source {
    void   *ctx;
    size_t  len;
    int   (*read_at)(void *, size_t, void *, size_t);
    void  (*close)(void *);
    char    final_name[MR_SOURCE_NAME_MAX];
    size_t  buffer_capacity;
    int     memory_cached;
};

typedef struct {
    int     fd;            /* -1 once the whole file is in buffer */
    size_t  len;           /* file size */
    size_t  fd_pos;        /* offset the next read() starts at */
    int     fd_pos_valid;
    unsigned char *buffer; /* the whole file, or the window's ring */
    size_t  buffer_size;
    int     cached;        /* buffer holds the whole file */
    size_t  win_off;       /* file offset of the window's first byte */
    size_t  win_len;       /* bytes in the window */
    size_t  win_head;      /* ring index of the byte at win_off */
    size_t  step;          /* next read-ahead size */
    size_t  step_min, step_max;
} file_source;

static char g_source_error[MR_SOURCE_ERROR_MAX];
static mr_source_timing g_timing;
static int g_timing_enabled = 0;

void mr_source_set_timing_enabled(int enabled) { g_timing_enabled = enabled != 0; }
int  mr_source_timing_enabled(void) { return g_timing_enabled; }

void mr_source_timing_get(mr_source_timing *timing)
{
    if (timing) *timing = g_timing;
}
void mr_source_timing_reset(void) { memset(&g_timing, 0, sizeof g_timing); }
void mr_source_timing_add_network(unsigned long ms) { g_timing.network_ms += ms; }
void mr_source_timing_add_hls_segment(unsigned long ms) { g_timing.hls_segment_ms += ms; }
void mr_source_timing_add_hls_playlist(unsigned long ms) { g_timing.hls_playlist_ms += ms; }


void mr_source_set_error(const char *message)
{
    size_t n;
    if (!message) message = "source open failed";
    n = strlen(message);
    if (n >= sizeof g_source_error) n = sizeof g_source_error - 1;
    memcpy(g_source_error, message, n);
    g_source_error[n] = '\0';
}

const char *mr_source_last_error(void)
{
    return g_source_error[0] ? g_source_error : "source open failed";
}

static int starts_nocase(const char *s, const char *prefix)
{
    while (*prefix) {
        int a = (unsigned char)*s++;
        int b = (unsigned char)*prefix++;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return 1;
}

int mr_source_is_url(const char *path)
{
    return path && (starts_nocase(path, "http://") ||
                    starts_nocase(path, "https://"));
}

mr_source *mr_source_create(void *ctx, size_t len,
                            int (*read_at)(void *, size_t, void *, size_t),
                            void (*close)(void *),
                            const char *final_name)
{
    mr_source *s;
    size_t n;
    /* len == MR_SOURCE_LEN_UNKNOWN marks a forward-only stream; only a truly
     * zero-length source is rejected. */
    if (!ctx || !read_at || !close || !len) return NULL;
    s = (mr_source *)mr_allocz(sizeof *s);
    if (!s) {
        close(ctx);
        mr_source_set_error("not enough memory for media source");
        return NULL;
    }
    s->ctx = ctx;
    s->len = len;
    s->read_at = read_at;
    s->close = close;
    if (!final_name) final_name = "";
    n = strlen(final_name);
    if (n >= sizeof s->final_name) n = sizeof s->final_name - 1;
    memcpy(s->final_name, final_name, n);
    s->final_name[n] = '\0';
    return s;
}

/* Reads len bytes at off straight from the file. */
static int fd_read_at(file_source *f, size_t off, unsigned char *dst,
                      size_t len)
{
    if (!f->fd_pos_valid || f->fd_pos != off) {
        if (off > 0x7fffffffUL ||
            lseek(f->fd, (off_t)off, SEEK_SET) != (off_t)off) {
            f->fd_pos_valid = 0;
            return 0;
        }
        f->fd_pos = off;
        f->fd_pos_valid = 1;
    }
    while (len) {
        long got = (long)read(f->fd, dst, len);
        if (got <= 0) {
            f->fd_pos_valid = 0;
            return 0;
        }
        dst += got;
        len -= (size_t)got;
        f->fd_pos += (size_t)got;
    }
    return 1;
}

/* Copies n window bytes starting at file offset off, which the window
 * holds. */
static void window_copy(const file_source *f, size_t off, unsigned char *dst,
                        size_t n)
{
    size_t at = (f->win_head + (off - f->win_off)) % f->buffer_size;
    size_t first = f->buffer_size - at;
    if (first > n) first = n;
    memcpy(dst, f->buffer + at, first);
    memcpy(dst + first, f->buffer, n - first);
}

/* Reads the n file bytes after the window's end into the ring, dropping its
 * oldest bytes to make room. n is at most the ring's size. */
static int window_extend(file_source *f, size_t n)
{
    size_t end = f->win_off + f->win_len, at, first;
    if (f->win_len + n > f->buffer_size) {
        size_t drop = f->win_len + n - f->buffer_size;
        f->win_off += drop;
        f->win_head = (f->win_head + drop) % f->buffer_size;
        f->win_len -= drop;
    }
    at = (f->win_head + f->win_len) % f->buffer_size;
    first = f->buffer_size - at;
    if (first > n) first = n;
    if (!fd_read_at(f, end, f->buffer + at, first) ||
        (n > first && !fd_read_at(f, end + first, f->buffer, n - first))) {
        f->win_len = 0;
        return 0;
    }
    f->win_len += n;
    return 1;
}

static int file_read_at(void *opaque, size_t off, void *dst, size_t len)
{
    file_source *f = (file_source *)opaque;
    unsigned char *out = (unsigned char *)dst;
    if (!f || (!dst && len)) return 0;
    if (off > f->len || len > f->len - off) return 0;
    if (f->cached) {
        if (len) memcpy(dst, f->buffer + off, len);
        return 1;
    }
    while (len) {
        size_t end = f->win_off + f->win_len, want;
        if (off >= f->win_off && off < end) {
            size_t n = end - off;
            if (n > len) n = len;
            window_copy(f, off, out, n);
            off += n;
            out += n;
            len -= n;
            continue;
        }
        if (len > f->buffer_size / 2) {
            /* Too big to keep: read it straight into the caller's buffer
             * and go on from its end. */
            if (!fd_read_at(f, off, out, len)) return 0;
            f->win_off = off + len;
            f->win_len = 0;
            f->win_head = 0;
            return 1;
        }
        if (off >= end && (off == end || f->win_len) &&
            off - end + len <= f->buffer_size / 2) {
            /* Ahead of the window but within reach: read through any gap,
             * so a reader still working behind stays in the window. Only
             * reading straight on makes the steps grow. */
            if (f->win_len && off - end <= f->step &&
                f->step < f->step_max) {
                f->step *= 2;
                if (f->step > f->step_max) f->step = f->step_max;
            }
        } else {
            f->win_off = end = off;
            f->win_len = 0;
            f->win_head = 0;
            f->step = f->step_min;
        }
        want = off - end + (len > f->step ? len : f->step);
        if (want > f->len - end) want = f->len - end;
        if (!window_extend(f, want)) return 0;
    }
    return 1;
}

static void file_close(void *opaque)
{
    file_source *f = (file_source *)opaque;
    if (!f) return;
    if (f->fd >= 0) close(f->fd);
    mr_free(f->buffer);
    free(f);
}

static mr_source *open_local_file(const char *path, size_t requested_buffer)
{
    file_source *ctx;
    mr_source *source;
    off_t end;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        mr_source_set_error("cannot open local file");
        return NULL;
    }
    end = lseek(fd, 0, SEEK_END);
    if (end <= 0) {
        close(fd);
        mr_source_set_error("cannot determine local file size");
        return NULL;
    }
    ctx = (file_source *)calloc(1, sizeof *ctx);
    if (!ctx) {
        close(fd);
        mr_source_set_error("not enough memory for local file");
        return NULL;
    }
    ctx->fd = fd;
    ctx->len = (size_t)end;

    /* Whole file fits inside the Fast buffer budget: read it once, up
     * front, into a block sized to the file itself, then close the file.
     * Every later mr_source_read_at() is then served straight from memory,
     * which is what a "Fast buffer" promises. */
    if (requested_buffer && ctx->len <= requested_buffer) {
        unsigned char *buffer = (unsigned char *)mr_alloc_fast(ctx->len);
        if (buffer) {
            if (fd_read_at(ctx, 0, buffer, ctx->len)) {
                close(fd);
                ctx->fd = -1;
                ctx->buffer = buffer;
                ctx->buffer_size = ctx->len;
                ctx->cached = 1;
                source = mr_source_create(ctx, ctx->len, file_read_at,
                                          file_close, path);
                mr_source_set_buffer_capacity(source, ctx->len);
                mr_source_set_memory_cached(source, 1);
                return source;
            }
            mr_free(buffer);
            close(fd);
            free(ctx);
            mr_source_set_error("cannot read local file");
            return NULL;
        }
        /* Not enough contiguous Fast RAM for even the exact file size -
         * fall through to the window below. */
    }

    /* The Fast buffer becomes the window, halving until it fits; without
     * one, the window is the smallest that still works. */
    if (requested_buffer) {
        size_t attempt = requested_buffer;
        while (attempt >= 4u * 1024u * 1024u) {
            ctx->buffer = (unsigned char *)mr_alloc_fast(attempt);
            if (ctx->buffer) {
                ctx->buffer_size = attempt;
                break;
            }
            attempt /= 2;
        }
    }
    if (!ctx->buffer) {
        ctx->buffer = (unsigned char *)mr_alloc(MR_LOCAL_WINDOW_MIN);
        ctx->buffer_size = MR_LOCAL_WINDOW_MIN;
    }
    if (!ctx->buffer) {
        close(fd);
        free(ctx);
        mr_source_set_error("not enough memory for local file");
        return NULL;
    }
    ctx->step_max = ctx->buffer_size / 2;
    if (ctx->step_max > MR_LOCAL_READ_MAX) ctx->step_max = MR_LOCAL_READ_MAX;
    ctx->step_min = MR_LOCAL_READ_MIN;
    if (ctx->step_min > ctx->step_max) ctx->step_min = ctx->step_max;
    ctx->step = ctx->step_min;
    source = mr_source_create(ctx, ctx->len, file_read_at, file_close, path);
    mr_source_set_buffer_capacity(source, ctx->buffer_size);
    return source;
}

mr_source *mr_source_open_ex(const char *path,
                             const struct mr_http_options *options)
{
    char youtube_media[MR_HTTP_URL_MAX];
    mr_http_options youtube_options;
    mr_youtube_media_kind youtube_kind;
    g_source_error[0] = '\0';
    if (!path || !*path) {
        mr_source_set_error("empty media path");
        return NULL;
    }
    if (mr_source_is_hls(path))
        return mr_hls_source_open_ex(path, options);
    if (mr_youtube_is_url(path)) {
        if (!mr_youtube_http_options_init(&youtube_options, options) ||
            !mr_youtube_resolve_media(path, &youtube_options, youtube_media,
                                      sizeof youtube_media, &youtube_kind) ||
            !mr_youtube_media_http_options_init(&youtube_options, options))
            return NULL;
        if (youtube_kind == MR_YOUTUBE_MEDIA_HLS ||
            youtube_kind == MR_YOUTUBE_MEDIA_HLS_VOD)
            return mr_hls_source_open_ex(youtube_media, &youtube_options);
        return mr_http_source_open_ex(youtube_media, &youtube_options);
    }
    if (mr_source_is_url(path))
        return mr_http_source_open_ex(path, options);
    return open_local_file(path, options ? options->source_buffer_bytes : 0);
}

mr_source *mr_source_open(const char *path)
{
    return mr_source_open_ex(path, NULL);
}

int mr_source_read_at(mr_source *s, size_t off, void *dst, size_t len)
{
    if (!s || (!dst && len)) return 0;
    /* Seekable sources are bounds-checked against their known size. A streaming
     * source has no known end, so the backend reports EOF via a short read. */
    if (s->len != MR_SOURCE_LEN_UNKNOWN &&
        (off > s->len || len > s->len - off))
        return 0;
    return s->read_at(s->ctx, off, dst, len);
}

size_t mr_source_length(const mr_source *s)
{
    return s ? s->len : 0;
}

int mr_source_is_streaming(const mr_source *s)
{
    return s && s->len == MR_SOURCE_LEN_UNKNOWN;
}

const char *mr_source_final_name(const mr_source *s)
{
    return s ? s->final_name : "";
}

void mr_source_close(mr_source *s)
{
    if (!s) return;
    s->close(s->ctx);
    mr_free(s);
}

size_t mr_source_buffer_capacity(const mr_source *s)
{
    return s ? s->buffer_capacity : 0;
}

void mr_source_set_buffer_capacity(mr_source *s, size_t bytes)
{
    if (s) s->buffer_capacity = bytes;
}

int mr_source_is_memory_cached(const mr_source *s)
{
    return s && s->memory_cached;
}

void mr_source_set_memory_cached(mr_source *s, int cached)
{
    if (s) s->memory_cached = cached != 0;
}
