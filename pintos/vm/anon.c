/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "devices/disk.h"
#include "lib/kernel/bitmap.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "vm/vm.h"
#ifndef BLOCK_SECTOR_SIZE
#define BLOCK_SECTOR_SIZE 512
#endif
static struct bitmap *swap_table;
static struct lock swap_lock;
/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in(struct page *page, void *kva);
static bool anon_swap_out(struct page *page);
static void anon_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
    .swap_in = anon_swap_in,
    .swap_out = anon_swap_out,
    .destroy = anon_destroy,
    .type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void vm_anon_init(void) {
    swap_disk = disk_get(1, 1);
    ASSERT(swap_disk != NULL);

    // 총 섹터 개수 (그냥 capacity)
    size_t total_sectors = disk_size(swap_disk);

    // 섹터 총 개수
    size_t sectors_per_page = PGSIZE / BLOCK_SECTOR_SIZE;
    ASSERT(PGSIZE % BLOCK_SECTOR_SIZE == 0);

    size_t slot_cnt = total_sectors / sectors_per_page;
    ASSERT(slot_cnt > 0);

    // 슬롯 개수 배열 만들기
    swap_table = bitmap_create(slot_cnt);
    if (swap_table == NULL) {
        PANIC("Failed to create swap bitmap");
    }

    bitmap_set_all(swap_table, false);
    lock_init(&swap_lock);
}

/* Initialize the file mapping */
bool anon_initializer(struct page *page, enum vm_type type, void *kva) {
    ASSERT(page != NULL && vm_type == VM_ANON);

    // 페이지 메소드 추가
    page->operations = &anon_ops;

    // 페이지 구조체 설정
    struct anon_page *anon_page = &page->anon;

    // slot 초가화
    anon_page->swap_slot = SIZE_MAX;
    return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool anon_swap_in(struct page *page, void *kva) {
    struct anon_page *anon_page = &page->anon;
}

/* Swap out the page by writing contents to the swap disk. */
static bool anon_swap_out(struct page *page) {
    struct anon_page *anon_page = &page->anon;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void anon_destroy(struct page *page) {
    struct anon_page *anon_page = &page->anon;
}
