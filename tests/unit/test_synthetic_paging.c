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

static int add_map(ProtonMapList *maps, uint64_t start, uint64_t end,
                   bool writable, bool executable, const char *path)
{
    ProtonVma *grown = realloc(maps->items, (maps->count + 1) * sizeof(*maps->items));
    ProtonVma *map;
    if (!grown) {
        return -1;
    }
    maps->items = grown;
    maps->capacity = maps->count + 1;
    map = &maps->items[maps->count++];
    memset(map, 0, sizeof(*map));
    map->start = start;
    map->end = end;
    map->readable = true;
    map->writable = writable;
    map->executable = executable;
    map->private_mapping = true;
    map->physical_start = PROTON_PHYS_UNASSIGNED;
    map->path = strdup(path);
    return map->path ? 0 : -1;
}

int main(void)
{
    ProtonMapList maps;
    ProtonPaging paging;
    ProtonTranslation translation;
    ProtonTranslation segments[2];
    size_t segment_count;
    uint64_t physical;
    uint64_t leaf;
    uint64_t expected_physical;
    char error[256];
    size_t i;

    proton_maps_init(&maps);
    proton_paging_init(&paging);
    CHECK(add_map(&maps, 0x1000, 0x2000, true, false, "rw") == 0);
    CHECK(add_map(&maps, 0x200000, 0x201000, false, true, "rx") == 0);
    CHECK(add_map(&maps, 0x40000000, 0x40001000, false, false, "ro") == 0);
    CHECK(add_map(&maps, 0x8000000000, 0x8000001000, true, true, "pml4-1") == 0);
    CHECK(add_map(&maps, PROTON_LOWER_CANONICAL_LIMIT,
                  PROTON_LOWER_CANONICAL_LIMIT + PROTON_PAGE_SIZE,
                  true, false, "five-level") == 0);

    CHECK(proton_paging_build(&maps, &paging, error, sizeof(error)) == 0);
    CHECK(paging.table_start == PROTON_DTB_PHYSICAL);
    CHECK(paging.table_page_count >= 10);
    CHECK(paging.skipped_mapping_count == 1);
    CHECK(maps.count == 4);
    CHECK((paging.data_start & (PROTON_DATA_ALIGNMENT - 1)) == 0);
    CHECK(proton_paging_validate(&maps, &paging, error, sizeof(error)) == 0);

    for (i = 0; i < maps.count; i++) {
        CHECK(proton_paging_walk(&paging, maps.items[i].start,
                                 &physical, &leaf) == 0);
        CHECK(physical == maps.items[i].physical_start);
        CHECK((leaf & PROTON_PTE_PRESENT) != 0);
        CHECK((leaf & PROTON_PTE_USER) != 0);
        CHECK(((leaf & PROTON_PTE_WRITE) != 0) == maps.items[i].writable);
        CHECK(((leaf & PROTON_PTE_NX) == 0) == maps.items[i].executable);
        CHECK(proton_paging_walk(&paging, maps.items[i].end - 1,
                                 &physical, NULL) == 0);
        CHECK(physical == maps.items[i].physical_start +
                          (maps.items[i].end - maps.items[i].start) - 1);
        CHECK(proton_translate_physical(&maps, maps.items[i].physical_start,
                                        &translation) == 0);
        CHECK(translation.target_address == maps.items[i].start);
        CHECK(proton_translate_physical(
                  &maps,
                  maps.items[i].physical_start +
                      (maps.items[i].end - maps.items[i].start) - 1,
                  &translation) == 0);
        CHECK(translation.target_address == maps.items[i].end - 1);
    }
    CHECK(proton_paging_walk(&paging, 0x3000, NULL, NULL) != 0);
    CHECK(proton_paging_walk(&paging, PROTON_LOWER_CANONICAL_LIMIT,
                             NULL, NULL) != 0);

    CHECK(proton_translate_physical(&maps, PROTON_DTB_PHYSICAL,
                                    &translation) != 0);
    CHECK(proton_translate_physical(&maps, paging.data_end,
                                    &translation) != 0);

    expected_physical = maps.items[0].physical_start + PROTON_PAGE_SIZE - 8;
    CHECK(proton_translate_split(&maps, expected_physical, 16,
                                 segments, 2, &segment_count) == 0);
    CHECK(segment_count == 2);
    CHECK(segments[0].length == 8 && segments[0].target_address == 0x1ff8);
    CHECK(segments[1].length == 8 && segments[1].target_address == 0x200000);
    CHECK(proton_translate_split(&maps, UINT64_MAX - 3, 8,
                                 segments, 2, &segment_count) != 0);

    proton_paging_free(&paging);
    proton_maps_free(&maps);
    puts("test_synthetic_paging: PASS");
    return 0;
}
