
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define DIMS 16
#define BUCKET_BITS 11
#define BUCKET_COUNT (1u << BUCKET_BITS)
#define MAGIC 0x314B42484E4952ULL /* RINHBK1 */
#define Q 8192

typedef struct {
    uint16_t bucket;
    uint8_t label;
    int16_t v[DIMS];
} Rec;

static inline int16_t q16(float x) {
    if (x <= -1.0f) return -Q;
    if (x >= 1.0f) return Q;
    return (int16_t)lrintf(x * (float)Q);
}

static inline uint16_t bucket4_nonneg(int16_t v, int16_t t1, int16_t t2, int16_t t3) {
    if (v < 0) v = 0;
    if (v <= t1) return 0;
    if (v <= t2) return 1;
    if (v <= t3) return 2;
    return 3;
}

static inline uint16_t bucket_key_from_full16(const int16_t full[DIMS]) {
    uint16_t amount = bucket4_nonneg(full[2], 819, 2048, 4096);
    uint16_t kmhome = bucket4_nonneg(full[7], 410, 1638, 4096);
    uint16_t tx24   = bucket4_nonneg(full[8], 819, 2048, 4096);
    uint16_t mcc    = bucket4_nonneg(full[12], 2048, 4096, 6144);
    uint16_t card   = full[10] > (Q / 2);
    uint16_t unk    = full[11] > (Q / 2);
    uint16_t online = full[9]  > (Q / 2);

    return (uint16_t)((amount << 9) | (kmhome << 7) | (tx24 << 5) | (mcc << 3) |
                      (card << 2) | (unk << 1) | online);
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
            if (!nb) { free(buf); return -1; }
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
    uint32_t max_refs = env_u32("MAX_REFS", 300000);
    if (max_refs > total_vectors) max_refs = total_vectors;
    if (max_refs == 0) max_refs = 1;

    uint32_t stride = 1;
    if (total_vectors > max_refs) {
        stride = total_vectors / max_refs;
        if (stride < 1) stride = 1;
    }

    Rec *recs = NULL;
    if (posix_memalign((void **)&recs, 64, (size_t)max_refs * sizeof(Rec)) != 0 || !recs) {
        perror("posix_memalign recs");
        return 1;
    }

    uint32_t count = 0;
    uint32_t seen = 0;
    uint32_t fraud_count = 0;
    uint32_t bucket_counts[BUCKET_COUNT];
    memset(bucket_counts, 0, sizeof(bucket_counts));

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

        Rec *r = &recs[count];
        for (int i = 0; i < 14; i++) {
            char *next;
            float x = strtof(p, &next);
            r->v[i] = q16(x);
            p = next;
            while (p < end && *p != ',' && *p != ']') p++;
            if (*p == ',') p++;
        }
        r->v[14] = 0;
        r->v[15] = 0;
        r->bucket = bucket_key_from_full16(r->v);

        char *label = strstr(p, "\"label\"");
        if (!label) break;
        char *obj_end = strchr(label, '}');
        char *fraud = strstr(label, "\"fraud\"");
        r->label = (fraud && obj_end && fraud < obj_end) ? 1 : 0;
        fraud_count += r->label;
        bucket_counts[r->bucket]++;

        count++;
        p = label + 7;
        if (count >= max_refs) break;
    }

    free(buf);

    uint32_t *offsets = (uint32_t *)calloc((size_t)BUCKET_COUNT + 1, sizeof(uint32_t));
    uint32_t *cursor = (uint32_t *)calloc(BUCKET_COUNT, sizeof(uint32_t));
    if (!offsets || !cursor) {
        perror("calloc offsets");
        return 1;
    }

    offsets[0] = 0;
    for (uint32_t b = 0; b < BUCKET_COUNT; b++) {
        offsets[b + 1] = offsets[b] + bucket_counts[b];
        cursor[b] = offsets[b];
    }

    int16_t *vectors = NULL;
    uint8_t *labels = NULL;
    if (posix_memalign((void **)&vectors, 64, (size_t)count * DIMS * sizeof(int16_t)) != 0 || !vectors) {
        perror("posix_memalign vectors");
        return 1;
    }
    if (posix_memalign((void **)&labels, 64, (size_t)count * sizeof(uint8_t)) != 0 || !labels) {
        perror("posix_memalign labels");
        return 1;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint16_t b = recs[i].bucket;
        uint32_t pos = cursor[b]++;
        memcpy(vectors + ((size_t)pos * DIMS), recs[i].v, DIMS * sizeof(int16_t));
        labels[pos] = recs[i].label;
    }

    FILE *out = fopen(argv[1], "wb");
    if (!out) {
        perror("fopen");
        return 1;
    }

    uint64_t magic = MAGIC;
    uint32_t dims = DIMS;
    uint32_t bucket_count = BUCKET_COUNT;
    uint32_t vector_stride = DIMS * sizeof(int16_t);
    uint32_t header_size = 8 + 7 * 4;
    uint32_t vector_offset = align_up64(header_size);
    uint32_t label_offset = align_up64(vector_offset + count * vector_stride);
    uint32_t bucket_offset_offset = align_up64(label_offset + count * sizeof(uint8_t));

    fwrite(&magic, sizeof(magic), 1, out);
    fwrite(&count, sizeof(count), 1, out);
    fwrite(&dims, sizeof(dims), 1, out);
    fwrite(&bucket_count, sizeof(bucket_count), 1, out);
    fwrite(&vector_stride, sizeof(vector_stride), 1, out);
    fwrite(&vector_offset, sizeof(vector_offset), 1, out);
    fwrite(&label_offset, sizeof(label_offset), 1, out);
    fwrite(&bucket_offset_offset, sizeof(bucket_offset_offset), 1, out);

    long pos = ftell(out);
    while ((uint32_t)pos < vector_offset) { fputc(0, out); pos++; }
    fwrite(vectors, vector_stride, count, out);

    pos = ftell(out);
    while ((uint32_t)pos < label_offset) { fputc(0, out); pos++; }
    fwrite(labels, sizeof(uint8_t), count, out);

    pos = ftell(out);
    while ((uint32_t)pos < bucket_offset_offset) { fputc(0, out); pos++; }
    fwrite(offsets, sizeof(uint32_t), BUCKET_COUNT + 1, out);

    fclose(out);

    uint32_t non_empty = 0;
    uint32_t max_bucket = 0;
    for (uint32_t b = 0; b < BUCKET_COUNT; b++) {
        if (bucket_counts[b]) non_empty++;
        if (bucket_counts[b] > max_bucket) max_bucket = bucket_counts[b];
    }

    fprintf(stderr,
            "wrote bucket-exact16 index: selected=%u total_vectors=%u max_refs=%u stride=%u fraud=%u legit=%u buckets=%u non_empty=%u max_bucket=%u vector_bytes=%zu total_bytes=%zu\n",
            count, total_vectors, max_refs, stride, fraud_count, count - fraud_count,
            BUCKET_COUNT, non_empty, max_bucket,
            (size_t)count * vector_stride,
            (size_t)bucket_offset_offset + ((size_t)BUCKET_COUNT + 1) * sizeof(uint32_t));

    free(recs);
    free(vectors);
    free(labels);
    free(offsets);
    free(cursor);
    return 0;
}
