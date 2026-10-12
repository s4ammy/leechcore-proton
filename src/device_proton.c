#include "proton_device.h"

#if !defined(__linux__)
#error "The Proton process connector is supported only on Linux."
#endif

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <leechcore_device.h>

typedef struct ProtonDeviceContext {
    ProtonConfig config;
    ProtonMapList maps;
    ProtonPaging paging;
    uint64_t process_start_time;
    _Atomic uint64_t read_requests;
    _Atomic uint64_t read_failures;
    _Atomic uint64_t write_requests;
    _Atomic uint64_t write_failures;
} ProtonDeviceContext;

static bool range_contains(uint64_t start, uint64_t end,
                           uint64_t address, size_t length)
{
    if (address < start || end < start || (uint64_t)length > end - start) {
        return false;
    }
    return address - start <= (end - start) - (uint64_t)length;
}

static bool valid_scatter(const MEM_SCATTER *memory)
{
    uint64_t page_offset;

    if (!memory || memory->f || MEM_SCATTER_ADDR_ISINVALID(memory) ||
        !memory->pb || memory->cb == 0 || memory->cb > PROTON_PAGE_SIZE) {
        return false;
    }
    page_offset = memory->qwA & (PROTON_PAGE_SIZE - 1);
    return (uint64_t)memory->cb <= PROTON_PAGE_SIZE - page_offset;
}

static bool calculate_request_capacity(size_t memory_count,
                                       size_t request_size,
                                       size_t *capacity)
{
    if (!capacity || request_size == 0 || memory_count > SIZE_MAX / 2) {
        return false;
    }
    memory_count *= 2;
    if (memory_count > SIZE_MAX / request_size) {
        return false;
    }
    *capacity = memory_count;
    return true;
}

static void proton_read_scatter(PLC_CONTEXT lc, DWORD memory_count,
                                PPMEM_SCATTER memories)
{
    ProtonDeviceContext *context = (ProtonDeviceContext *)lc->hDevice;
    ProtonIoRequest *requests = NULL;
    DWORD *owners = NULL;
    DWORD *required = NULL;
    DWORD *completed = NULL;
    size_t request_count = 0;
    size_t capacity;
    DWORD i;
    char error[256];

    if (!context || !memories || memory_count == 0) {
        return;
    }
    atomic_fetch_add(&context->read_requests, memory_count);
    if (proton_process_check_identity(context->config.pid,
                                      context->process_start_time,
                                      error, sizeof(error)) != 0) {
        atomic_fetch_add(&context->read_failures, memory_count);
        lcprintf(lc, "PROTON: read rejected: %s\n", error);
        return;
    }
    if (!calculate_request_capacity((size_t)memory_count, sizeof(*requests),
                                    &capacity)) {
        atomic_fetch_add(&context->read_failures, memory_count);
        return;
    }
    requests = calloc(capacity, sizeof(*requests));
    owners = calloc(capacity, sizeof(*owners));
    required = calloc(memory_count, sizeof(*required));
    completed = calloc(memory_count, sizeof(*completed));
    if (!requests || !owners || !required || !completed) {
        atomic_fetch_add(&context->read_failures, memory_count);
        goto done;
    }
    for (i = 0; i < memory_count; i++) {
        PMEM_SCATTER memory = memories[i];
        ProtonTranslation segments[2];
        size_t segment_count = 0;
        size_t segment;
        size_t buffer_offset = 0;

        if (!valid_scatter(memory)) {
            continue;
        }
        if (range_contains(context->paging.table_start, context->paging.table_end,
                           memory->qwA, memory->cb)) {
            if (proton_paging_read(&context->paging, memory->qwA,
                                   memory->pb, memory->cb) == 0) {
                memory->f = true;
            }
            continue;
        }
        if (proton_translate_split(&context->maps, memory->qwA, memory->cb,
                                   segments, 2, &segment_count) != 0) {
            continue;
        }
        required[i] = (DWORD)segment_count;
        for (segment = 0; segment < segment_count; segment++) {
            requests[request_count].remote_address = segments[segment].target_address;
            requests[request_count].local_buffer = memory->pb + buffer_offset;
            requests[request_count].length = segments[segment].length;
            owners[request_count] = i;
            buffer_offset += segments[segment].length;
            request_count++;
        }
    }
    (void)proton_process_read_batch(context->config.pid, requests, request_count);
    for (i = 0; i < request_count; i++) {
        if (requests[i].complete) {
            completed[owners[i]]++;
        }
    }
    for (i = 0; i < memory_count; i++) {
        if (!memories[i]->f && required[i] && completed[i] == required[i]) {
            memories[i]->f = true;
        }
        if (!memories[i]->f) {
            atomic_fetch_add(&context->read_failures, 1);
        }
    }
done:
    free(completed);
    free(required);
    free(owners);
    free(requests);
}

static void proton_write_scatter(PLC_CONTEXT lc, DWORD memory_count,
                                 PPMEM_SCATTER memories)
{
    ProtonDeviceContext *context = (ProtonDeviceContext *)lc->hDevice;
    ProtonIoRequest *requests = NULL;
    DWORD *owners = NULL;
    DWORD *required = NULL;
    DWORD *completed = NULL;
    size_t request_count = 0;
    size_t capacity;
    DWORD i;
    char error[256];

    if (!context || context->config.read_only || !memories || memory_count == 0) {
        return;
    }
    atomic_fetch_add(&context->write_requests, memory_count);
    if (proton_process_check_identity(context->config.pid,
                                      context->process_start_time,
                                      error, sizeof(error)) != 0) {
        atomic_fetch_add(&context->write_failures, memory_count);
        lcprintf(lc, "PROTON: write rejected: %s\n", error);
        return;
    }
    if (!calculate_request_capacity((size_t)memory_count, sizeof(*requests),
                                    &capacity)) {
        atomic_fetch_add(&context->write_failures, memory_count);
        return;
    }
    requests = calloc(capacity, sizeof(*requests));
    owners = calloc(capacity, sizeof(*owners));
    required = calloc(memory_count, sizeof(*required));
    completed = calloc(memory_count, sizeof(*completed));
    if (!requests || !owners || !required || !completed) {
        atomic_fetch_add(&context->write_failures, memory_count);
        goto done;
    }
    for (i = 0; i < memory_count; i++) {
        PMEM_SCATTER memory = memories[i];
        ProtonTranslation segments[2];
        size_t segment_count = 0;
        size_t segment;
        size_t buffer_offset = 0;
        bool writable = true;

        if (!valid_scatter(memory) ||
            range_contains(context->paging.table_start, context->paging.table_end,
                           memory->qwA, memory->cb) ||
            proton_translate_split(&context->maps, memory->qwA, memory->cb,
                                   segments, 2, &segment_count) != 0) {
            continue;
        }
        for (segment = 0; segment < segment_count; segment++) {
            if (!segments[segment].writable) {
                writable = false;
            }
        }
        if (!writable) {
            continue;
        }
        required[i] = (DWORD)segment_count;
        for (segment = 0; segment < segment_count; segment++) {
            requests[request_count].remote_address = segments[segment].target_address;
            requests[request_count].local_buffer = memory->pb + buffer_offset;
            requests[request_count].length = segments[segment].length;
            owners[request_count] = i;
            buffer_offset += segments[segment].length;
            request_count++;
        }
    }
    (void)proton_process_write_batch(context->config.pid, requests, request_count);
    for (i = 0; i < request_count; i++) {
        if (requests[i].complete) {
            completed[owners[i]]++;
        }
    }
    for (i = 0; i < memory_count; i++) {
        if (required[i] && completed[i] == required[i]) {
            memories[i]->f = true;
        }
        if (!memories[i]->f) {
            atomic_fetch_add(&context->write_failures, 1);
        }
    }
done:
    free(completed);
    free(required);
    free(owners);
    free(requests);
}

static BOOL proton_get_option(PLC_CONTEXT lc, QWORD option, PQWORD value)
{
    (void)lc;
    if (!value) {
        return false;
    }
    *value = 0;
    switch (option) {
        case LC_OPT_MEMORYINFO_ARCH:
            *value = LC_ARCH_X64;
            return true;
        case LC_OPT_MEMORYINFO_OS_DTB:
            *value = PROTON_DTB_PHYSICAL;
            return true;
        case LC_OPT_MEMORYINFO_VALID:
            return true;
        default:
            return false;
    }
}

static VOID proton_close(PLC_CONTEXT lc)
{
    ProtonDeviceContext *context = (ProtonDeviceContext *)lc->hDevice;

    if (!context) {
        return;
    }
    lcprintfv(lc,
              "PROTON: closing PID %ld; reads=%" PRIu64 " failed=%" PRIu64
              " writes=%" PRIu64 " failed=%" PRIu64 "\n",
              (long)context->config.pid,
              atomic_load(&context->read_requests),
              atomic_load(&context->read_failures),
              atomic_load(&context->write_requests),
              atomic_load(&context->write_failures));
    lc->hDevice = NULL;
    proton_paging_free(&context->paging);
    proton_maps_free(&context->maps);
    free(context);
}

_Success_(return) EXPORTED_FUNCTION
BOOL LcPluginCreate(PLC_CONTEXT lc, PPLC_CONFIG_ERRORINFO error_info)
{
    ProtonDeviceContext *context = NULL;
    char error[512];

    if (error_info) {
        *error_info = NULL;
    }
    if (!lc || lc->version != LC_CONTEXT_VERSION) {
        return false;
    }
    context = calloc(1, sizeof(*context));
    if (!context) {
        lcprintf(lc, "PROTON: out of memory creating device context\n");
        return false;
    }
    proton_maps_init(&context->maps);
    proton_paging_init(&context->paging);
    lc->hDevice = (HANDLE)context;

    if (proton_parse_connection(lc->Config.szDevice, &context->config,
                                error, sizeof(error)) != 0) {
        lcprintf(lc, "PROTON: invalid device string: %s\n"
                     "         expected proton://pid=<linux-host-pid>,ro=<0|1>\n",
                 error);
        goto fail;
    }
    if (proton_process_start_time(context->config.pid,
                                  &context->process_start_time,
                                  error, sizeof(error)) != 0) {
        lcprintf(lc, "PROTON: %s\n", error);
        goto fail;
    }
    if (proton_maps_load(context->config.pid, &context->maps,
                         error, sizeof(error)) != 0) {
        lcprintf(lc, "PROTON: cannot snapshot target mappings: %s\n", error);
        goto fail;
    }
    if (proton_process_probe(context->config.pid, &context->maps,
                             error, sizeof(error)) != 0) {
        lcprintf(lc, "PROTON: %s\n", error);
        goto fail;
    }
    if (proton_process_check_identity(context->config.pid,
                                      context->process_start_time,
                                      error, sizeof(error)) != 0) {
        lcprintf(lc, "PROTON: target changed during initialization: %s\n", error);
        goto fail;
    }
    if (proton_paging_build(&context->maps, &context->paging,
                            error, sizeof(error)) != 0) {
        lcprintf(lc, "PROTON: cannot build synthetic page tables: %s\n", error);
        goto fail;
    }
    if (!LcMemMap_AddRange(lc, context->paging.table_start,
                           context->paging.table_end - context->paging.table_start,
                           context->paging.table_start) ||
        !LcMemMap_AddRange(lc, context->paging.data_start,
                           context->paging.data_end - context->paging.data_start,
                           context->paging.data_start)) {
        lcprintf(lc, "PROTON: cannot register synthetic physical ranges\n");
        goto fail;
    }
    lc->fMultiThread = true;
    lc->Config.fVolatile = true;
    lc->pfnClose = proton_close;
    lc->pfnReadScatter = proton_read_scatter;
    lc->pfnWriteScatter = context->config.read_only ? NULL : proton_write_scatter;
    lc->pfnGetOption = proton_get_option;
    if (context->paging.skipped_mapping_count) {
        lcprintf(lc,
                 "PROTON: warning: skipped %zu mapping(s) outside the canonical "
                 "lower 48-bit address space\n",
                 context->paging.skipped_mapping_count);
    }
    lcprintfv(lc,
              "PROTON: attached to Linux PID %ld (%zu mappings, %zu table pages, "
              "DTB=0x%llx, %s)\n",
              (long)context->config.pid, context->maps.count,
              context->paging.table_page_count,
              (unsigned long long)PROTON_DTB_PHYSICAL,
              context->config.read_only ? "read-only" : "writes enabled");
    return true;

fail:
    proton_close(lc);
    return false;
}
