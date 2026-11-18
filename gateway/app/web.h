#ifndef WEB_H
#define WEB_H

#include <stdbool.h>
#include <stddef.h>

#include "gateway.h"

/* The setup page's JSON API (docs/PLAN.md 3.9), without the HTTP server: the
 * ESP32 build hands each request to web_handle on the gateway's task.
 *
 *   GET  /api/state                   pair window, rooms, devices
 *   POST /api/pair                    opens a pair window
 *   POST /api/pair/close
 *   POST /api/rooms                   name=<name>: a new room
 *   POST /api/rooms/<zone>            name=<name>: renames it;
 *                                     setpoint=<C>: sets its setpoint
 *   POST /api/rooms/<zone>/delete     its devices go to no room
 *   POST /api/devices/<eui>           zone=<zone>: puts it in a room, 0: none
 *
 * Bodies are form-encoded (application/x-www-form-urlencoded); answers are
 * JSON, {"error": "..."} with a 4xx status when refused. */

#define WEB_MAX_BODY 256

/* Returns the HTTP status; the JSON answer is in out (NUL-terminated). */
int web_handle(struct gateway *g, const char *method, const char *path, const char *body, size_t body_len,
               char *out, size_t cap);

/* The value of key in a form-encoded body, decoded; false when it is
 * missing, malformed or does not fit cap. */
bool web_form(const char *body, size_t len, const char *key, char *out, size_t cap);

/* Whether an Authorization header is HTTP Basic for user and password. */
bool web_auth_ok(const char *header, const char *user, const char *password);

#endif
