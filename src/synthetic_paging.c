#include "proton_device.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct PageNode {
    struct PageNode *children[512];
    uint64_t physical_address;
    unsigned int level;
} PageNode;

typedef struct NodeList {
    PageNode **items;
    size_t count;
    size_t capacity;
} NodeList;

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

static bool add_overflow_u64(uint64_t left, uint64_t right, uint64_t *result)
{
    if (left > UINT64_MAX - right) {
        return true;
    }
    *result = left + right;
    return false;
}

static int align_up_u64(uint64_t value, uint64_t alignment, uint64_t *result)
{
    uint64_t mask = alignment - 1;

    if ((alignment & mask) != 0 || value > UINT64_MAX - mask) {
        return -1;
    }
    *result = (value + mask) & ~mask;
    return 0;
}

void proton_paging_init(ProtonPaging *paging)
{
    if (paging) {
        memset(paging, 0, sizeof(*paging));
        paging->table_start = PROTON_DTB_PHYSICAL;
    }
}

void proton_paging_free(ProtonPaging *paging)
{
    if (!paging) {
        return;
    }
    free(paging->tables);
    proton_paging_init(paging);
}

static void node_list_free(NodeList *nodes)
{
    size_t i;

    for (i = 0; i < nodes->count; i++) {
        free(nodes->items[i]);
    }
    free(nodes->items);
    memset(nodes, 0, sizeof(*nodes));
}

static PageNode *node_create(NodeList *nodes, unsigned int level)
{
    PageNode **grown;
    PageNode *node;
    size_t capacity;

    if (nodes->count == nodes->capacity) {
        capacity = nodes->capacity ? nodes->capacity * 2 : 32;
        if (capacity < nodes->capacity || capacity > SIZE_MAX / sizeof(*nodes->items)) {
            return NULL;
        }
        grown = realloc(nodes->items, capacity * sizeof(*nodes->items));
        if (!grown) {
            return NULL;
        }
        nodes->items = grown;
        nodes->capacity = capacity;
    }
    node = calloc(1, sizeof(*node));
    if (!node) {
        return NULL;
    }
    node->level = level;
    nodes->items[nodes->count++] = node;
    return node;
}

static PageNode *ensure_path(PageNode *root, NodeList *nodes, uint64_t address)
{
    const unsigned int shifts[] = {39, 30, 21};
    PageNode *node = root;
    size_t i;

    for (i = 0; i < sizeof(shifts) / sizeof(shifts[0]); i++) {
        size_t index = (size_t)((address >> shifts[i]) & UINT64_C(0x1ff));
        if (!node->children[index]) {
            node->children[index] = node_create(nodes, node->level - 1);
            if (!node->children[index]) {
                return NULL;
            }
        }
        node = node->children[index];
    }
    return node;
}

static void remove_unsupported_maps(ProtonMapList *maps, size_t *skipped)
{
    size_t source;
    size_t destination = 0;

    *skipped = 0;
    for (source = 0; source < maps->count; source++) {
        ProtonVma *map = &maps->items[source];
        if (map->start >= PROTON_LOWER_CANONICAL_LIMIT ||
            map->end > PROTON_LOWER_CANONICAL_LIMIT) {
            free(map->path);
            map->path = NULL;
            (*skipped)++;
            continue;
        }
        if (source != destination) {
            maps->items[destination] = *map;
            memset(map, 0, sizeof(*map));
        }
        maps->items[destination].physical_start = PROTON_PHYS_UNASSIGNED;
        destination++;
    }
    maps->count = destination;
}

static uint64_t *table_for_node(const ProtonPaging *paging, const PageNode *node)
{
    size_t offset = (size_t)(node->physical_address - paging->table_start);
    return (uint64_t *)(void *)(paging->tables + offset);
}

int proton_paging_build(ProtonMapList *maps, ProtonPaging *paging,
                        char *error, size_t error_size)
{
    NodeList nodes = {0};
    PageNode *root = NULL;
    uint64_t table_bytes;
    uint64_t data_cursor;
    size_t i;
    int result = -1;

    if (error && error_size) {
        error[0] = '\0';
    }
    if (!maps || !paging) {
        set_error(error, error_size, "maps and paging output are required");
        return -1;
    }
    proton_paging_free(paging);
    remove_unsupported_maps(maps, &paging->skipped_mapping_count);
    if (maps->count == 0) {
        set_error(error, error_size, "no readable lower-48-bit mappings remain");
        return -1;
    }
    root = node_create(&nodes, 4);
    if (!root) {
        set_error(error, error_size, "out of memory for PML4");
        goto done;
    }
    for (i = 0; i < maps->count; i++) {
        uint64_t address;
        for (address = maps->items[i].start; address < maps->items[i].end;
             address += PROTON_PAGE_SIZE) {
            if (!ensure_path(root, &nodes, address)) {
                set_error(error, error_size, "out of memory building paging tree");
                goto done;
            }
        }
    }
    if (nodes.count > UINT64_MAX / PROTON_PAGE_SIZE ||
        nodes.count > SIZE_MAX / (size_t)PROTON_PAGE_SIZE) {
        set_error(error, error_size, "paging structure is too large");
        goto done;
    }
    table_bytes = (uint64_t)nodes.count * PROTON_PAGE_SIZE;
    paging->table_start = PROTON_DTB_PHYSICAL;
    if (add_overflow_u64(paging->table_start, table_bytes, &paging->table_end) ||
        align_up_u64(paging->table_end, PROTON_DATA_ALIGNMENT, &paging->data_start) != 0) {
        set_error(error, error_size, "physical paging layout overflow");
        goto done;
    }
    data_cursor = paging->data_start;
    for (i = 0; i < maps->count; i++) {
        uint64_t length = maps->items[i].end - maps->items[i].start;
        maps->items[i].physical_start = data_cursor;
        if (add_overflow_u64(data_cursor, length, &data_cursor)) {
            set_error(error, error_size, "synthetic data layout overflow");
            goto done;
        }
    }
    paging->data_end = data_cursor;
    if (paging->data_end > PROTON_PTE_ADDRESS_MASK + PROTON_PAGE_SIZE) {
        set_error(error, error_size, "synthetic physical layout exceeds x86-64 PTE capacity");
        goto done;
    }
    paging->tables = calloc(nodes.count, (size_t)PROTON_PAGE_SIZE);
    if (!paging->tables) {
        set_error(error, error_size, "out of memory materializing page tables");
        goto done;
    }
    paging->table_page_count = nodes.count;
    for (i = 0; i < nodes.count; i++) {
        nodes.items[i]->physical_address =
            paging->table_start + (uint64_t)i * PROTON_PAGE_SIZE;
    }
    for (i = 0; i < nodes.count; i++) {
        PageNode *node = nodes.items[i];
        uint64_t *entries = table_for_node(paging, node);
        size_t index;
        if (node->level == 1) {
            continue;
        }
        for (index = 0; index < 512; index++) {
            if (node->children[index]) {
                entries[index] = node->children[index]->physical_address |
                                 PROTON_PTE_PRESENT | PROTON_PTE_WRITE | PROTON_PTE_USER;
            }
        }
    }
    for (i = 0; i < maps->count; i++) {
        const ProtonVma *map = &maps->items[i];
        uint64_t virtual_address;
        uint64_t physical_address = map->physical_start;
        uint64_t flags = PROTON_PTE_PRESENT | PROTON_PTE_USER;
        if (map->writable) {
            flags |= PROTON_PTE_WRITE;
        }
        if (!map->executable) {
            flags |= PROTON_PTE_NX;
        }
        for (virtual_address = map->start; virtual_address < map->end;
             virtual_address += PROTON_PAGE_SIZE, physical_address += PROTON_PAGE_SIZE) {
            PageNode *pt = ensure_path(root, &nodes, virtual_address);
            size_t index = (size_t)((virtual_address >> 12) & UINT64_C(0x1ff));
            uint64_t *entries;
            if (!pt || pt->physical_address == 0) {
                set_error(error, error_size, "internal paging tree inconsistency");
                goto done;
            }
            entries = table_for_node(paging, pt);
            if (entries[index] != 0) {
                set_error(error, error_size, "duplicate virtual page at 0x%" PRIx64,
                          virtual_address);
                goto done;
            }
            entries[index] = physical_address | flags;
        }
    }
    if (proton_paging_validate(maps, paging, error, error_size) != 0) {
        goto done;
    }
    result = 0;
done:
    node_list_free(&nodes);
    if (result != 0) {
        free(paging->tables);
        paging->tables = NULL;
        paging->table_page_count = 0;
    }
    return result;
}

static int read_table_entry(const ProtonPaging *paging, uint64_t table_address,
                            size_t index, uint64_t *entry)
{
    uint64_t offset;
    uint64_t table_bytes;

    if (!paging || !paging->tables || index >= 512 ||
        table_address < paging->table_start ||
        (table_address & (PROTON_PAGE_SIZE - 1))) {
        return -1;
    }
    offset = table_address - paging->table_start;
    table_bytes = (uint64_t)paging->table_page_count * PROTON_PAGE_SIZE;
    if (offset > table_bytes - PROTON_PAGE_SIZE) {
        return -1;
    }
    memcpy(entry, paging->tables + (size_t)offset + index * sizeof(*entry),
           sizeof(*entry));
    return 0;
}

int proton_paging_walk(const ProtonPaging *paging, uint64_t virtual_address,
                       uint64_t *physical_address, uint64_t *leaf_entry)
{
    const unsigned int shifts[] = {39, 30, 21, 12};
    uint64_t table = PROTON_DTB_PHYSICAL;
    uint64_t entry = 0;
    size_t level;

    if (!paging || virtual_address >= PROTON_LOWER_CANONICAL_LIMIT) {
        return -1;
    }
    for (level = 0; level < sizeof(shifts) / sizeof(shifts[0]); level++) {
        size_t index = (size_t)((virtual_address >> shifts[level]) & UINT64_C(0x1ff));
        if (read_table_entry(paging, table, index, &entry) != 0 ||
            !(entry & PROTON_PTE_PRESENT)) {
            return -1;
        }
        if (level != 3 && (entry & (UINT64_C(1) << 7))) {
            return -1;
        }
        table = entry & PROTON_PTE_ADDRESS_MASK;
    }
    if (physical_address) {
        *physical_address = table | (virtual_address & (PROTON_PAGE_SIZE - 1));
    }
    if (leaf_entry) {
        *leaf_entry = entry;
    }
    return 0;
}

static int validate_table(const ProtonPaging *paging, uint64_t table_address,
                          unsigned int level, uint64_t *leaf_count,
                          char *error, size_t error_size)
{
    size_t index;
    for (index = 0; index < 512; index++) {
        uint64_t entry;
        uint64_t address;
        if (read_table_entry(paging, table_address, index, &entry) != 0) {
            set_error(error, error_size, "page table address is outside table range");
            return -1;
        }
        if (!(entry & PROTON_PTE_PRESENT)) {
            continue;
        }
        address = entry & PROTON_PTE_ADDRESS_MASK;
        if (level > 1) {
            if (!(entry & PROTON_PTE_USER) || !(entry & PROTON_PTE_WRITE) ||
                address < paging->table_start || address >= paging->table_end) {
                set_error(error, error_size, "invalid intermediate page-table entry");
                return -1;
            }
            if (validate_table(paging, address, level - 1, leaf_count,
                               error, error_size) != 0) {
                return -1;
            }
        } else {
            if (!(entry & PROTON_PTE_USER) || address < paging->data_start ||
                address >= paging->data_end) {
                set_error(error, error_size, "leaf PTE points outside registered data");
                return -1;
            }
            (*leaf_count)++;
        }
    }
    return 0;
}

int proton_paging_validate(const ProtonMapList *maps,
                           const ProtonPaging *paging,
                           char *error, size_t error_size)
{
    uint64_t expected_data;
    uint64_t expected_pages = 0;
    uint64_t leaf_count = 0;
    size_t i;

    if (!maps || !paging || !paging->tables || paging->table_page_count == 0 ||
        paging->table_start != PROTON_DTB_PHYSICAL ||
        paging->table_end != paging->table_start +
                             (uint64_t)paging->table_page_count * PROTON_PAGE_SIZE ||
        paging->data_start < paging->table_end ||
        (paging->data_start & (PROTON_DATA_ALIGNMENT - 1))) {
        set_error(error, error_size, "invalid synthetic physical layout");
        return -1;
    }
    expected_data = paging->data_start;
    for (i = 0; i < maps->count; i++) {
        uint64_t length = maps->items[i].end - maps->items[i].start;
        if (maps->items[i].physical_start != expected_data) {
            set_error(error, error_size, "non-contiguous physical translation records");
            return -1;
        }
        expected_data += length;
        expected_pages += length / PROTON_PAGE_SIZE;
    }
    if (expected_data != paging->data_end ||
        validate_table(paging, paging->table_start, 4, &leaf_count,
                       error, error_size) != 0) {
        return -1;
    }
    if (leaf_count != expected_pages) {
        set_error(error, error_size, "page-table leaf count does not match mappings");
        return -1;
    }
    return 0;
}

int proton_paging_read(const ProtonPaging *paging, uint64_t physical_address,
                       void *buffer, size_t length)
{
    uint64_t offset;
    uint64_t available;

    if (!paging || !paging->tables || (!buffer && length) ||
        physical_address < paging->table_start) {
        return -1;
    }
    offset = physical_address - paging->table_start;
    available = paging->table_end - paging->table_start;
    if ((uint64_t)length > available || offset > available - (uint64_t)length) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    memcpy(buffer, paging->tables + (size_t)offset, length);
    return 0;
}

int proton_translate_physical(const ProtonMapList *maps,
                              uint64_t physical_address,
                              ProtonTranslation *translation)
{
    size_t low = 0;
    size_t high;
    size_t index;
    const ProtonVma *map;
    uint64_t length;

    if (!maps || !translation || maps->count == 0) {
        return -1;
    }
    high = maps->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (maps->items[middle].physical_start <= physical_address) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == 0) {
        return -1;
    }
    index = low - 1;
    map = &maps->items[index];
    length = map->end - map->start;
    if (physical_address - map->physical_start >= length) {
        return -1;
    }
    translation->physical_address = physical_address;
    translation->target_address = map->start +
                                  (physical_address - map->physical_start);
    translation->length = (size_t)(length -
                          (physical_address - map->physical_start));
    translation->map_index = index;
    translation->writable = map->writable;
    translation->executable = map->executable;
    return 0;
}

int proton_translate_split(const ProtonMapList *maps,
                           uint64_t physical_address, size_t length,
                           ProtonTranslation *segments,
                           size_t segment_capacity, size_t *segment_count)
{
    uint64_t cursor = physical_address;
    size_t remaining = length;
    size_t count = 0;

    if (!segment_count || (!segments && segment_capacity) ||
        (length && physical_address > UINT64_MAX - (uint64_t)length)) {
        return -1;
    }
    *segment_count = 0;
    while (remaining) {
        ProtonTranslation translation;
        size_t part;
        if (proton_translate_physical(maps, cursor, &translation) != 0) {
            return -1;
        }
        part = translation.length < remaining ? translation.length : remaining;
        if (count >= segment_capacity) {
            *segment_count = count + 1;
            return -2;
        }
        translation.length = part;
        segments[count++] = translation;
        cursor += part;
        remaining -= part;
    }
    *segment_count = count;
    return 0;
}
