#ifndef HARNESS_H
#define HARNESS_H

/* The test harness of the firmware's and the gateway's host tests: checks
 * and the golden vectors (golden/vectors.txt, golden/host_vectors.txt). */
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

/* Checks bytes against the whole golden vector of that name. */
#define CHECK_VEC(got, got_len, name)                                              \
    do {                                                                           \
        const struct vec *want_ = V(name);                                         \
        CHECK_BYTES(got, got_len, want_->data, want_->len);                        \
    } while (0)

/* A named byte string from a vectors file. */
struct vec {
    char name[64];
    uint8_t data[512];
    size_t len;
};

/* Adds a vectors file ("name hex" lines, # comments). Exits with an error for
 * a missing file, or one that does not fit the harness. */
void load_vectors(const char *path);

/* The vector of that name; exits with an error when there is none. */
const struct vec *V(const char *name);

/* Decodes hex into out; returns the byte count, 0 when it does not fit cap. */
size_t unhex(const char *hex, uint8_t *out, size_t cap);

#endif
