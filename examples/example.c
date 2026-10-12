/*
 * Minimal MemProcFS C API example for the Proton process connector.
 *
 * Usage: proton_example <linux-pid> <virtual-address> [byte-count]
 */
#include <errno.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vmmdll.h>

#define EXAMPLE_DEFAULT_BYTES 256U
#define EXAMPLE_MAX_BYTES (1024U * 1024U)

static int parse_u64(const char *text, int base, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (!text || !*text || *text == '-' || *text == '+' ||
        isspace((unsigned char)*text)) {
        return -1;
    }
    errno = 0;
    parsed = strtoull(text, &end, base);
    if (errno == ERANGE || end == text || *end != '\0') {
        return -1;
    }
    *value = (uint64_t)parsed;
    return 0;
}

static void print_hex_ascii(const unsigned char *bytes, size_t length,
                            uint64_t base_address)
{
    size_t offset;

    for (offset = 0; offset < length; offset += 16) {
        size_t column;
        size_t line_length = length - offset < 16 ? length - offset : 16;
        printf("%016" PRIx64 "  ", base_address + offset);
        for (column = 0; column < 16; column++) {
            if (column < line_length) {
                printf("%02x ", bytes[offset + column]);
            } else {
                printf("   ");
            }
            if (column == 7) {
                putchar(' ');
            }
        }
        printf(" |");
        for (column = 0; column < line_length; column++) {
            unsigned char value = bytes[offset + column];
            putchar(value >= 0x20 && value <= 0x7e ? value : '.');
        }
        for (; column < 16; column++) {
            putchar(' ');
        }
        puts("|");
    }
}

static const VMMDLL_MAP_PTEENTRY *find_pte(const VMMDLL_MAP_PTE *map,
                                           uint64_t address)
{
    DWORD i;

    if (!map || map->dwVersion != VMMDLL_MAP_PTE_VERSION) {
        return NULL;
    }
    for (i = 0; i < map->cMap; i++) {
        uint64_t size = map->pMap[i].cPages << 12;
        if (address >= map->pMap[i].vaBase &&
            address - map->pMap[i].vaBase < size) {
            return &map->pMap[i];
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    uint64_t parsed_pid;
    uint64_t virtual_address;
    uint64_t requested = EXAMPLE_DEFAULT_BYTES;
    ULONG64 physical_address;
    char device[128];
    LPCSTR vmm_arguments[8];
    VMM_HANDLE vmm = NULL;
    PVMMDLL_MAP_PTE pte_map = NULL;
    const VMMDLL_MAP_PTEENTRY *pte;
    unsigned char *buffer = NULL;
    DWORD bytes_read = 0;
    DWORD print_length;
    int result = 1;

    if (argc < 3 || argc > 4) {
        fprintf(stderr,
                "Usage: %s <linux-pid> <virtual-address> [byte-count]\n"
                "Example: %s 12345 0x7bc00000 256\n",
                argv[0], argv[0]);
        return 2;
    }
    if (parse_u64(argv[1], 10, &parsed_pid) != 0 ||
        parsed_pid == 0 || parsed_pid > INT_MAX) {
        fprintf(stderr, "Invalid Linux PID: %s\n", argv[1]);
        return 2;
    }
    if (parse_u64(argv[2], 0, &virtual_address) != 0) {
        fprintf(stderr, "Invalid virtual address: %s\n", argv[2]);
        return 2;
    }
    if (argc == 4 && parse_u64(argv[3], 0, &requested) != 0) {
        fprintf(stderr, "Invalid byte count: %s\n", argv[3]);
        return 2;
    }
    if (requested == 0 || requested > EXAMPLE_MAX_BYTES || requested > UINT32_MAX ||
        virtual_address > UINT64_MAX - requested) {
        fprintf(stderr, "Byte count must be between 1 and %u without address overflow.\n",
                EXAMPLE_MAX_BYTES);
        return 2;
    }
    if (snprintf(device, sizeof(device), "proton://pid=%" PRIu64 ",ro=1",
                 parsed_pid) >= (int)sizeof(device)) {
        fprintf(stderr, "Device string is too long.\n");
        return 2;
    }
    vmm_arguments[0] = "";
    vmm_arguments[1] = "-device";
    vmm_arguments[2] = device;
    vmm_arguments[3] = "-arch";
    vmm_arguments[4] = "x64";
    vmm_arguments[5] = "-dtb";
    vmm_arguments[6] = "0x1000";
    vmm_arguments[7] = "-norefresh";

    printf("Opening %s through MemProcFS...\n", device);
    vmm = VMMDLL_Initialize(8, vmm_arguments);
    if (!vmm) {
        fprintf(stderr,
                "VMMDLL_Initialize failed. Ensure leechcore_device_proton.so is "
                "beside the loaded leechcore.so and check ptrace permissions.\n");
        goto done;
    }
    if (!VMMDLL_MemVirt2Phys(vmm, 1, virtual_address, &physical_address)) {
        fprintf(stderr, "Virtual-to-physical translation failed for 0x%" PRIx64 ".\n",
                virtual_address);
        goto done;
    }
    printf("unknown_process PID 1: VA 0x%016" PRIx64 " -> synthetic PA 0x%016"
           PRIx64 "\n", virtual_address, (uint64_t)physical_address);

    if (VMMDLL_Map_GetPteU(vmm, 1, 0, &pte_map)) {
        pte = find_pte(pte_map, virtual_address);
        if (pte) {
            printf("PTE range: 0x%016" PRIx64 "-0x%016" PRIx64
                   "  %c%c%c  (%" PRIu64 " pages)\n",
                   (uint64_t)pte->vaBase,
                   (uint64_t)(pte->vaBase + (pte->cPages << 12) - 1),
                   'r', (pte->fPage & VMMDLL_MEMMAP_FLAG_PAGE_W) ? 'w' : '-',
                   (pte->fPage & VMMDLL_MEMMAP_FLAG_PAGE_NX) ? '-' : 'x',
                   (uint64_t)pte->cPages);
        }
    }

    buffer = malloc((size_t)requested);
    if (!buffer) {
        fprintf(stderr, "Cannot allocate read buffer.\n");
        goto done;
    }
    if (!VMMDLL_MemReadEx(vmm, 1, virtual_address, buffer, (DWORD)requested,
                          &bytes_read, VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_NOPAGING)) {
        fprintf(stderr, "Virtual read failed.\n");
        goto done;
    }
    printf("Read %u/%" PRIu64 " live bytes.\n", bytes_read, requested);
    print_length = bytes_read < 256 ? bytes_read : 256;
    print_hex_ascii(buffer, print_length, virtual_address);
    if (bytes_read > print_length) {
        printf("... %u additional bytes omitted\n", bytes_read - print_length);
    }
    result = bytes_read == requested ? 0 : 1;

done:
    free(buffer);
    if (pte_map) {
        VMMDLL_MemFree(pte_map);
    }
    if (vmm) {
        VMMDLL_Close(vmm);
    }
    return result;
}
