/*
 * End-to-end uncached virtual-read benchmark for the Proton connector.
 *
 * Usage: proton_benchmark <linux-pid> <virtual-address> <size>
 *                         [seconds] [batch-pages]
 */
#include <errno.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <leechcore.h>
#include <vmmdll.h>

#define PAGE_SIZE_4K UINT64_C(0x1000)
#define DEFAULT_SECONDS 5.0
#define DEFAULT_BATCH_PAGES 256U
#define MAX_BATCH_PAGES 65536U

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

static int parse_seconds(const char *text, double *value)
{
    char *end = NULL;
    double parsed;

    if (!text || !*text || *text == '-' || *text == '+' ||
        isspace((unsigned char)*text)) {
        return -1;
    }
    errno = 0;
    parsed = strtod(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' || !isfinite(parsed) ||
        parsed < 0.1 || parsed > 3600.0) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static double monotonic_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0.0;
    }
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void prepare_batch(PPMEM_SCATTER pages, DWORD page_count,
                          uint64_t base_address, uint64_t working_set_pages,
                          uint64_t first_page)
{
    DWORD i;
    for (i = 0; i < page_count; i++) {
        uint64_t page = (first_page + i) % working_set_pages;
        pages[i]->qwA = base_address + page * PAGE_SIZE_4K;
        pages[i]->cb = (DWORD)PAGE_SIZE_4K;
        pages[i]->f = 0;
    }
}

int main(int argc, char **argv)
{
    uint64_t parsed_pid;
    uint64_t base_address;
    uint64_t working_set_size;
    uint64_t working_set_pages;
    uint64_t parsed_batch_pages = DEFAULT_BATCH_PAGES;
    DWORD batch_pages;
    double requested_seconds = DEFAULT_SECONDS;
    char device[128];
    LPCSTR vmm_arguments[8];
    VMM_HANDLE vmm = NULL;
    PPMEM_SCATTER pages = NULL;
    uint64_t first_page = 0;
    uint64_t attempted_pages = 0;
    uint64_t successful_pages = 0;
    uint64_t batches = 0;
    uint64_t checksum = 0;
    double start;
    double end;
    double elapsed;
    DWORD i;
    int result = 1;

    if (argc < 4 || argc > 6) {
        fprintf(stderr,
                "Usage: %s <linux-pid> <virtual-address> <size> "
                "[seconds] [batch-pages]\n"
                "Example: %s 12345 0x7bc00000 0x4000000 5 256\n",
                argv[0], argv[0]);
        return 2;
    }
    if (parse_u64(argv[1], 10, &parsed_pid) != 0 ||
        parsed_pid == 0 || parsed_pid > INT_MAX ||
        parse_u64(argv[2], 0, &base_address) != 0 ||
        parse_u64(argv[3], 0, &working_set_size) != 0) {
        fprintf(stderr, "Invalid PID, virtual address, or working-set size.\n");
        return 2;
    }
    if (argc >= 5 && parse_seconds(argv[4], &requested_seconds) != 0) {
        fprintf(stderr, "Duration must be between 0.1 and 3600 seconds.\n");
        return 2;
    }
    if (argc == 6 && parse_u64(argv[5], 10, &parsed_batch_pages) != 0) {
        fprintf(stderr, "Invalid batch-page count.\n");
        return 2;
    }
    if ((base_address & (PAGE_SIZE_4K - 1)) ||
        working_set_size == 0 || (working_set_size & (PAGE_SIZE_4K - 1)) ||
        base_address > UINT64_MAX - working_set_size ||
        parsed_batch_pages == 0 || parsed_batch_pages > MAX_BATCH_PAGES) {
        fprintf(stderr,
                "Address and size must be 4 KiB aligned, non-empty, non-overflowing; "
                "batch-pages must be 1-%u.\n", MAX_BATCH_PAGES);
        return 2;
    }
    working_set_pages = working_set_size / PAGE_SIZE_4K;
    batch_pages = (DWORD)parsed_batch_pages;
    if (working_set_pages < batch_pages) {
        batch_pages = (DWORD)working_set_pages;
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

    vmm = VMMDLL_Initialize(8, vmm_arguments);
    if (!vmm) {
        fprintf(stderr,
                "VMMDLL_Initialize failed. Check plugin placement and ptrace permissions.\n");
        goto done;
    }
    if (!LcAllocScatter1(batch_pages, &pages)) {
        fprintf(stderr, "Cannot allocate %u scatter pages.\n", batch_pages);
        goto done;
    }

    prepare_batch(pages, batch_pages, base_address, working_set_pages, 0);
    if (VMMDLL_MemReadScatter(vmm, 1, pages, batch_pages,
                              VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_NOPAGING) != batch_pages) {
        fprintf(stderr,
                "Warm-up read failed. Confirm the complete benchmark range is readable.\n");
        goto done;
    }

    printf("Proton connector virtual-read benchmark\n"
           "  device:       %s\n"
           "  range:        0x%016" PRIx64 "-0x%016" PRIx64 " (%0.2f MiB)\n"
           "  batch:        %u pages (%0.2f MiB)\n"
           "  duration:     %.2f seconds\n",
           device, base_address, base_address + working_set_size - 1,
           (double)working_set_size / (1024.0 * 1024.0), batch_pages,
           (double)batch_pages * 4096.0 / (1024.0 * 1024.0), requested_seconds);

    start = monotonic_seconds();
    if (start == 0.0) {
        fprintf(stderr, "clock_gettime(CLOCK_MONOTONIC) failed.\n");
        goto done;
    }
    do {
        DWORD success;
        prepare_batch(pages, batch_pages, base_address, working_set_pages, first_page);
        success = VMMDLL_MemReadScatter(vmm, 1, pages, batch_pages,
                                        VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_NOPAGING);
        attempted_pages += batch_pages;
        successful_pages += success;
        batches++;
        first_page = (first_page + batch_pages) % working_set_pages;
        for (i = 0; i < batch_pages; i++) {
            if (pages[i]->f) {
                checksum += pages[i]->pb[0];
            }
        }
        end = monotonic_seconds();
        if (end == 0.0) {
            fprintf(stderr, "clock_gettime(CLOCK_MONOTONIC) failed.\n");
            goto done;
        }
    } while (end - start < requested_seconds);

    elapsed = end - start;
    printf("\nResults\n"
           "  elapsed:      %.3f s\n"
           "  batches:      %" PRIu64 " (%.0f batches/s)\n"
           "  pages:        %" PRIu64 "/%" PRIu64 " successful\n"
           "  throughput:   %.2f MiB/s (%.3f GiB/s)\n"
           "  failure rate: %.4f%%\n"
           "  checksum:     0x%016" PRIx64 "\n",
           elapsed, batches, (double)batches / elapsed,
           successful_pages, attempted_pages,
           (double)successful_pages * 4096.0 / (1024.0 * 1024.0) / elapsed,
           (double)successful_pages * 4096.0 /
               (1024.0 * 1024.0 * 1024.0) / elapsed,
           attempted_pages ?
               100.0 * (double)(attempted_pages - successful_pages) /
                   (double)attempted_pages : 0.0,
           checksum);
    printf("RESULT seconds=%.6f bytes=%" PRIu64
           " mib_per_second=%.3f failed_pages=%" PRIu64 "\n",
           elapsed, successful_pages * UINT64_C(4096),
           (double)successful_pages * 4096.0 / (1024.0 * 1024.0) / elapsed,
           attempted_pages - successful_pages);
    result = successful_pages == attempted_pages ? 0 : 1;

done:
    if (pages) {
        LcMemFree(pages);
    }
    if (vmm) {
        VMMDLL_Close(vmm);
    }
    return result;
}
