#ifndef HARNESS_H
#define HARNESS_H

/* The test harness shared by the gateway's test files. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int failures, checks;

#define CHECK(cond)                                                                \
    do {                                                                           \
        checks++;                                                                  \
        if (!(cond)) {                                                             \
            failures++;                                                            \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)

#define CHECK_BYTES(got, got_len, want, want_len)                                  \
    CHECK((got_len) == (want_len) && memcmp((got), (want), (want_len)) == 0)

#define CHECK_VEC(got, got_len, name)                                              \
    do {                                                                           \
        const struct vec *want_ = V(name);                                         \
        CHECK_BYTES(got, got_len, want_->data, want_->len);                        \
    } while (0)

/* A named byte string from golden/vectors.txt or golden/host_vectors.txt. */
struct vec {
    char name[64];
    uint8_t data[512];
    size_t len;
};

const struct vec *V(const char *name);
size_t unhex(const char *hex, uint8_t *out, size_t cap);

void run_session_tests(void);

#endif
