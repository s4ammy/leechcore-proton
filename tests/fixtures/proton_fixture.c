#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FIXTURE_SIZE 8192
#define MARKER_OFFSET 0
#define SEQUENCE_OFFSET 64
#define SELF_OFFSET 72
#define COMMAND_OFFSET 80
#define ACK_OFFSET 88
#define BOUNDARY_OFFSET (4096 - 32)
#define BOUNDARY_SIZE 64
#define BENCHMARK_SIZE (16U * 1024U * 1024U)
#define READY_LINE_SIZE 1024
#define ACK_XOR UINT64_C(0xa55aa55adeadbeef)

static const char marker[] = "MEMPROCFS_PROTON_TEST_V1";

static int WriteReadyFile(const char *path, const char *line, DWORD lineLength) {
    HANDLE readyFile;
    DWORD bytesWritten;
    BOOL writeResult;

    readyFile = CreateFileA(path, GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (readyFile == INVALID_HANDLE_VALUE) {
        return -1;
    }

    writeResult = WriteFile(readyFile, line, lineLength, &bytesWritten, NULL);
    if (writeResult) {
        writeResult = FlushFileBuffers(readyFile);
    }
    CloseHandle(readyFile);

    return writeResult && bytesWritten == lineLength ? 0 : -1;
}

int main(int argc, char **argv) {
    unsigned char *region;
    unsigned char *read_only;
    unsigned char *read_write;
    unsigned char *read_execute;
    unsigned char *benchmark_region;
    DWORD old_protection;
    unsigned int i;
    char readyLine[READY_LINE_SIZE];
    int readyLength;

    region = VirtualAlloc(NULL, FIXTURE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    read_only = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    read_write = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    read_execute = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    benchmark_region = VirtualAlloc(NULL, BENCHMARK_SIZE,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!region || !read_only || !read_write || !read_execute || !benchmark_region) {
        fprintf(stderr, "fixture allocation failed: %lu\n", GetLastError());
        return 1;
    }
    memcpy(region + MARKER_OFFSET, marker, sizeof(marker) - 1);
    *(volatile uint64_t *)(void *)(region + SEQUENCE_OFFSET) = 1;
    *(uint64_t *)(void *)(region + SELF_OFFSET) = (uint64_t)(uintptr_t)region;
    *(volatile uint64_t *)(void *)(region + COMMAND_OFFSET) = 0;
    *(volatile uint64_t *)(void *)(region + ACK_OFFSET) = 0;
    for (i = 0; i < BOUNDARY_SIZE; i++) {
        region[BOUNDARY_OFFSET + i] = (unsigned char)((i * 37 + 11) & 0xff);
    }

    memcpy(read_only, "PROTON_READ_ONLY_V1", sizeof("PROTON_READ_ONLY_V1") - 1);
    memcpy(read_write, "PROTON_READ_WRITE_V1", sizeof("PROTON_READ_WRITE_V1") - 1);
    read_execute[0] = 0xc3; /* ret */
    for (i = 0; i < BENCHMARK_SIZE; i += 4096) {
        benchmark_region[i] = (unsigned char)((i >> 12) & 0xff);
    }
    if (!VirtualProtect(read_only, 4096, PAGE_READONLY, &old_protection) ||
        !VirtualProtect(read_execute, 4096, PAGE_EXECUTE_READ, &old_protection)) {
        fprintf(stderr, "fixture VirtualProtect failed: %lu\n", GetLastError());
        return 1;
    }

    readyLength = snprintf(
        readyLine, sizeof(readyLine),
        "READY address=0x%016llx size=%u ro=0x%016llx rw=0x%016llx "
        "rx=0x%016llx marker_offset=%u sequence_offset=%u self_offset=%u "
        "command_offset=%u ack_offset=%u boundary_offset=%u boundary_size=%u "
        "benchmark=0x%016llx benchmark_size=%u\n",
        (unsigned long long)(uintptr_t)region, FIXTURE_SIZE,
        (unsigned long long)(uintptr_t)read_only,
        (unsigned long long)(uintptr_t)read_write,
        (unsigned long long)(uintptr_t)read_execute,
        MARKER_OFFSET, SEQUENCE_OFFSET, SELF_OFFSET, COMMAND_OFFSET,
        ACK_OFFSET, BOUNDARY_OFFSET, BOUNDARY_SIZE,
        (unsigned long long)(uintptr_t)benchmark_region, BENCHMARK_SIZE);
    if (readyLength < 0 || (size_t)readyLength >= sizeof(readyLine)) {
        fprintf(stderr, "fixture READY line is too long\n");
        return 1;
    }

    fputs(readyLine, stderr);
    fflush(stderr);
    if (argc == 2 &&
        WriteReadyFile(argv[1], readyLine, (DWORD)readyLength) != 0) {
        fprintf(stderr, "fixture could not write READY file: %lu\n", GetLastError());
        return 1;
    }

    for (;;) {
        uint64_t command;
        InterlockedIncrement64((volatile LONG64 *)(void *)(region + SEQUENCE_OFFSET));
        command = *(volatile uint64_t *)(void *)(region + COMMAND_OFFSET);
        if (command != 0 &&
            *(volatile uint64_t *)(void *)(region + ACK_OFFSET) != (command ^ ACK_XOR)) {
            *(volatile uint64_t *)(void *)(region + ACK_OFFSET) = command ^ ACK_XOR;
        }
        Sleep(50);
    }
}
