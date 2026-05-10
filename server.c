
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>

#ifndef PORT
#define PORT 8080
#endif

#define DIMS 16
#define FULL_DIMS 16
#define FAST_DIMS 8
#define TOP_CANDIDATES 64
#define MAGIC 0x315346484E4952ULL /* RINHFS1 */
#define REQ_MAX 16384
#define Q 8192

typedef struct {
    const char *s;
    size_t n;
} StaticResp;

static uint32_t g_count;
static int16_t *g_full_vectors;
static int16_t *g_fast_vectors;
static uint8_t *g_labels;
static size_t g_map_len;

static const StaticResp READY_RESP = {
    "HTTP/1.1 204 No Content\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n",
    sizeof("HTTP/1.1 204 No Content\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n") - 1
};

static const StaticResp NOT_FOUND_RESP = {
    "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
    sizeof("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n") - 1
};

static const StaticResp BAD_RESP = {
    "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
    sizeof("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n") - 1
};

static const StaticResp SCORE_RESP[6] = {
    { "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.0}",
      sizeof("HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.0}") - 1 },
    { "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.2}",
      sizeof("HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.2}") - 1 },
    { "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":0.4}",
      sizeof("HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":0.4}") - 1 },
    { "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":0.6}",
      sizeof("HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":0.6}") - 1 },
    { "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":0.8}",
      sizeof("HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":0.8}") - 1 },
    { "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":1.0}",
      sizeof("HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":1.0}") - 1 }
};

static const int16_t INSTALLMENTS_Q[13] = {
    0, 683, 1365, 2048, 2731, 3413, 4096, 4779, 5461, 6144, 6827, 7509, 8192
};

static const int16_t HOUR_Q[24] = {
    0, 356, 712, 1069, 1425, 1781, 2137, 2493,
    2850, 3206, 3562, 3918, 4274, 4631, 4987, 5343,
    5699, 6055, 6412, 6768, 7124, 7480, 7836, 8192
};

static const int16_t DOW_Q[7] = {
    0, 1365, 2731, 4096, 5461, 6827, 8192
};

static const int16_t TX24_Q[21] = {
    0, 410, 819, 1229, 1638, 2048, 2458, 2867, 3277, 3686,
    4096, 4506, 4915, 5325, 5734, 6144, 6554, 6963, 7373, 7782, 8192
};

static inline float clamp01(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

static inline int16_t q16(float x) {
    if (x <= -1.0f) return -Q;
    if (x >= 1.0f) return Q;
    return (int16_t)lrintf(x * (float)Q);
}

static inline const char *find_lit(const char *p, const char *end, const char *lit, size_t n) {
    if (!p || !end || !lit || n == 0 || (size_t)(end - p) < n) return NULL;
    return (const char *)memmem(p, (size_t)(end - p), lit, n);
}

static inline const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p;
}

static inline const char *skip_to_value(const char *p, const char *end) {
    while (p < end && *p != ':') p++;
    if (p < end) p++;
    return skip_ws(p, end);
}

static inline float parse_float_fast(const char **pp, const char *end) {
    const char *p = *pp;
    int neg = 0;
    if (p < end && *p == '-') { neg = 1; p++; }

    int ip = 0;
    while (p < end && (unsigned)(*p - '0') <= 9) {
        ip = ip * 10 + (*p - '0');
        p++;
    }

    float v = (float)ip;
    if (p < end && *p == '.') {
        p++;
        float scale = 0.1f;
        while (p < end && (unsigned)(*p - '0') <= 9) {
            v += (float)(*p - '0') * scale;
            scale *= 0.1f;
            p++;
        }
    }

    *pp = p;
    return neg ? -v : v;
}

static inline int parse_int_fast(const char **pp, const char *end) {
    const char *p = *pp;
    int v = 0;
    while (p < end && (unsigned)(*p - '0') <= 9) {
        v = v * 10 + (*p - '0');
        p++;
    }
    *pp = p;
    return v;
}

static inline float field_float(const char *base, const char *end, const char *field, size_t flen, float def) {
    const char *p = find_lit(base, end, field, flen);
    if (!p) return def;
    p = skip_to_value(p, end);
    if (!p || p >= end || *p == 'n') return def;
    return parse_float_fast(&p, end);
}

static inline int field_int(const char *base, const char *end, const char *field, size_t flen, int def) {
    const char *p = find_lit(base, end, field, flen);
    if (!p) return def;
    p = skip_to_value(p, end);
    if (!p || p >= end || *p == 'n') return def;
    return parse_int_fast(&p, end);
}

static inline int field_bool(const char *base, const char *end, const char *field, size_t flen, int def) {
    const char *p = find_lit(base, end, field, flen);
    if (!p) return def;
    p = skip_to_value(p, end);
    if (!p || p >= end) return def;
    return (p + 4 <= end && memcmp(p, "true", 4) == 0) ? 1 : 0;
}

static int field_str(const char *base, const char *end, const char *field, size_t flen, char *out, size_t out_sz) {
    if (out_sz == 0) return 0;
    const char *p = find_lit(base, end, field, flen);
    if (!p) return 0;
    p = skip_to_value(p, end);
    if (!p || p >= end || *p != '"') return 0;
    p++;
    const char *q = p;
    while (q < end && *q != '"') q++;
    if (q >= end) return 0;
    size_t n = (size_t)(q - p);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}

static int object_bounds(const char *json, const char *end, const char *key, size_t klen,
                         const char **obj_start, const char **obj_end) {
    const char *p = find_lit(json, end, key, klen);
    if (!p) return 0;
    p = skip_to_value(p, end);
    if (!p || p >= end || *p == 'n') return 0;
    while (p < end && *p != '{') p++;
    if (p >= end) return 0;

    const char *s = p;
    int depth = 0;
    int in_str = 0;
    for (; p < end; p++) {
        char c = *p;
        if (in_str) {
            if (c == '\\') { p++; continue; }
            if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) {
                *obj_start = s;
                *obj_end = p + 1;
                return 1;
            }
        }
    }
    return 0;
}

static inline int parse_timestamp_at_quote(const char *p, const char *end,
                                           int *y, int *mo, int *d, int *h, int *mi, int *s) {
    if (p >= end || *p != '"') return 0;
    p++;
    if (p + 19 > end) return 0;
    *y  = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
    *mo = (p[5]-'0')*10 + (p[6]-'0');
    *d  = (p[8]-'0')*10 + (p[9]-'0');
    *h  = (p[11]-'0')*10 + (p[12]-'0');
    *mi = (p[14]-'0')*10 + (p[15]-'0');
    *s  = (p[17]-'0')*10 + (p[18]-'0');
    return 1;
}

static int field_timestamp(const char *base, const char *end, const char *field, size_t flen,
                           int *y, int *mo, int *d, int *h, int *mi, int *s) {
    const char *p = find_lit(base, end, field, flen);
    if (!p) return 0;
    p = skip_to_value(p, end);
    return parse_timestamp_at_quote(p, end, y, mo, d, h, mi, s);
}

static inline int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

static inline int weekday_mon0(int y, int m, int d) {
    int64_t z = days_from_civil(y, m, d);
    int w = (int)((z + 3) % 7);
    return w < 0 ? w + 7 : w;
}

static inline int64_t epoch_minutes(int y, int m, int d, int h, int mi) {
    return days_from_civil(y, m, d) * 1440 + h * 60 + mi;
}

static inline int16_t mcc_risk_q(const char *mcc) {
    if (!mcc || !mcc[0]) return Q / 2;
    if (!memcmp(mcc, "5411", 4)) return 1229;
    if (!memcmp(mcc, "5812", 4)) return 2458;
    if (!memcmp(mcc, "5912", 4)) return 1638;
    if (!memcmp(mcc, "5944", 4)) return 3686;
    if (!memcmp(mcc, "7801", 4)) return 6554;
    if (!memcmp(mcc, "7802", 4)) return 6144;
    if (!memcmp(mcc, "7995", 4)) return 6963;
    if (!memcmp(mcc, "4511", 4)) return 2867;
    if (!memcmp(mcc, "5311", 4)) return 2048;
    if (!memcmp(mcc, "5999", 4)) return 4096;
    return Q / 2;
}

static int array_contains_str(const char *base, const char *end, const char *field, size_t flen,
                              const char *value) {
    if (!value || !value[0]) return 0;
    const char *p = find_lit(base, end, field, flen);
    if (!p) return 0;
    p = skip_to_value(p, end);
    if (!p || p >= end || *p != '[') return 0;

    size_t vlen = strlen(value);
    const char *arr_end = p;
    while (arr_end < end && *arr_end != ']') arr_end++;
    if (arr_end >= end) return 0;

    while (p < arr_end) {
        while (p < arr_end && *p != '"') p++;
        if (p >= arr_end) break;
        p++;
        const char *q = p;
        while (q < arr_end && *q != '"') q++;
        if (q >= arr_end) break;
        if ((size_t)(q - p) == vlen && memcmp(p, value, vlen) == 0) return 1;
        p = q + 1;
    }
    return 0;
}

static void vectorize_fast(const char *json, const char *end, int16_t out[DIMS]) {
    const char *transaction = NULL, *transaction_end = NULL;
    const char *customer = NULL, *customer_end = NULL;
    const char *merchant = NULL, *merchant_end = NULL;
    const char *terminal = NULL, *terminal_end = NULL;
    const char *last = NULL, *last_end = NULL;

    object_bounds(json, end, "\"transaction\"", sizeof("\"transaction\"") - 1, &transaction, &transaction_end);
    object_bounds(json, end, "\"customer\"", sizeof("\"customer\"") - 1, &customer, &customer_end);
    object_bounds(json, end, "\"merchant\"", sizeof("\"merchant\"") - 1, &merchant, &merchant_end);
    object_bounds(json, end, "\"terminal\"", sizeof("\"terminal\"") - 1, &terminal, &terminal_end);
    object_bounds(json, end, "\"last_transaction\"", sizeof("\"last_transaction\"") - 1, &last, &last_end);

    float amount = transaction ? field_float(transaction, transaction_end, "\"amount\"", sizeof("\"amount\"") - 1, 0.0f) : 0.0f;
    int installments = transaction ? field_int(transaction, transaction_end, "\"installments\"", sizeof("\"installments\"") - 1, 0) : 0;

    float customer_avg = customer ? field_float(customer, customer_end, "\"avg_amount\"", sizeof("\"avg_amount\"") - 1, 1.0f) : 1.0f;
    if (customer_avg <= 0.0001f) customer_avg = 1.0f;

    int tx_count_24h = customer ? field_int(customer, customer_end, "\"tx_count_24h\"", sizeof("\"tx_count_24h\"") - 1, 0) : 0;

    char merchant_id[64] = {0};
    char mcc[8] = {0};
    if (merchant) {
        field_str(merchant, merchant_end, "\"id\"", sizeof("\"id\"") - 1, merchant_id, sizeof(merchant_id));
        field_str(merchant, merchant_end, "\"mcc\"", sizeof("\"mcc\"") - 1, mcc, sizeof(mcc));
    }

    float merchant_avg = merchant ? field_float(merchant, merchant_end, "\"avg_amount\"", sizeof("\"avg_amount\"") - 1, 0.0f) : 0.0f;

    int is_online = terminal ? field_bool(terminal, terminal_end, "\"is_online\"", sizeof("\"is_online\"") - 1, 0) : 0;
    int card_present = terminal ? field_bool(terminal, terminal_end, "\"card_present\"", sizeof("\"card_present\"") - 1, 0) : 0;
    float km_home = terminal ? field_float(terminal, terminal_end, "\"km_from_home\"", sizeof("\"km_from_home\"") - 1, 0.0f) : 0.0f;

    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    int has_req_ts = transaction ? field_timestamp(transaction, transaction_end, "\"requested_at\"", sizeof("\"requested_at\"") - 1, &y, &mo, &d, &h, &mi, &s) : 0;

    int16_t hour_q = has_req_ts && (unsigned)h < 24 ? HOUR_Q[h] : 0;
    int16_t dow_q = 0;
    if (has_req_ts) {
        int dow = weekday_mon0(y, mo, d);
        if ((unsigned)dow < 7) dow_q = DOW_Q[dow];
    }

    float minutes_since = -1.0f;
    float km_last = -1.0f;

    if (last) {
        int ly = 0, lmo = 0, ld = 0, lh = 0, lmi = 0, ls = 0;
        if (has_req_ts && field_timestamp(last, last_end, "\"timestamp\"", sizeof("\"timestamp\"") - 1, &ly, &lmo, &ld, &lh, &lmi, &ls)) {
            int64_t cur = epoch_minutes(y, mo, d, h, mi);
            int64_t prev = epoch_minutes(ly, lmo, ld, lh, lmi);
            int64_t diff = cur - prev;
            if (diff < 0) diff = 0;
            minutes_since = clamp01((float)diff / 1440.0f);
        }

        km_last = clamp01(field_float(last, last_end, "\"km_from_current\"", sizeof("\"km_from_current\"") - 1, 0.0f) / 1000.0f);
    }

    int unknown_merchant = customer ? !array_contains_str(customer, customer_end, "\"known_merchants\"", sizeof("\"known_merchants\"") - 1, merchant_id) : 1;

    memset(out, 0, sizeof(int16_t) * DIMS);

    out[0]  = q16(amount / 10000.0f);

    if (installments < 0) installments = 0;
    if (installments > 12) installments = 12;
    out[1]  = INSTALLMENTS_Q[installments];

    out[2]  = q16((amount / customer_avg) / 10.0f);
    out[3]  = hour_q;
    out[4]  = dow_q;
    out[5]  = minutes_since < 0.0f ? -Q : q16(minutes_since);
    out[6]  = km_last < 0.0f ? -Q : q16(km_last);
    out[7]  = q16(km_home / 1000.0f);

    if (tx_count_24h < 0) tx_count_24h = 0;
    if (tx_count_24h > 20) tx_count_24h = 20;
    out[8]  = TX24_Q[tx_count_24h];

    out[9]  = is_online ? Q : 0;
    out[10] = card_present ? Q : 0;
    out[11] = unknown_merchant ? Q : 0;
    out[12] = mcc_risk_q(mcc);
    out[13] = q16(merchant_avg / 10000.0f);
    out[14] = 0;
    out[15] = 0;
}

static inline int16_t clamp_i16_i32(int32_t x) {
    if (x > 32767) return 32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}

static inline void make_fast8_from_full16(const int16_t full[FULL_DIMS], int16_t fast[FAST_DIMS]) {
    static const uint8_t dims[FAST_DIMS] = {2, 5, 6, 7, 8, 11, 12, 9};
    static const int16_t w10[FAST_DIMS] = {20, 15, 13, 15, 18, 20, 15, 12};

    for (int i = 0; i < FAST_DIMS; i++) {
        int32_t v = (int32_t)full[dims[i]] * (int32_t)w10[i];
        if (v >= 0) v = (v + 10) / 20;
        else v = (v - 10) / 20;
        fast[i] = clamp_i16_i32(v);
    }
}

static inline uint32_t hsum4_epi32(__m128i v) {
    v = _mm_hadd_epi32(v, v);
    v = _mm_hadd_epi32(v, v);
    return (uint32_t)_mm_cvtsi128_si32(v);
}

static inline uint64_t dist8_i16_sse(const int16_t *a, const int16_t *b) {
    __m128i aa = _mm_load_si128((const __m128i *)a);
    __m128i bb = _mm_load_si128((const __m128i *)b);
    __m128i diff = _mm_sub_epi16(aa, bb);
    __m128i sq = _mm_madd_epi16(diff, diff);
    return hsum4_epi32(sq);
}

static inline uint64_t dist16_i16_avx2(const int16_t *a, const int16_t *b) {
    __m256i aa = _mm256_load_si256((const __m256i *)a);
    __m256i bb = _mm256_load_si256((const __m256i *)b);
    __m256i diff = _mm256_sub_epi16(aa, bb);
    __m256i sq = _mm256_madd_epi16(diff, diff);

    __m128i lo = _mm256_castsi256_si128(sq);
    __m128i hi = _mm256_extracti128_si256(sq, 1);
    __m128i sum = _mm_add_epi32(lo, hi);
    return hsum4_epi32(sum);
}

static inline void consider_fast_candidate(uint64_t d, uint32_t idx, uint64_t best_d[TOP_CANDIDATES], uint32_t best_i[TOP_CANDIDATES]) {
    if (d < best_d[TOP_CANDIDATES - 1]) {
        int pos = TOP_CANDIDATES - 1;
        while (pos > 0 && d < best_d[pos - 1]) {
            best_d[pos] = best_d[pos - 1];
            best_i[pos] = best_i[pos - 1];
            pos--;
        }
        best_d[pos] = d;
        best_i[pos] = idx;
    }
}

static inline void consider_full_candidate(const int16_t q[FULL_DIMS], const int16_t *v, uint8_t label, uint64_t best_d[5], uint8_t best_l[5]) {
    uint64_t d = dist16_i16_avx2(q, v);

    if (d < best_d[4]) {
        int pos = 4;
        while (pos > 0 && d < best_d[pos - 1]) {
            best_d[pos] = best_d[pos - 1];
            best_l[pos] = best_l[pos - 1];
            pos--;
        }
        best_d[pos] = d;
        best_l[pos] = label;
    }
}

static int fraud_count_two_stage(const int16_t qfull[FULL_DIMS]) {
    int16_t qfast[FAST_DIMS] __attribute__((aligned(16)));
    make_fast8_from_full16(qfull, qfast);

    uint64_t cand_d[TOP_CANDIDATES];
    uint32_t cand_i[TOP_CANDIDATES];

    for (int i = 0; i < TOP_CANDIDATES; i++) {
        cand_d[i] = UINT64_MAX;
        cand_i[i] = UINT32_MAX;
    }

    const int16_t *fast_vectors = g_fast_vectors;
    uint32_t n = g_count;

    for (uint32_t i = 0; i < n; i++) {
        const int16_t *v = fast_vectors + ((size_t)i * FAST_DIMS);
        uint64_t d = dist8_i16_sse(qfast, v);
        consider_fast_candidate(d, i, cand_d, cand_i);
    }

    uint64_t best_d[5] = { UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX };
    uint8_t best_l[5] = {0, 0, 0, 0, 0};

    const int16_t *full_vectors = g_full_vectors;
    const uint8_t *labels = g_labels;

    for (int k = 0; k < TOP_CANDIDATES; k++) {
        uint32_t idx = cand_i[k];
        if (idx == UINT32_MAX || idx >= n) break;
        const int16_t *v = full_vectors + ((size_t)idx * FULL_DIMS);
        consider_full_candidate(qfull, v, labels[idx], best_d, best_l);
    }

    return best_l[0] + best_l[1] + best_l[2] + best_l[3] + best_l[4];
}

static void load_index(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open index");
        exit(1);
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        perror("stat index");
        exit(1);
    }

    g_map_len = (size_t)st.st_size;
    uint8_t *map = mmap(NULL, g_map_len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (map == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    madvise(map, g_map_len, MADV_WILLNEED);

    volatile uint8_t warm = 0;
    for (size_t i = 0; i < g_map_len; i += 4096) {
        warm ^= map[i];
    }

    uint64_t magic;
    uint32_t full_dims, fast_dims, full_stride, fast_stride, full_offset, fast_offset, label_offset;

    memcpy(&magic, map, 8);
    memcpy(&g_count, map + 8, 4);
    memcpy(&full_dims, map + 12, 4);
    memcpy(&fast_dims, map + 16, 4);
    memcpy(&full_stride, map + 20, 4);
    memcpy(&fast_stride, map + 24, 4);
    memcpy(&full_offset, map + 28, 4);
    memcpy(&fast_offset, map + 32, 4);
    memcpy(&label_offset, map + 36, 4);

    size_t full_bytes = (size_t)g_count * FULL_DIMS * sizeof(int16_t);
    size_t fast_bytes = (size_t)g_count * FAST_DIMS * sizeof(int16_t);
    size_t label_bytes = (size_t)g_count;

    if (magic != MAGIC || full_dims != FULL_DIMS || fast_dims != FAST_DIMS ||
        full_stride != FULL_DIMS * sizeof(int16_t) || fast_stride != FAST_DIMS * sizeof(int16_t) ||
        full_offset >= g_map_len || fast_offset >= g_map_len || label_offset >= g_map_len ||
        full_offset + full_bytes > g_map_len || fast_offset + fast_bytes > g_map_len ||
        label_offset + label_bytes > g_map_len ||
        ((uintptr_t)(map + full_offset) % 32) != 0 ||
        ((uintptr_t)(map + fast_offset) % 16) != 0) {
        fprintf(stderr,
                "bad index: magic=%llx count=%u full_dims=%u fast_dims=%u full_stride=%u fast_stride=%u full_off=%u fast_off=%u label_off=%u len=%zu\n",
                (unsigned long long)magic, g_count, full_dims, fast_dims, full_stride, fast_stride,
                full_offset, fast_offset, label_offset, g_map_len);
        exit(1);
    }

    g_full_vectors = (int16_t *)(map + full_offset);
    g_fast_vectors = (int16_t *)(map + fast_offset);
    g_labels = (uint8_t *)(map + label_offset);

    fprintf(stderr,
            "loaded fast8-rerank64 index: %u vectors, index=%zu bytes, full_bytes=%zu, fast_bytes=%zu, label_offset=%u\n",
            g_count, g_map_len, full_bytes, fast_bytes, label_offset);
}

static inline void send_static(int fd, StaticResp r) {
    const char *s = r.s;
    size_t n = r.n;

    while (n) {
        ssize_t w = send(fd, s, n, MSG_NOSIGNAL);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

static int process_one_request(int fd, char *req, char *body, size_t body_len) {
    if (!strncmp(req, "GET /ready", 10)) {
        send_static(fd, READY_RESP);
        return 0;
    }

    if (strncmp(req, "POST /fraud-score", 17)) {
        send_static(fd, NOT_FOUND_RESP);
        return 1;
    }

    int16_t q[DIMS] __attribute__((aligned(32)));
    vectorize_fast(body, body + body_len, q);

    int frauds = fraud_count_two_stage(q);
    if (frauds < 0) frauds = 0;
    if (frauds > 5) frauds = 5;

    send_static(fd, SCORE_RESP[frauds]);
    return 0;
}

static void handle_client(int fd) {
    char buf[REQ_MAX + 1];
    size_t used = 0;

    for (;;) {
        char *hdr_end = NULL;

        if (used >= 4) {
            hdr_end = memmem(buf, used, "\r\n\r\n", 4);
        }

        if (hdr_end) {
            size_t header_len = (size_t)(hdr_end - buf) + 4;
            int content_len = 0;

            char *cl = strcasestr(buf, "Content-Length:");
            if (cl && cl < hdr_end) {
                content_len = atoi(cl + 15);
            }

            if (content_len < 0 || header_len + (size_t)content_len > REQ_MAX) {
                send_static(fd, BAD_RESP);
                close(fd);
                return;
            }

            if (used >= header_len + (size_t)content_len) {
                size_t req_len = header_len + (size_t)content_len;
                buf[req_len] = 0;

                int should_close = process_one_request(fd, buf, buf + header_len, (size_t)content_len);
                if (should_close) {
                    close(fd);
                    return;
                }

                size_t remaining = used - req_len;
                if (remaining > 0) memmove(buf, buf + req_len, remaining);
                used = remaining;
                if (used < REQ_MAX) buf[used] = 0;
                continue;
            }
        }

        if (used == REQ_MAX) {
            send_static(fd, BAD_RESP);
            close(fd);
            return;
        }

        ssize_t n = recv(fd, buf + used, REQ_MAX - used, 0);
        if (n <= 0) {
            close(fd);
            return;
        }

        used += (size_t)n;
        buf[used] = 0;
    }
}

static void *worker(void *arg) {
    int server_fd = *(int *)arg;

    for (;;) {
        int c = accept4(server_fd, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) continue;

        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        handle_client(c);
    }

    return NULL;
}

static int make_tcp_socket(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        exit(1);
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind tcp");
        exit(1);
    }

    if (listen(fd, 8192) != 0) {
        perror("listen tcp");
        exit(1);
    }

    fprintf(stderr, "listening tcp port=%d\n", PORT);
    return fd;
}

static int make_unix_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket unix");
        exit(1);
    }

    unlink(path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "unix socket path too long: %s\n", path);
        exit(1);
    }

    strcpy(addr.sun_path, path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind unix");
        exit(1);
    }

    chmod(path, 0777);

    if (listen(fd, 8192) != 0) {
        perror("listen unix");
        exit(1);
    }

    fprintf(stderr, "listening unix socket=%s\n", path);
    return fd;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    const char *index_path = argc > 1 ? argv[1] : "/app/index.bin";
    load_index(index_path);

    int threads = 1;
    const char *env_threads = getenv("THREADS");
    if (env_threads) {
        int t = atoi(env_threads);
        if (t > 0) threads = t;
    }

    if (threads > 8) threads = 8;

    const char *sock_path = getenv("SOCKET_PATH");
    int fd = (sock_path && sock_path[0]) ? make_unix_socket(sock_path) : make_tcp_socket();

    fprintf(stderr, "server ready, mode=fast8-rerank64-keepalive, refs=%u, threads=%d\n", g_count, threads);

    pthread_t th[8];

    for (int i = 0; i < threads; i++) {
        pthread_create(&th[i], NULL, worker, &fd);
    }

    for (int i = 0; i < threads; i++) {
        pthread_join(th[i], NULL);
    }

    return 0;
}
