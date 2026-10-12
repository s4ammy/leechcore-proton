#include "proton_device.h"

#if !defined(__linux__)
#error "Live Proton process access is supported only on Linux."
#endif

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

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

int proton_process_start_time(pid_t pid, uint64_t *start_time,
                              char *error, size_t error_size)
{
    char path[64];
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    char *close_parenthesis;
    char *cursor;
    char *save = NULL;
    char *token;
    unsigned int field;
    FILE *stream;
    int result = -1;

    if (error && error_size) {
        error[0] = '\0';
    }
    if (pid <= 0 || !start_time) {
        set_error(error, error_size, "valid PID and start-time output are required");
        return -1;
    }
    (void)snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    stream = fopen(path, "re");
    if (!stream) {
        set_error(error, error_size, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    length = getline(&line, &capacity, stream);
    (void)fclose(stream);
    if (length < 0) {
        set_error(error, error_size, "cannot read %s: %s", path, strerror(errno));
        goto done;
    }
    close_parenthesis = strrchr(line, ')');
    if (!close_parenthesis || close_parenthesis[1] != ' ') {
        set_error(error, error_size, "malformed process stat for PID %ld", (long)pid);
        goto done;
    }
    cursor = close_parenthesis + 2;
    for (field = 3, token = strtok_r(cursor, " \t\r\n", &save);
         token;
         field++, token = strtok_r(NULL, " \t\r\n", &save)) {
        if (field == 22) {
            char *end = NULL;
            unsigned long long parsed;
            errno = 0;
            parsed = strtoull(token, &end, 10);
            if (errno == ERANGE || end == token || *end != '\0') {
                break;
            }
            *start_time = (uint64_t)parsed;
            result = 0;
            goto done;
        }
    }
    set_error(error, error_size, "process stat for PID %ld has no start time", (long)pid);
done:
    free(line);
    return result;
}

int proton_process_check_identity(pid_t pid, uint64_t expected_start_time,
                                  char *error, size_t error_size)
{
    uint64_t current_start_time;

    if (kill(pid, 0) != 0 && errno != EPERM) {
        set_error(error, error_size, "target PID %ld no longer exists", (long)pid);
        return -1;
    }
    if (proton_process_start_time(pid, &current_start_time, error, error_size) != 0) {
        return -1;
    }
    if (current_start_time != expected_start_time) {
        set_error(error, error_size,
                  "target PID %ld was reused (process start time changed)", (long)pid);
        return -1;
    }
    return 0;
}

static ssize_t process_vm_read_retry(pid_t pid, const struct iovec *local,
                                     unsigned long local_count,
                                     const struct iovec *remote,
                                     unsigned long remote_count)
{
    ssize_t result;
    do {
        result = process_vm_readv(pid, local, local_count, remote, remote_count, 0);
    } while (result < 0 && errno == EINTR);
    return result;
}

static ssize_t process_vm_write_retry(pid_t pid, const struct iovec *local,
                                      unsigned long local_count,
                                      const struct iovec *remote,
                                      unsigned long remote_count)
{
    ssize_t result;
    do {
        result = process_vm_writev(pid, local, local_count, remote, remote_count, 0);
    } while (result < 0 && errno == EINTR);
    return result;
}

int proton_process_probe(pid_t pid, const ProtonMapList *maps,
                         char *error, size_t error_size)
{
    size_t i;
    int saved_error = EFAULT;

    if (!maps || maps->count == 0) {
        set_error(error, error_size, "no readable mapping is available to probe");
        return -1;
    }
    for (i = 0; i < maps->count; i++) {
        unsigned char value;
        struct iovec local = {&value, 1};
        struct iovec remote = {(void *)(uintptr_t)maps->items[i].start, 1};
        ssize_t bytes = process_vm_read_retry(pid, &local, 1, &remote, 1);
        if (bytes == 1) {
            return 0;
        }
        if (bytes < 0) {
            saved_error = errno;
        }
    }
    set_error(error, error_size,
              "cannot read PID %ld with process_vm_readv: %s. Check same-user/parent "
              "access, kernel.yama.ptrace_scope, CAP_SYS_PTRACE or root, Steam Flatpak "
              "and PID namespaces, and use the Linux host PID",
              (long)pid, strerror(saved_error));
    return -1;
}

typedef ssize_t (*ProcessVmOperation)(pid_t, const struct iovec *, unsigned long,
                                      const struct iovec *, unsigned long);

static size_t process_batch(pid_t pid, ProtonIoRequest *requests,
                            size_t request_count, ProcessVmOperation operation)
{
    struct iovec *local = NULL;
    struct iovec *remote = NULL;
    long configured_max;
    size_t iov_max;
    size_t position = 0;
    size_t completed = 0;
    size_t i;

    if (!requests || !operation) {
        return 0;
    }
    for (i = 0; i < request_count; i++) {
        requests[i].complete = false;
    }
    configured_max = sysconf(_SC_IOV_MAX);
    iov_max = configured_max > 0 ? (size_t)configured_max : 1024;
    if (iov_max > 65536) {
        iov_max = 65536;
    }
    local = calloc(iov_max, sizeof(*local));
    remote = calloc(iov_max, sizeof(*remote));
    if (!local || !remote) {
        goto done;
    }
    while (position < request_count) {
        size_t count = request_count - position;
        size_t batch_end;
        size_t total = 0;
        ssize_t transferred;
        size_t remaining;

        if (count > iov_max) {
            count = iov_max;
        }
        for (i = 0; i < count; i++) {
            ProtonIoRequest *request = &requests[position + i];
            if (!request->local_buffer || request->length == 0 ||
                request->remote_address > UINTPTR_MAX ||
                total > (size_t)SSIZE_MAX - request->length) {
                count = i;
                break;
            }
            local[i].iov_base = request->local_buffer;
            local[i].iov_len = request->length;
            remote[i].iov_base = (void *)(uintptr_t)request->remote_address;
            remote[i].iov_len = request->length;
            total += request->length;
        }
        if (count == 0) {
            position++;
            continue;
        }
        batch_end = position + count;
        transferred = operation(pid, local, (unsigned long)count,
                                remote, (unsigned long)count);
        if (transferred < 0) {
            position++;
            continue;
        }
        remaining = (size_t)transferred;
        for (i = position; i < batch_end; i++) {
            if (remaining >= requests[i].length) {
                remaining -= requests[i].length;
                requests[i].complete = true;
                completed++;
                position = i + 1;
            } else {
                position = i + 1;
                break;
            }
        }
    }
done:
    free(local);
    free(remote);
    return completed;
}

size_t proton_process_read_batch(pid_t pid, ProtonIoRequest *requests,
                                 size_t request_count)
{
    return process_batch(pid, requests, request_count, process_vm_read_retry);
}

size_t proton_process_write_batch(pid_t pid, ProtonIoRequest *requests,
                                  size_t request_count)
{
    return process_batch(pid, requests, request_count, process_vm_write_retry);
}
