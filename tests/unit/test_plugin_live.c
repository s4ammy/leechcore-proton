#include "proton_device.h"

#include <leechcore_device.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        return 1; \
    } \
} while (0)

static volatile unsigned char live_marker[] = "PROTON_PLUGIN_LIVE_MARKER";
static volatile uint64_t writable_value = UINT64_C(0x1122334455667788);

extern BOOL LcPluginCreate(PLC_CONTEXT context,
                           PPLC_CONFIG_ERRORINFO error_info);

BOOL LcMemMap_AddRange(PLC_CONTEXT context, QWORD address, QWORD length, QWORD remap)
{
    PLC_MEMMAP_ENTRY grown;
    DWORD capacity;

    if (!context || !length || (address & 0xfff) || (length & 0xfff) ||
        address > UINT64_MAX - length ||
        (context->cMemMap &&
         context->pMemMap[context->cMemMap - 1].pa +
         context->pMemMap[context->cMemMap - 1].cb > address)) {
        return false;
    }
    if (context->cMemMap == context->cMemMapMax) {
        capacity = context->cMemMapMax ? context->cMemMapMax * 2 : 4;
        grown = realloc(context->pMemMap, capacity * sizeof(*grown));
        if (!grown) {
            return false;
        }
        context->pMemMap = grown;
        context->cMemMapMax = capacity;
    }
    context->pMemMap[context->cMemMap].pa = address;
    context->pMemMap[context->cMemMap].cb = length;
    context->pMemMap[context->cMemMap].paRemap = remap;
    context->cMemMap++;
    return true;
}

static int physical_read(PLC_CONTEXT context, uint64_t address,
                         void *buffer, size_t length)
{
    MEM_SCATTER memory;
    PMEM_SCATTER pointer = &memory;

    memset(&memory, 0, sizeof(memory));
    memory.version = MEM_SCATTER_VERSION;
    memory.qwA = address;
    memory.pb = buffer;
    memory.cb = (DWORD)length;
    context->pfnReadScatter(context, 1, &pointer);
    return memory.f ? 0 : -1;
}

static int physical_write(PLC_CONTEXT context, uint64_t address,
                          void *buffer, size_t length)
{
    MEM_SCATTER memory;
    PMEM_SCATTER pointer = &memory;

    memset(&memory, 0, sizeof(memory));
    memory.version = MEM_SCATTER_VERSION;
    memory.qwA = address;
    memory.pb = buffer;
    memory.cb = (DWORD)length;
    context->pfnWriteScatter(context, 1, &pointer);
    return memory.f ? 0 : -1;
}

static int virtual_to_physical(PLC_CONTEXT context, uint64_t virtual_address,
                               uint64_t *physical_address)
{
    const unsigned int shifts[] = {39, 30, 21, 12};
    uint64_t table = PROTON_DTB_PHYSICAL;
    uint64_t entry;
    size_t i;

    for (i = 0; i < sizeof(shifts) / sizeof(shifts[0]); i++) {
        uint64_t entry_address = table +
            ((virtual_address >> shifts[i]) & UINT64_C(0x1ff)) * sizeof(entry);
        if (physical_read(context, entry_address, &entry, sizeof(entry)) != 0 ||
            !(entry & PROTON_PTE_PRESENT)) {
            return -1;
        }
        table = entry & PROTON_PTE_ADDRESS_MASK;
    }
    *physical_address = table | (virtual_address & (PROTON_PAGE_SIZE - 1));
    return 0;
}

static int open_device(LC_CONTEXT *context, bool read_only)
{
    memset(context, 0, sizeof(*context));
    context->version = LC_CONTEXT_VERSION;
    (void)snprintf(context->Config.szDevice, sizeof(context->Config.szDevice),
                   "proton://pid=%ld,ro=%d", (long)getpid(), read_only ? 1 : 0);
    return LcPluginCreate(context, NULL) ? 0 : -1;
}

int main(void)
{
    LC_CONTEXT context;
    uint64_t physical_address;
    uint64_t volatile_physical[3];
    unsigned char marker_copy[sizeof(live_marker)];
    unsigned char volatile_copy[3][8];
    unsigned char *volatile_region;
    MEM_SCATTER volatile_memories[3];
    PMEM_SCATTER volatile_pointers[3];
    uint64_t replacement = UINT64_C(0xfedcba9876543210);
    uint64_t original_table_word;
    uint64_t table_word = 0;
    uint64_t start_time;
    char identity_error[160];

    CHECK(proton_process_start_time(getpid(), &start_time,
                                    identity_error, sizeof(identity_error)) == 0);
    CHECK(proton_process_check_identity(getpid(), start_time,
                                        identity_error, sizeof(identity_error)) == 0);
    CHECK(proton_process_check_identity(getpid(), start_time + 1,
                                        identity_error, sizeof(identity_error)) != 0);

    volatile_region = mmap(NULL, 3 * (size_t)PROTON_PAGE_SIZE,
                           PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                           -1, 0);
    CHECK(volatile_region != MAP_FAILED);
    memcpy(volatile_region, "PAGE000", 8);
    memcpy(volatile_region + 2 * PROTON_PAGE_SIZE, "PAGE222", 8);

    CHECK(open_device(&context, true) == 0);
    CHECK(context.pfnReadScatter != NULL);
    CHECK(context.pfnWriteScatter == NULL);
    CHECK(context.cMemMap == 2);
    CHECK(context.pMemMap[0].pa == PROTON_DTB_PHYSICAL);
    CHECK(context.pMemMap[1].pa % PROTON_DATA_ALIGNMENT == 0);
    CHECK(virtual_to_physical(&context, (uint64_t)(uintptr_t)live_marker,
                              &physical_address) == 0);
    CHECK(physical_read(&context, physical_address, marker_copy,
                        sizeof(marker_copy)) == 0);
    CHECK(memcmp(marker_copy, (const void *)live_marker, sizeof(marker_copy)) == 0);
    for (size_t i = 0; i < 3; i++) {
        CHECK(virtual_to_physical(&context,
                                  (uint64_t)(uintptr_t)(volatile_region +
                                      i * PROTON_PAGE_SIZE),
                                  &volatile_physical[i]) == 0);
        memset(&volatile_memories[i], 0, sizeof(volatile_memories[i]));
        volatile_memories[i].version = MEM_SCATTER_VERSION;
        volatile_memories[i].qwA = volatile_physical[i];
        volatile_memories[i].pb = volatile_copy[i];
        volatile_memories[i].cb = sizeof(volatile_copy[i]);
        volatile_pointers[i] = &volatile_memories[i];
    }
    CHECK(munmap(volatile_region + PROTON_PAGE_SIZE,
                 (size_t)PROTON_PAGE_SIZE) == 0);
    context.pfnReadScatter(&context, 3, volatile_pointers);
    CHECK(volatile_memories[0].f);
    CHECK(!volatile_memories[1].f);
    CHECK(volatile_memories[2].f);
    CHECK(memcmp(volatile_copy[0], "PAGE000", 8) == 0);
    CHECK(memcmp(volatile_copy[2], "PAGE222", 8) == 0);
    context.pfnClose(&context);
    free(context.pMemMap);
    CHECK(munmap(volatile_region, (size_t)PROTON_PAGE_SIZE) == 0);
    CHECK(munmap(volatile_region + 2 * PROTON_PAGE_SIZE,
                 (size_t)PROTON_PAGE_SIZE) == 0);

    CHECK(open_device(&context, false) == 0);
    CHECK(context.pfnWriteScatter != NULL);
    CHECK(virtual_to_physical(&context, (uint64_t)(uintptr_t)&writable_value,
                              &physical_address) == 0);
    CHECK(physical_write(&context, physical_address, &replacement,
                         sizeof(replacement)) == 0);
    CHECK(writable_value == replacement);
    CHECK(physical_read(&context, PROTON_DTB_PHYSICAL,
                        &original_table_word, sizeof(original_table_word)) == 0);
    CHECK(physical_write(&context, PROTON_DTB_PHYSICAL,
                         &table_word, sizeof(table_word)) != 0);
    CHECK(physical_read(&context, PROTON_DTB_PHYSICAL,
                        &table_word, sizeof(table_word)) == 0);
    CHECK(table_word == original_table_word);
    context.pfnClose(&context);
    free(context.pMemMap);

    puts("test_plugin_live: PASS");
    return 0;
}
