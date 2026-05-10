
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define FULL_DIMS 16
#define FAST_DIMS 8
#define MAGIC 0x315346484E4952ULL /* RINHFS1, little-endian safe marker */
#define Q 8192

/*
 * Index format v2: full16 + fast8 + labels
 *
 * Header:
 *   uint64 magic
 *   uint32 count
 *   uint32 full_dims
 *   uint32 fast_dims
 *   uint32 full_stride
 *   uint32 fast_stride
 *   uint32 full_offset
 *   uint32 fast_offset
 *   uint32 label_offset
 *
 * Rationale:
 *   full16 keeps the original vector for final rerank.
 *   fast8 is a reduced/weighted projection used to select top candidates cheaply.
 */

static inline int16_t q16(float x) {
    if (x <= -1.0f) return -Q;
    if (x >= 1.0f) return Q;
    return (int16_t)lrintf(x * (float)Q);
}

static inline int16_t clamp_i16_i32(int32_t x) {
    if (x > 32767) return 32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}

/*
 * Fast vector dimensions from the 16-dim vector:
 *   2  amount_vs_avg
 *   5  minutes_since_last_tx
 *   6  km_from_last_tx
 *   7  km_from_home
 *   8  tx_count_24h
 *   11 unknown_merchant
 *   12 mcc_risk
 *   9  is_online
 *
 * Weights are represented as weight*10 and divided by 20, which gives:
 *   effective = source * weight / 2
 * This keeps the fast vector bounded roughly in [-8192, 8192] even with weight 2.0,
 * avoiding overflow in 16-bit SIMD subtraction/madd.
 */
static inline void make_fast8_from_full16(const int16_t full[FULL_DIMS], int16_t fast[FAST_DIMS]) {
    static const uint8_t dims[FAST_DIMS] = {2, 5, 6, 7, 8, 11, 12, 9};
    static const int16_t w10[FAST_DIMS] = {20, 15, 13, 15, 18, 20, 15, 12};

    for (int i = 0; i < FAST_DIMS; i++) {
        int32_t v = (int32_t)full[dims[i]] * (int32_t)w10[i];
        /* round away from zero before /20 */
        if (v >= 0) v = (v + 10) / 20;
        else v = (v - 10) / 20;
        fast[i] = clamp_i16_i32(v);
    }
}

static uint32_t align_up64(uint32_t x) {
    return (x + 63U) & ~63U;
}

static int read_all_stdin(char **out, size_t *out_len) {
    size_t cap = 64 * 1024 * 1024ULL;
    size_t len = 0;
    char *buf = (char *)malloc(cap);

    if (!buf) return -1;

    for (;;) {
        if (len == cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                return -1;
            }
            buf = nb;
        }

        size_t n = fread(buf + len, 1, cap - len, stdin);
        len += n;
        if (n == 0) break;
    }

    *out = buf;
    *out_len = len;
    return 0;
}

static uint32_t count_vectors(char *buf, char *end) {
    uint32_t total = 0;
    char *p = buf;

    while (p < end) {
        char *vec = strstr(p, "\"vector\"");
        if (!vec) break;
        total++;
        p = vec + 8;
    }

    return total;
}

static uint32_t env_u32(const char *name, uint32_t def) {
    const char *v = getenv(name);
    if (!v || !*v) return def;
    long x = strtol(v, NULL, 10);
    if (x <= 0) return def;
    if (x > 2000000000L) x = 2000000000L;
    return (uint32_t)x;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s index.bin < references.json\n", argv[0]);
        return 1;
    }

    char *buf = NULL;
    size_t len = 0;

    if (read_all_stdin(&buf, &len) != 0) {
        perror("read stdin");
        return 1;
    }

    char *end = buf + len;
    uint32_t total_vectors = count_vectors(buf, end);
    uint32_t max_refs = env_u32("MAX_REFS", 100000);
    if (max_refs > total_vectors) max_refs = total_vectors;

    uint32_t stride = 1;
    if (max_refs > 0 && total_vectors > max_refs) {
        stride = total_vectors / max_refs;
        if (stride < 1) stride = 1;
    }

    size_t vector_count = max_refs ? max_refs : 1;

    int16_t *full_vectors = NULL;
    int16_t *fast_vectors = NULL;
    uint8_t *labels = NULL;

    if (posix_memalign((void **)&full_vectors, 64, vector_count * FULL_DIMS * sizeof(int16_t)) != 0 || !full_vectors) {
        perror("posix_memalign full_vectors");
        return 1;
    }

    if (posix_memalign((void **)&fast_vectors, 64, vector_count * FAST_DIMS * sizeof(int16_t)) != 0 || !fast_vectors) {
        perror("posix_memalign fast_vectors");
        return 1;
    }

    if (posix_memalign((void **)&labels, 64, vector_count * sizeof(uint8_t)) != 0 || !labels) {
        perror("posix_memalign labels");
        return 1;
    }

    uint32_t count = 0;
    uint32_t seen = 0;
    char *p = buf;

    while (p < end) {
        char *vec = strstr(p, "\"vector\"");
        if (!vec) break;

        char *lb = strchr(vec, '[');
        if (!lb) break;

        uint32_t take = 0;
        if (count < max_refs) {
            if (total_vectors <= max_refs) take = 1;
            else if ((seen % stride) == 0) take = 1;
        }

        seen++;
        p = lb + 1;

        if (!take) {
            char *label_skip = strstr(p, "\"label\"");
            if (!label_skip) break;
            p = label_skip + 7;
            continue;
        }

        int16_t *full = full_vectors + ((size_t)count * FULL_DIMS);
        int16_t *fast = fast_vectors + ((size_t)count * FAST_DIMS);

        for (int i = 0; i < 14; i++) {
            char *next;
            float x = strtof(p, &next);
            full[i] = q16(x);
            p = next;

            while (p < end && *p != ',' && *p != ']') p++;
            if (*p == ',') p++;
        }

        full[14] = 0;
        full[15] = 0;
        make_fast8_from_full16(full, fast);

        char *label = strstr(p, "\"label\"");
        if (!label) break;

        char *obj_end = strchr(label, '}');
        char *fraud = strstr(label, "\"fraud\"");
        labels[count] = (fraud && obj_end && fraud < obj_end) ? 1 : 0;

        count++;
        p = label + 7;
        if (count >= max_refs) break;
    }

    free(buf);

    FILE *out = fopen(argv[1], "wb");
    if (!out) {
        perror("fopen");
        return 1;
    }

    uint64_t magic = MAGIC;
    uint32_t full_dims = FULL_DIMS;
    uint32_t fast_dims = FAST_DIMS;
    uint32_t full_stride = FULL_DIMS * sizeof(int16_t);
    uint32_t fast_stride = FAST_DIMS * sizeof(int16_t);
    uint32_t header_size = 8 + 8 * 4;
    uint32_t full_offset = align_up64(header_size);
    uint32_t fast_offset = align_up64(full_offset + count * full_stride);
    uint32_t label_offset = align_up64(fast_offset + count * fast_stride);

    fwrite(&magic, sizeof(magic), 1, out);
    fwrite(&count, sizeof(count), 1, out);
    fwrite(&full_dims, sizeof(full_dims), 1, out);
    fwrite(&fast_dims, sizeof(fast_dims), 1, out);
    fwrite(&full_stride, sizeof(full_stride), 1, out);
    fwrite(&fast_stride, sizeof(fast_stride), 1, out);
    fwrite(&full_offset, sizeof(full_offset), 1, out);
    fwrite(&fast_offset, sizeof(fast_offset), 1, out);
    fwrite(&label_offset, sizeof(label_offset), 1, out);

    long pos = ftell(out);
    while ((uint32_t)pos < full_offset) { fputc(0, out); pos++; }

    fwrite(full_vectors, full_stride, count, out);

    pos = ftell(out);
    while ((uint32_t)pos < fast_offset) { fputc(0, out); pos++; }

    fwrite(fast_vectors, fast_stride, count, out);

    pos = ftell(out);
    while ((uint32_t)pos < label_offset) { fputc(0, out); pos++; }

    fwrite(labels, sizeof(uint8_t), count, out);

    fclose(out);
    free(full_vectors);
    free(fast_vectors);
    free(labels);

    fprintf(stderr,
            "wrote fast8-rerank64 index: selected=%u total_vectors=%u max_refs=%u stride=%u full_bytes=%zu fast_bytes=%zu label_offset=%u total_bytes=%zu\n",
            count, total_vectors, max_refs, stride,
            (size_t)count * full_stride,
            (size_t)count * fast_stride,
            label_offset,
            (size_t)label_offset + count);

    return 0;
}
