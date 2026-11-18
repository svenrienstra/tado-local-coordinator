#include "harness.h"

#include <stdlib.h>

int failures, checks;

static struct vec vecs[96];
static size_t nvecs;

size_t unhex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex) / 2;
    if (n > cap)
        return 0;
    for (size_t i = 0; i < n; i++)
        sscanf(&hex[2 * i], "%2hhx", &out[i]);
    return n;
}

void load_vectors(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        exit(2);
    }
    char line[2048], hex[2048];
    while (fgets(line, sizeof line, f)) {
        struct vec v;
        if (line[0] == '#' || sscanf(line, "%63s %2047s", v.name, hex) != 2)
            continue;
        if (nvecs == sizeof vecs / sizeof vecs[0] || strlen(hex) / 2 > sizeof v.data) {
            fprintf(stderr, "%s: %s does not fit the test harness (struct vec, vecs[])\n", path, v.name);
            exit(2);
        }
        v.len = unhex(hex, v.data, sizeof v.data);
        vecs[nvecs++] = v;
    }
    fclose(f);
}

const struct vec *V(const char *name)
{
    for (size_t i = 0; i < nvecs; i++)
        if (strcmp(vecs[i].name, name) == 0)
            return &vecs[i];
    fprintf(stderr, "missing vector %s\n", name);
    exit(2);
}
