#ifndef COAP_H
#define COAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CoAP messages (RFC 7252). A decoded message points into the bytes it came
 * from; an encoded one sorts its options by number, stably, so repeated
 * options keep their order. */

enum coap_type { COAP_CON = 0, COAP_NON = 1, COAP_ACK = 2, COAP_RST = 3 };

#define COAP_CODE(cls, detail) ((uint8_t)((cls) << 5 | (detail)))
#define COAP_GET  COAP_CODE(0, 1)
#define COAP_POST COAP_CODE(0, 2)
#define COAP_PUT  COAP_CODE(0, 3)

#define COAP_OPT_ETAG           4
#define COAP_OPT_URI_PORT       7
#define COAP_OPT_URI_PATH       11
#define COAP_OPT_CONTENT_FORMAT 12
#define COAP_OPT_URI_QUERY      15
#define COAP_OPT_BLOCK2         23
#define COAP_OPT_SESSION        2048 /* tado session id; above every standard option */

#define COAP_MAX_OPTIONS 16

struct coap_option {
    uint16_t number;
    uint16_t len;
    const uint8_t *value;
};

struct coap_msg {
    enum coap_type type;
    uint8_t code;
    uint16_t mid;
    uint8_t tkl;
    uint8_t token[8];
    size_t n_options;
    struct coap_option options[COAP_MAX_OPTIONS];
    const uint8_t *payload;
    size_t payload_len;
};

/* False for anything but a well-formed CoAP v1 message with at most
 * COAP_MAX_OPTIONS options. */
bool coap_decode(const uint8_t *b, size_t len, struct coap_msg *m);

/* Returns the encoded length, or 0 when it does not fit cap. */
size_t coap_encode(const struct coap_msg *m, uint8_t *out, size_t cap);

/* Appends an option; false when the message is full. */
bool coap_add_option(struct coap_msg *m, uint16_t number, const void *value, size_t len);

/* Appends one Uri-Path option per '/'-separated segment of path. */
bool coap_add_path(struct coap_msg *m, const char *path);

/* The first option of that number, or NULL. */
const struct coap_option *coap_find(const struct coap_msg *m, uint16_t number);

/* The option's value as an unsigned big-endian integer (0 for an empty one). */
uint32_t coap_uint(const struct coap_option *o);

/* Writes the Uri-Path segments joined by '/' as a C string; false when it
 * does not fit cap (out then holds a truncated path). */
bool coap_path(const struct coap_msg *m, char *out, size_t cap);

/* Whether the Uri-Path equals path ('/'-separated), segment by segment. */
bool coap_path_is(const struct coap_msg *m, const char *path);

#endif
