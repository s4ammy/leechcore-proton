#include "proton_device.h"

#if !defined(__linux__)
#error "Proton /proc mapping discovery is supported only on Linux."
#endif

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list args;

    if (!error || error_size == 0) {
        return;
    }
    va_start(args, format);
    (void)vsnprintf(error, error_size, format, args);
    va_end(args);
}

void proton_maps_init(ProtonMapList *maps)
{
    if (maps) {
        memset(maps, 0, sizeof(*maps));
    }
}

void proton_maps_free(ProtonMapList *maps)
{
    size_t i;

    if (!maps) {
        return;
    }
    for (i = 0; i < maps->count; i++) {
        free(maps->items[i].path);
    }
    free(maps->items);
    memset(maps, 0, sizeof(*maps));
}

static int parse_u64(char **cursor, int base, char delimiter, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (!cursor || !*cursor || !**cursor) {
        return -1;
    }
    errno = 0;
    parsed = strtoull(*cursor, &end, base);
    if (errno == ERANGE || end == *cursor || (delimiter && *end != delimiter)) {
        return -1;
    }
    *value = (uint64_t)parsed;
    *cursor = delimiter ? end + 1 : end;
    return 0;
}

static void skip_spaces(char **cursor)
{
    while (**cursor == ' ' || **cursor == '\t') {
        (*cursor)++;
    }
}

static int append_map(ProtonMapList *maps, const ProtonVma *map)
{
    ProtonVma *grown;
    size_t capacity;

    if (maps->count == maps->capacity) {
        capacity = maps->capacity ? maps->capacity * 2 : 32;
        if (capacity < maps->capacity || capacity > SIZE_MAX / sizeof(*maps->items)) {
            return -1;
        }
        grown = realloc(maps->items, capacity * sizeof(*maps->items));
        if (!grown) {
            return -1;
        }
        maps->items = grown;
        maps->capacity = capacity;
    }
    maps->items[maps->count++] = *map;
    return 0;
}

static int parse_line(char *line, ProtonVma *map, char *error, size_t error_size)
{
    char *cursor = line;
    char *path;
    uint64_t major;
    uint64_t minor;

    memset(map, 0, sizeof(*map));
    map->physical_start = PROTON_PHYS_UNASSIGNED;
    if (parse_u64(&cursor, 16, '-', &map->start) != 0 ||
        parse_u64(&cursor, 16, ' ', &map->end) != 0) {
        set_error(error, error_size, "invalid address range");
        return -1;
    }
    skip_spaces(&cursor);
    if (strlen(cursor) < 5 ||
        (cursor[0] != 'r' && cursor[0] != '-') ||
        (cursor[1] != 'w' && cursor[1] != '-') ||
        (cursor[2] != 'x' && cursor[2] != '-') ||
        (cursor[3] != 'p' && cursor[3] != 's') ||
        !isspace((unsigned char)cursor[4])) {
        set_error(error, error_size, "invalid permissions field");
        return -1;
    }
    map->readable = cursor[0] == 'r';
    map->writable = cursor[1] == 'w';
    map->executable = cursor[2] == 'x';
    map->private_mapping = cursor[3] == 'p';
    cursor += 5;
    skip_spaces(&cursor);
    if (parse_u64(&cursor, 16, ' ', &map->offset) != 0) {
        set_error(error, error_size, "invalid file offset");
        return -1;
    }
    skip_spaces(&cursor);
    if (parse_u64(&cursor, 16, ':', &major) != 0 ||
        parse_u64(&cursor, 16, ' ', &minor) != 0 ||
        major > UINT_MAX || minor > UINT_MAX) {
        set_error(error, error_size, "invalid device field");
        return -1;
    }
    map->device_major = (unsigned int)major;
    map->device_minor = (unsigned int)minor;
    skip_spaces(&cursor);
    if (parse_u64(&cursor, 10, 0, &map->inode) != 0 ||
        (*cursor != '\0' && !isspace((unsigned char)*cursor))) {
        set_error(error, error_size, "invalid inode field");
        return -1;
    }
    skip_spaces(&cursor);
    path = cursor;
    map->path = strdup(path);
    if (!map->path) {
        set_error(error, error_size, "out of memory for pathname");
        return -1;
    }
    if (map->start >= map->end ||
        (map->start & (PROTON_PAGE_SIZE - 1)) ||
        (map->end & (PROTON_PAGE_SIZE - 1))) {
        set_error(error, error_size, "mapping range is reversed or not page aligned");
        free(map->path);
        map->path = NULL;
        return -1;
    }
    return 0;
}

static int compare_maps(const void *left, const void *right)
{
    const ProtonVma *a = left;
    const ProtonVma *b = right;

    if (a->start < b->start) {
        return -1;
    }
    if (a->start > b->start) {
        return 1;
    }
    return 0;
}

static bool maps_are_mergeable(const ProtonVma *a, const ProtonVma *b)
{
    uint64_t length = a->end - a->start;

    return a->end == b->start &&
           a->offset <= UINT64_MAX - length &&
           a->offset + length == b->offset &&
           a->readable == b->readable &&
           a->writable == b->writable &&
           a->executable == b->executable &&
           a->private_mapping == b->private_mapping &&
           a->device_major == b->device_major &&
           a->device_minor == b->device_minor &&
           a->inode == b->inode &&
           strcmp(a->path, b->path) == 0;
}

static int normalize_maps(ProtonMapList *maps, char *error, size_t error_size)
{
    size_t source;
    size_t destination = 0;

    qsort(maps->items, maps->count, sizeof(*maps->items), compare_maps);
    for (source = 0; source < maps->count; source++) {
        ProtonVma *current = &maps->items[source];

        if (destination && maps->items[destination - 1].end > current->start) {
            set_error(error, error_size,
                      "overlapping mappings at 0x%" PRIx64, current->start);
            return -1;
        }
        if (destination && maps_are_mergeable(&maps->items[destination - 1], current)) {
            maps->items[destination - 1].end = current->end;
            free(current->path);
            current->path = NULL;
            continue;
        }
        if (destination != source) {
            maps->items[destination] = *current;
            memset(current, 0, sizeof(*current));
        }
        destination++;
    }
    maps->count = destination;
    return 0;
}

int proton_maps_parse_stream(FILE *stream, ProtonMapList *maps,
                             char *error, size_t error_size)
{
    char *line = NULL;
    size_t line_capacity = 0;
    ssize_t line_length;
    size_t line_number = 0;
    ProtonMapList parsed;
    int result = -1;

    if (error && error_size) {
        error[0] = '\0';
    }
    if (!stream || !maps) {
        set_error(error, error_size, "maps stream and output are required");
        return -1;
    }
    proton_maps_init(&parsed);
    while ((line_length = getline(&line, &line_capacity, stream)) >= 0) {
        ProtonVma map;
        char line_error[160];

        line_number++;
        while (line_length > 0 &&
               (line[line_length - 1] == '\n' || line[line_length - 1] == '\r')) {
            line[--line_length] = '\0';
        }
        if (line_length == 0) {
            set_error(error, error_size, "malformed empty line at line %zu", line_number);
            goto done;
        }
        if (parse_line(line, &map, line_error, sizeof(line_error)) != 0) {
            set_error(error, error_size, "maps line %zu: %s", line_number, line_error);
            goto done;
        }
        if (!map.readable) {
            free(map.path);
            continue;
        }
        if (append_map(&parsed, &map) != 0) {
            free(map.path);
            set_error(error, error_size, "out of memory while reading maps");
            goto done;
        }
    }
    if (ferror(stream)) {
        set_error(error, error_size, "failed while reading maps: %s", strerror(errno));
        goto done;
    }
    if (parsed.count == 0) {
        set_error(error, error_size, "process has no readable mappings");
        goto done;
    }
    if (normalize_maps(&parsed, error, error_size) != 0) {
        goto done;
    }
    proton_maps_free(maps);
    *maps = parsed;
    memset(&parsed, 0, sizeof(parsed));
    result = 0;
done:
    free(line);
    proton_maps_free(&parsed);
    return result;
}

int proton_maps_load(pid_t pid, ProtonMapList *maps,
                     char *error, size_t error_size)
{
    char path[64];
    FILE *stream;
    int result;

    if (pid <= 0) {
        set_error(error, error_size, "invalid process PID");
        return -1;
    }
    (void)snprintf(path, sizeof(path), "/proc/%ld/maps", (long)pid);
    stream = fopen(path, "re");
    if (!stream) {
        set_error(error, error_size, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    result = proton_maps_parse_stream(stream, maps, error, error_size);
    if (fclose(stream) != 0 && result == 0) {
        set_error(error, error_size, "cannot close %s: %s", path, strerror(errno));
        result = -1;
    }
    return result;
}
