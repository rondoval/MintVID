#include "../core/mr_http.h"
#include "../core/mr_source.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_PATH "/tmp/mr_source_buffer_check.bin"
#define FAST_BUFFER_BYTES (4u * 1024u * 1024u)
#define WINDOW_PATH "/tmp/mr_source_window_check.bin"
#define WINDOW_FILE_BYTES (6u * 1024u * 1024u + 12345u)

static unsigned char pattern_byte(size_t off)
{
    uint32_t x = (uint32_t)off * 2654435761u;
    return (unsigned char)((x >> 13) ^ (x >> 24));
}

static int matches(const unsigned char *p, size_t off, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        if (p[i] != pattern_byte(off + i)) return 0;
    return 1;
}

static uint32_t rng_state;

static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 8;
}

/* Bytes and read() calls this process has made so far, from Linux's
 * /proc/self/io; both 0 where it does not exist. */
static void proc_reads(unsigned long *bytes, unsigned long *calls)
{
    char line[128];
    FILE *io = fopen("/proc/self/io", "r");
    *bytes = *calls = 0;
    if (!io) return;
    while (fgets(line, sizeof line, io)) {
        sscanf(line, "rchar: %lu", bytes);
        sscanf(line, "syscr: %lu", calls);
    }
    fclose(io);
}

static void write_window_file(void)
{
    static unsigned char chunk[64u * 1024u];
    FILE *file = fopen(WINDOW_PATH, "wb");
    size_t off, i, n;
    assert(file);
    for (off = 0; off < WINDOW_FILE_BYTES; off += n) {
        n = WINDOW_FILE_BYTES - off;
        if (n > sizeof chunk) n = sizeof chunk;
        for (i = 0; i < n; i++) chunk[i] = pattern_byte(off + i);
        assert(fwrite(chunk, 1, n, file) == n);
    }
    assert(fclose(file) == 0);
}

/* A file larger than its buffer is read through a window. Every read has to
 * return the right bytes whatever the pattern, and a demuxer reading forward
 * with short steps back must not make the window read the file more than
 * about once. */
static void check_window(size_t fast_buffer)
{
    static unsigned char got[2u * 1024u * 1024u];
    mr_http_options options;
    mr_source *source;
    unsigned long bytes0, calls0, bytes1, calls1;
    size_t pos, a, v;
    int i;

    assert(mr_http_options_init(&options, NULL, NULL));
    options.source_buffer_bytes = fast_buffer;
    source = mr_source_open_ex(WINDOW_PATH, &options);
    assert(source);
    assert(!mr_source_is_memory_cached(source));
    assert(mr_source_buffer_capacity(source) > 0);
    assert(mr_source_length(source) == WINDOW_FILE_BYTES);
    if (fast_buffer)
        assert(mr_source_buffer_capacity(source) == fast_buffer);

    /* Matroska: peek at a 16-byte element header, then read the payload
     * from just after the ID. Some payloads are bigger than the window. */
    rng_state = 1;
    proc_reads(&bytes0, &calls0);
    for (pos = 0; pos + 16 <= WINDOW_FILE_BYTES;) {
        size_t size = rng() % (192u * 1024u) + 1;
        if (rng() % 16 == 0) size = rng() % (1536u * 1024u) + 1;
        if (size > WINDOW_FILE_BYTES - pos - 3)
            size = WINDOW_FILE_BYTES - pos - 3;
        assert(mr_source_read_at(source, pos, got, 16));
        assert(matches(got, pos, 16));
        assert(mr_source_read_at(source, pos + 3, got, size));
        assert(matches(got, pos + 3, size));
        pos += 3 + size;
    }
    proc_reads(&bytes1, &calls1);
    if (bytes0) {
        assert(bytes1 - bytes0 < WINDOW_FILE_BYTES + WINDOW_FILE_BYTES / 8);
        /* A Fast buffer window reads in steps of up to 512 KB. */
        if (fast_buffer) assert(calls1 - calls0 < 64);
    }

    /* MP4: audio chunks a steady 300 KB behind the video, read in turn. */
    rng_state = 2;
    proc_reads(&bytes0, &calls0);
    for (v = 300u * 1024u; v < WINDOW_FILE_BYTES;) {
        size_t na = rng() % (8u * 1024u) + 1, nv = rng() % (60u * 1024u) + 1;
        if (nv > WINDOW_FILE_BYTES - v) nv = WINDOW_FILE_BYTES - v;
        a = v - 300u * 1024u;
        assert(mr_source_read_at(source, a, got, na));
        assert(matches(got, a, na));
        assert(mr_source_read_at(source, v, got, nv));
        assert(matches(got, v, nv));
        v += nv;
    }
    proc_reads(&bytes1, &calls1);
    if (bytes0 && fast_buffer)
        assert(bytes1 - bytes0 < WINDOW_FILE_BYTES + WINDOW_FILE_BYTES / 8);

    /* Anywhere, any size. */
    rng_state = 3;
    for (i = 0; i < 2000; i++) {
        size_t off = rng() % WINDOW_FILE_BYTES;
        size_t len = rng() % (i % 7 ? 4096u : sizeof got) + 1;
        if (len > WINDOW_FILE_BYTES - off) len = WINDOW_FILE_BYTES - off;
        assert(mr_source_read_at(source, off, got, len));
        assert(matches(got, off, len));
    }

    /* The end of the file. */
    assert(mr_source_read_at(source, WINDOW_FILE_BYTES - 1, got, 1));
    assert(matches(got, WINDOW_FILE_BYTES - 1, 1));
    assert(mr_source_read_at(source, WINDOW_FILE_BYTES - 100, got, 100));
    assert(matches(got, WINDOW_FILE_BYTES - 100, 100));
    assert(!mr_source_read_at(source, WINDOW_FILE_BYTES, got, 1));
    assert(!mr_source_read_at(source, WINDOW_FILE_BYTES - 1, got, 2));
    assert(mr_source_read_at(source, 0, got, 64));
    assert(matches(got, 0, 64));
    mr_source_close(source);
}

int main(int argc, char **argv)
{
    static const unsigned char expected[] = {
        0x4d, 0x69, 0x6e, 0x74, 0x56, 0x49, 0x44, 0x2d,
        0x62, 0x75, 0x66, 0x66, 0x65, 0x72, 0x0a
    };
    unsigned char got[sizeof expected];
    mr_http_options options;
    mr_source *source;
    FILE *file;

    if (argc == 2) {
        assert(mr_http_options_init(&options, NULL, NULL));
        options.source_buffer_bytes = 8u * 1024u * 1024u;
        source = mr_source_open_ex(argv[1], &options);
        assert(source);
        assert(mr_source_buffer_capacity(source) == 8u * 1024u * 1024u);
        assert(mr_source_length(source) != MR_SOURCE_LEN_UNKNOWN);
        assert(mr_source_length(source) >= sizeof got);
        assert(mr_source_read_at(source, 0, got, sizeof got));
        assert(mr_source_read_at(source,
                                 mr_source_length(source) - sizeof got,
                                 got, sizeof got));
        mr_source_close(source);
        puts("progressive HTTP Fast RAM buffer checks passed");
        return 0;
    }
    assert(argc == 1);
    file = fopen(TEST_PATH, "wb");

    assert(file);
    assert(fwrite(expected, 1, sizeof expected, file) == sizeof expected);
    assert(fclose(file) == 0);

    /* The test file is far smaller than the Fast buffer budget, so it must
     * be read into memory whole - not merely wrapped in a wider stdio
     * window sized to the (much larger) budget. Capacity therefore tracks
     * the file itself, and every further read must come from memory with
     * no dependence on the file still being open. */
    assert(mr_http_options_init(&options, NULL, NULL));
    options.source_buffer_bytes = FAST_BUFFER_BYTES;
    source = mr_source_open_ex(TEST_PATH, &options);
    assert(source);
    assert(mr_source_is_memory_cached(source));
    assert(mr_source_buffer_capacity(source) == sizeof expected);
    assert(mr_source_read_at(source, 0, got, sizeof got));
    assert(!memcmp(got, expected, sizeof got));
    assert(mr_source_read_at(source, 8, got, 6));
    assert(!memcmp(got, expected + 8, 6));
    /* Out-of-range reads against the cached copy must fail cleanly. */
    assert(!mr_source_read_at(source, sizeof expected, got, 1));
    remove(TEST_PATH); /* the on-disk file is no longer needed at all */
    assert(mr_source_read_at(source, 0, got, sizeof got));
    assert(!memcmp(got, expected, sizeof got));
    mr_source_close(source);

    file = fopen(TEST_PATH, "wb");
    assert(file);
    assert(fwrite(expected, 1, sizeof expected, file) == sizeof expected);
    assert(fclose(file) == 0);

    source = mr_source_open(TEST_PATH);
    assert(source);
    assert(!mr_source_is_memory_cached(source));
    assert(mr_source_read_at(source, 3, got, 9));
    assert(!memcmp(got, expected + 3, 9));
    mr_source_close(source);
    remove(TEST_PATH);

    write_window_file();
    check_window(0);
    check_window(FAST_BUFFER_BYTES);
    remove(WINDOW_PATH);
    puts("source Fast RAM buffer checks passed");
    return 0;
}
