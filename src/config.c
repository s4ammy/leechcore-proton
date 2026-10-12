#include "proton_device.h"

#include <errno.h>
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

static int parse_decimal(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;
    const unsigned char *cursor = (const unsigned char *)text;

    if (!text || !*text) {
        return -1;
    }
    while (*cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return -1;
        }
        cursor++;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || !end || *end != '\0') {
        return -1;
    }
    *value = (uint64_t)parsed;
    return 0;
}

int proton_parse_connection(const char *connection, ProtonConfig *config,
                            char *error, size_t error_size)
{
    static const char prefix[] = "proton://";
    char *copy = NULL;
    char *cursor;
    bool saw_pid = false;
    bool saw_ro = false;
    int result = -1;

    if (error && error_size) {
        error[0] = '\0';
    }
    if (!connection || !config) {
        set_error(error, error_size, "connection and output configuration are required");
        return -1;
    }
    config->pid = 0;
    config->read_only = true;
    if (strncmp(connection, prefix, sizeof(prefix) - 1) != 0) {
        set_error(error, error_size, "connection must start with proton://");
        return -1;
    }
    if (connection[sizeof(prefix) - 1] == '\0') {
        set_error(error, error_size, "pid is required");
        return -1;
    }
    copy = strdup(connection + sizeof(prefix) - 1);
    if (!copy) {
        set_error(error, error_size, "out of memory while parsing connection");
        return -1;
    }
    cursor = copy;
    while (cursor && *cursor) {
        char *next = strpbrk(cursor, ",;");
        char *equals;
        uint64_t number;

        if (next) {
            *next++ = '\0';
            if (*next == '\0') {
                set_error(error, error_size, "empty trailing connection parameter");
                goto done;
            }
        }
        if (*cursor == '\0' || !(equals = strchr(cursor, '=')) ||
            equals == cursor || equals[1] == '\0' || strchr(equals + 1, '=')) {
            set_error(error, error_size, "malformed connection parameter '%s'", cursor);
            goto done;
        }
        *equals++ = '\0';
        if (strcmp(cursor, "pid") == 0) {
            if (saw_pid) {
                set_error(error, error_size, "duplicate pid parameter");
                goto done;
            }
            if (parse_decimal(equals, &number) != 0 || number == 0 || number > INT_MAX) {
                set_error(error, error_size, "pid must be a positive decimal Linux PID");
                goto done;
            }
            config->pid = (pid_t)number;
            saw_pid = true;
        } else if (strcmp(cursor, "ro") == 0) {
            if (saw_ro) {
                set_error(error, error_size, "duplicate ro parameter");
                goto done;
            }
            if (parse_decimal(equals, &number) != 0 || number > 1) {
                set_error(error, error_size, "ro must be exactly 0 or 1");
                goto done;
            }
            config->read_only = number != 0;
            saw_ro = true;
        } else {
            set_error(error, error_size, "unknown connection parameter '%s'", cursor);
            goto done;
        }
        cursor = next;
    }
    if (!saw_pid) {
        set_error(error, error_size, "pid is required");
        goto done;
    }
    result = 0;
done:
    free(copy);
    return result;
}
