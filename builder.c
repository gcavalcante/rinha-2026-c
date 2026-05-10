
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define DIMS 16
#define MAGIC 0x52494E4849504B31ULL /* RINHIPK1 */
#define Q 8192

static inline int16_t q16(float x) {
    if (x <= -1.0f) return -Q;
    if (x >= 1.0f) return Q;
    return (int16_t)lrintf(x * (float)Q);
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

    int16_t *vectors = NULL;
    uint8_t *labels = NULL;

    if (posix_memalign((void **)&vectors, 64, vector_count * DIMS * sizeof(int16_t)) != 0 || !vectors) {
        perror("posix_memalign vectors");
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

        int16_t *v = vectors + ((size_t)count * DIMS);

        for (int i = 0; i < 14; i++) {
            char *next;
            float x = strtof(p, &next);
            v[i] = q16(x);
            p = next;

            while (p < end && *p != ',' && *p != ']') p++;
            if (*p == ',') p++;
        }

        v[14] = 0;
        v[15] = 0;

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
    uint32_t dims = DIMS;
    uint32_t vector_stride = DIMS * sizeof(int16_t);
    uint32_t header_size = 8 + 5 * 4;
    uint32_t vector_offset = align_up64(header_size);
    uint32_t label_offset = align_up64(vector_offset + count * vector_stride);

    fwrite(&magic, sizeof(magic), 1, out);
    fwrite(&count, sizeof(count), 1, out);
    fwrite(&dims, sizeof(dims), 1, out);
    fwrite(&vector_stride, sizeof(vector_stride), 1, out);
    fwrite(&vector_offset, sizeof(vector_offset), 1, out);
    fwrite(&label_offset, sizeof(label_offset), 1, out);

    long pos = ftell(out);
    while ((uint32_t)pos < vector_offset) {
        fputc(0, out);
        pos++;
    }

    fwrite(vectors, vector_stride, count, out);

    pos = ftell(out);
    while ((uint32_t)pos < label_offset) {
        fputc(0, out);
        pos++;
    }

    fwrite(labels, sizeof(uint8_t), count, out);

    fclose(out);
    free(vectors);
    free(labels);

    fprintf(stderr,
            "wrote exact-i16-packed index: selected=%u total_vectors=%u max_refs=%u stride=%u vector_bytes=%zu label_offset=%u total_bytes=%zu\n",
            count, total_vectors, max_refs, stride, (size_t)count * vector_stride,
            label_offset, (size_t)label_offset + count);

    return 0;
}
