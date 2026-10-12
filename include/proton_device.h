#ifndef PROTON_DEVICE_H
#define PROTON_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#define PROTON_PAGE_SIZE UINT64_C(0x1000)
#define PROTON_DTB_PHYSICAL UINT64_C(0x1000)
#define PROTON_DATA_ALIGNMENT UINT64_C(0x200000)
#define PROTON_LOWER_CANONICAL_LIMIT UINT64_C(0x0000800000000000)
#define PROTON_PHYS_UNASSIGNED UINT64_MAX

#define PROTON_PTE_PRESENT UINT64_C(0x001)
#define PROTON_PTE_WRITE UINT64_C(0x002)
#define PROTON_PTE_USER UINT64_C(0x004)
#define PROTON_PTE_NX UINT64_C(0x8000000000000000)
#define PROTON_PTE_ADDRESS_MASK UINT64_C(0x000ffffffffff000)

typedef struct ProtonConfig {
    pid_t pid;
    bool read_only;
} ProtonConfig;

typedef struct ProtonVma {
    uint64_t start;
    uint64_t end;
    uint64_t offset;
    uint64_t inode;
    uint64_t physical_start;
    unsigned int device_major;
    unsigned int device_minor;
    bool readable;
    bool writable;
    bool executable;
    bool private_mapping;
    char *path;
} ProtonVma;

typedef struct ProtonMapList {
    ProtonVma *items;
    size_t count;
    size_t capacity;
} ProtonMapList;

typedef struct ProtonPaging {
    uint8_t *tables;
    size_t table_page_count;
    size_t skipped_mapping_count;
    uint64_t table_start;
    uint64_t table_end;
    uint64_t data_start;
    uint64_t data_end;
} ProtonPaging;

typedef struct ProtonTranslation {
    uint64_t physical_address;
    uint64_t target_address;
    size_t length;
    size_t map_index;
    bool writable;
    bool executable;
} ProtonTranslation;

typedef struct ProtonIoRequest {
    uint64_t remote_address;
    void *local_buffer;
    size_t length;
    bool complete;
} ProtonIoRequest;

int proton_parse_connection(const char *connection, ProtonConfig *config,
                            char *error, size_t error_size);

void proton_maps_init(ProtonMapList *maps);
void proton_maps_free(ProtonMapList *maps);
int proton_maps_parse_stream(FILE *stream, ProtonMapList *maps,
                             char *error, size_t error_size);
int proton_maps_load(pid_t pid, ProtonMapList *maps,
                     char *error, size_t error_size);

void proton_paging_init(ProtonPaging *paging);
void proton_paging_free(ProtonPaging *paging);
int proton_paging_build(ProtonMapList *maps, ProtonPaging *paging,
                        char *error, size_t error_size);
int proton_paging_walk(const ProtonPaging *paging, uint64_t virtual_address,
                       uint64_t *physical_address, uint64_t *leaf_entry);
int proton_paging_validate(const ProtonMapList *maps,
                           const ProtonPaging *paging,
                           char *error, size_t error_size);
int proton_paging_read(const ProtonPaging *paging, uint64_t physical_address,
                       void *buffer, size_t length);

int proton_translate_physical(const ProtonMapList *maps,
                              uint64_t physical_address,
                              ProtonTranslation *translation);
int proton_translate_split(const ProtonMapList *maps,
                           uint64_t physical_address, size_t length,
                           ProtonTranslation *segments,
                           size_t segment_capacity, size_t *segment_count);

int proton_process_start_time(pid_t pid, uint64_t *start_time,
                              char *error, size_t error_size);
int proton_process_check_identity(pid_t pid, uint64_t expected_start_time,
                                  char *error, size_t error_size);
int proton_process_probe(pid_t pid, const ProtonMapList *maps,
                         char *error, size_t error_size);
size_t proton_process_read_batch(pid_t pid, ProtonIoRequest *requests,
                                 size_t request_count);
size_t proton_process_write_batch(pid_t pid, ProtonIoRequest *requests,
                                  size_t request_count);

#endif
