/* The setup page's pieces that need no gateway: the installation as it is
 * kept in flash, form bodies and HTTP Basic authorization. */
#include "harness.h"
#include "install.h"
#include "web.h"

static void the_installation_survives_its_encoding(void)
{
    struct install in = {.away = true, .next_zone = 12, .stale_minutes = 45, .n_rooms = 2, .n_devices = 2}, out;
    in.rooms[0] = (struct install_room){.zone = 1, .setpoint = 1800, .away_setpoint = 1500, .name = "Living room"};
    in.rooms[1] = (struct install_room){.zone = 11, .setpoint = 2150, .name = "Bad\xc3\xa4kamer"}; /* UTF-8 */
    in.devices[0] = (struct install_device){.eui = {1, 2, 3, 4, 5, 0xc5, 0x1b, 0}, .zone = 1, .serial = "VA1234567890"};
    in.devices[1] = (struct install_device){.eui = {6, 7, 8, 9, 10, 0xc5, 0x1b, 0}, .zone = 0, .serial = ""};
    uint8_t blob[INSTALL_MAX_BLOB];
    size_t n = install_encode(&in, blob, sizeof blob);
    CHECK(n > 0 && install_decode(blob, n, &out));
    CHECK(out.away && out.next_zone == 12 && out.n_rooms == 2 && out.rooms[1].zone == 11 && out.rooms[1].setpoint == 2150);
    CHECK(out.rooms[0].away_setpoint == 1500 && out.rooms[1].away_setpoint == 0 && out.stale_minutes == 45);
    CHECK(strcmp(out.rooms[0].name, "Living room") == 0 && strcmp(out.rooms[1].name, "Bad\xc3\xa4kamer") == 0);
    CHECK(out.n_devices == 2 && memcmp(out.devices[1].eui, in.devices[1].eui, 8) == 0);
    CHECK(out.devices[0].zone == 1 && strcmp(out.devices[0].serial, "VA1234567890") == 0 && !out.devices[1].serial[0]);

    /* A blob that is no install leaves the defaults. */
    struct install bad;
    CHECK(!install_decode(blob, 3, &bad) && bad.stale_minutes == INSTALL_STALE_MINUTES_DEFAULT);

    /* Anything but a whole blob is refused, and leaves nothing behind. */
    for (size_t cut = 0; cut < n; cut++)
        CHECK(!install_decode(blob, cut, &out) && out.n_rooms == 0 && out.n_devices == 0);
    blob[n] = 0;
    CHECK(!install_decode(blob, n + 1, &out));
    blob[0] = INSTALL_VERSION + 1;
    CHECK(!install_decode(blob, n, &out)); /* another version */
    CHECK(install_encode(&in, blob, n - 1) == 0);

    /* The full table fits the size flash keeps for it. */
    struct install full = {.n_rooms = INSTALL_MAX_ROOMS, .n_devices = INSTALL_MAX_DEVICES};
    for (size_t i = 0; i < INSTALL_MAX_ROOMS; i++)
        full.rooms[i] = (struct install_room){.zone = (uint8_t)(i + 1), .name = "12345678901234567890123"};
    for (size_t i = 0; i < INSTALL_MAX_DEVICES; i++)
        full.devices[i] = (struct install_device){.serial = "VA1234567890123"};
    CHECK(install_encode(&full, blob, sizeof blob) == INSTALL_MAX_BLOB);
}

/* A later version's blob: an unknown record, a room record with a field
 * after its name, a settings flag this version does not know. The rooms and
 * devices it knows survive. */
static void a_later_versions_installation_still_reads(void)
{
    static const uint8_t blob[] = {
        INSTALL_VERSION,
        INSTALL_TAG_SETTINGS, 5, 0x03, 7, 30, 0, 0x55,          /* flags: away + an unknown bit; one byte more */
        9, 2, 0xaa, 0xbb,                                       /* an unknown record */
        INSTALL_TAG_ROOM, 11, 4, 0x08, 0x07, 0xdc, 0x05, 3, 'D', 'e', 'n', 0x42, 0x42, /* zone 4, 18.00, 15.00, "Den", more */
        INSTALL_TAG_DEVICE, 13, 1, 2, 3, 4, 5, 0xc5, 0x1b, 0, 4, 2, 'S', 'U', 0x99, /* serial "SU": too short */
        INSTALL_TAG_END, 0,
    };
    struct install in;
    CHECK(!install_decode(blob, sizeof blob, &in)); /* a 2-byte serial is no serial */

    static const uint8_t ok[] = {
        INSTALL_VERSION,
        INSTALL_TAG_SETTINGS, 5, 0x03, 7, 30, 0, 0x55,
        9, 2, 0xaa, 0xbb,
        INSTALL_TAG_ROOM, 11, 4, 0x08, 0x07, 0xdc, 0x05, 3, 'D', 'e', 'n', 0x42, 0x42,
        INSTALL_TAG_DEVICE, 15, 1, 2, 3, 4, 5, 0xc5, 0x1b, 0, 4, 4, 'S', 'U', '1', '2', 0x99,
        INSTALL_TAG_END, 0,
    };
    CHECK(install_decode(ok, sizeof ok, &in));
    CHECK(in.away && in.next_zone == 7 && in.stale_minutes == 30);
    CHECK(in.n_rooms == 1 && in.rooms[0].zone == 4 && in.rooms[0].setpoint == 1800 && in.rooms[0].away_setpoint == 1500);
    CHECK(strcmp(in.rooms[0].name, "Den") == 0);
    CHECK(in.n_devices == 1 && in.devices[0].zone == 4 && strcmp(in.devices[0].serial, "SU12") == 0);
}

/* Names go into JSON and MQTT as they are. */
static void a_room_name_needs_no_escaping(void)
{
    CHECK(install_name_ok("Kitchen", 7));
    CHECK(install_name_ok("Kinderkamer 2", 13));
    CHECK(!install_name_ok("", 0));
    CHECK(!install_name_ok("a\"b", 3));
    CHECK(!install_name_ok("a\\b", 3));
    CHECK(!install_name_ok("a\nb", 3));
    CHECK(!install_name_ok("123456789012345678901234", 24));
}

static void reads_a_form_body(void)
{
    char out[32];
    const char *body = "zone=3&name=Living+room%21&x=";
    CHECK(web_form(body, strlen(body), "name", out, sizeof out) == WEB_FIELD_OK && strcmp(out, "Living room!") == 0);
    CHECK(web_form(body, strlen(body), "zone", out, sizeof out) == WEB_FIELD_OK && strcmp(out, "3") == 0);
    CHECK(web_form(body, strlen(body), "x", out, sizeof out) == WEB_FIELD_OK && strcmp(out, "") == 0);
    CHECK(web_form(body, strlen(body), "nam", out, sizeof out) == WEB_FIELD_MISSING);
    CHECK(web_form("name=a%2", 8, "name", out, sizeof out) == WEB_FIELD_BAD);   /* a cut escape */
    CHECK(web_form("name=a%00b", 10, "name", out, sizeof out) == WEB_FIELD_BAD); /* no NULs */
    CHECK(web_form(body, strlen(body), "name", out, 5) == WEB_FIELD_BAD);       /* does not fit */
}

static void checks_basic_authorization(void)
{
    /* admin:secret */
    CHECK(web_auth_ok("Basic YWRtaW46c2VjcmV0", "admin", "secret"));
    CHECK(!web_auth_ok("Basic YWRtaW46c2VjcmV1", "admin", "secret"));
    CHECK(!web_auth_ok("Basic YWRtaW46c2VjcmV0x", "admin", "secret"));
    CHECK(!web_auth_ok("Bearer YWRtaW46c2VjcmV0", "admin", "secret"));
    CHECK(!web_auth_ok(NULL, "admin", "secret"));
    CHECK(!web_auth_ok("Basic YWRtaW46", "admin", "")); /* no password set: nobody */
}

void run_web_tests(void)
{
    the_installation_survives_its_encoding();
    a_later_versions_installation_still_reads();
    a_room_name_needs_no_escaping();
    reads_a_form_body();
    checks_basic_authorization();
}
