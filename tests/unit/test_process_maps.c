#include "proton_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        return 1; \
    } \
} while (0)

static int parse_text(const char *text, ProtonMapList *maps, char *error, size_t error_size)
{
    FILE *stream = fmemopen((void *)text, strlen(text), "r");
    int result;
    if (!stream) {
        return -1;
    }
    result = proton_maps_parse_stream(stream, maps, error, error_size);
    fclose(stream);
    return result;
}

static int expect_rejected(const char *text)
{
    ProtonMapList maps;
    char error[256];
    int rejected;
    proton_maps_init(&maps);
    rejected = parse_text(text, &maps, error, sizeof(error)) != 0;
    proton_maps_free(&maps);
    return rejected;
}

int main(void)
{
    static const char valid_maps[] =
        "00006000-00007000 r--p 00000000 08:01 12 /tmp/gone (deleted)\n"
        "00001000-00002000 rw-p 00000000 00:00 0\n"
        "00002000-00003000 ---p 00000000 00:00 0 [guard]\n"
        "00004000-00005000 r-xp 00001000 08:01 11 /tmp/a file\n"
        "00003000-00004000 r-xp 00000000 08:01 11 /tmp/a file\n"
        "00007000-00008000 rw-p 00000000 00:00 0 [heap]\n"
        "00008000-00009000 rw-p 00000000 00:00 0 [stack]\n"
        "00009000-0000a000 rw-s 00000000 00:01 44 /dev/shm/proton test\n";
    ProtonMapList maps;
    char error[256];

    proton_maps_init(&maps);
    CHECK(parse_text(valid_maps, &maps, error, sizeof(error)) == 0);
    CHECK(maps.count == 6);
    CHECK(maps.items[0].start == 0x1000 && maps.items[0].end == 0x2000);
    CHECK(maps.items[0].writable && !maps.items[0].executable);
    CHECK(strcmp(maps.items[0].path, "") == 0);
    CHECK(maps.items[1].start == 0x3000 && maps.items[1].end == 0x5000);
    CHECK(!maps.items[1].writable && maps.items[1].executable);
    CHECK(maps.items[1].offset == 0);
    CHECK(strcmp(maps.items[1].path, "/tmp/a file") == 0);
    CHECK(strcmp(maps.items[2].path, "/tmp/gone (deleted)") == 0);
    CHECK(strcmp(maps.items[3].path, "[heap]") == 0);
    CHECK(strcmp(maps.items[4].path, "[stack]") == 0);
    CHECK(!maps.items[5].private_mapping);
    CHECK(strcmp(maps.items[5].path, "/dev/shm/proton test") == 0);
    proton_maps_free(&maps);

    CHECK(expect_rejected("1001-2000 r--p 00000000 00:00 0\n"));
    CHECK(expect_rejected("3000-2000 r--p 00000000 00:00 0\n"));
    CHECK(expect_rejected("1000-3000 r--p 00000000 00:00 0\n"
                          "2000-4000 r--p 00000000 00:00 0\n"));
    CHECK(expect_rejected("1000-2000 rw?x 00000000 00:00 0\n"));
    CHECK(expect_rejected("1000-2000 r--p xyz 00:00 0\n"));
    CHECK(expect_rejected("\n"));

    puts("test_process_maps: PASS");
    return 0;
}
