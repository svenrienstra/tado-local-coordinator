/* The setup page's pieces that need no gateway: the installation as it is
 * kept in flash, form bodies and HTTP Basic authorization. */
#include "harness.h"
#include "install.h"
#include "web.h"

static void the_installation_survives_its_encoding(void)
{
    struct install in = {.n_rooms = 2, .n_devices = 2}, out;
    in.rooms[0] = (struct install_room){.zone = 1, .setpoint = 1800, .name = "Living room"};
    in.rooms[1] = (struct install_room){.zone = 11, .setpoint = 2150, .name = "Bad\xc3\xa4kamer"}; /* UTF-8 */
    in.devices[0] = (struct install_device){.eui = {1, 2, 3, 4, 5, 0xc5, 0x1b, 0}, .zone = 1, .serial = "VA1234567890"};
    in.devices[1] = (struct install_device){.eui = {6, 7, 8, 9, 10, 0xc5, 0x1b, 0}, .zone = 0, .serial = ""};
    uint8_t blob[INSTALL_MAX_BLOB];
    size_t n = install_encode(&in, blob, sizeof blob);
    CHECK(n > 0 && install_decode(blob, n, &out));
    CHECK(out.n_rooms == 2 && out.rooms[1].zone == 11 && out.rooms[1].setpoint == 2150);
    CHECK(strcmp(out.rooms[0].name, "Living room") == 0 && strcmp(out.rooms[1].name, "Bad\xc3\xa4kamer") == 0);
    CHECK(out.n_devices == 2 && memcmp(out.devices[1].eui, in.devices[1].eui, 8) == 0);
    CHECK(out.devices[0].zone == 1 && strcmp(out.devices[0].serial, "VA1234567890") == 0 && !out.devices[1].serial[0]);

    /* Anything but a whole blob is refused, and leaves nothing behind. */
    for (size_t cut = 0; cut < n; cut++)
        CHECK(!install_decode(blob, cut, &out) && out.n_rooms == 0 && out.n_devices == 0);
    blob[n] = 0;
    CHECK(!install_decode(blob, n + 1, &out));
    blob[0] = 2;
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
    CHECK(web_form(body, strlen(body), "name", out, sizeof out) && strcmp(out, "Living room!") == 0);
    CHECK(web_form(body, strlen(body), "zone", out, sizeof out) && strcmp(out, "3") == 0);
    CHECK(web_form(body, strlen(body), "x", out, sizeof out) && strcmp(out, "") == 0);
    CHECK(!web_form(body, strlen(body), "nam", out, sizeof out));
    CHECK(!web_form("name=a%2", 8, "name", out, sizeof out));   /* a cut escape */
    CHECK(!web_form("name=a%00b", 10, "name", out, sizeof out)); /* no NULs */
    CHECK(!web_form(body, strlen(body), "name", out, 5));       /* does not fit */
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
    a_room_name_needs_no_escaping();
    reads_a_form_body();
    checks_basic_authorization();
}
