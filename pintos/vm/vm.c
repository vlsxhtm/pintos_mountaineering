/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"

#include "anon.h"
#include "disk.h"
#include "file.h"
#include "list.h"
#include "mmu.h"
#include "threads/malloc.h"
#include "uninit.h"
#include "vm/inspect.h"

static struct list frame_table;
static struct lock frame_table_lock;
static struct list_elem *clock_hand = NULL;

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void) {
    vm_anon_init();
    vm_file_init();
#ifdef EFILESYS /* For project 4 */
    pagecache_init();
#endif
    register_inspect_intr();
    /* DO NOT MODIFY UPPER LINES. */
    /* TODO: Your code goes here. */
    list_init(&frame_table);
    disk_init();  // vm_anon_init()에서 swap 영역 지정할 때 사용하기 위해서 여기서 초기화함
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type page_get_type(struct page *page) {
    int ty = VM_TYPE(page->operations->type);
    switch (ty) {
        case VM_UNINIT:
            return VM_TYPE(page->uninit.type);
        default:
            return ty;
    }
}

/* Helpers */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
                                    vm_initializer *init, void *aux) {
    ASSERT(VM_TYPE(type) != VM_UNINIT)

    struct supplemental_page_table *spt = &thread_current()->spt;

    /* Check wheter the upage is already occupied or not. */
    if (spt_find_page(spt, upage) == NULL) {
        /* TODO: Create the page, fetch the initialier according to the VM type,
         * TODO: and then create "uninit" page struct by calling uninit_new. You
         * TODO: should modify the field after calling the uninit_new. */
        // anon_initializer -> 익명 페이지 1
        // uninit_initialize -> 0
        // file_backed_initializer -> 2
        struct page *p = (struct page *)malloc(sizeof(struct page));
        p->writable = writable;
        p->va = upage;

        /* 포인터 타입 함수 */
        typedef bool (*initializerFunc)(struct page *, enum vm_type, void *);
        initializerFunc initializer = NULL;

        switch (VM_TYPE(type)) {
            case VM_ANON:
                initializer = anon_initializer;
                break;
            case VM_FILE:
                initializer = file_backed_initializer;
                break;
            case VM_PAGE_CACHE:
                PANIC("쌈@뽕하게 Proj4는 무시");
                break;
            default:
                PANIC("undefined vm_type SH@IT");
                break;
        }

        uninit_new(page, upage, init, type, aux, initializer);

        /* TODO: Insert the page into the spt. */
        return spt_insert_page(spt, page);
    }
err:
    return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *spt_find_page(struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
    struct page *page = NULL;

    if (va == NULL || spt == NULL) {
        return NULL;
    }

    struct page fake_page;
    fake_page.va = va;
    struct hash_elem *elem = hash_find(&spt->spt_hash, &fake_page.hash_elem);

    if (elem == NULL) {
        return NULL;
    }
    return hash_entry(elem, struct page, hash_elem);
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table *spt UNUSED, struct page *page UNUSED) {
    if (spt == NULL || page == NULL) {
        return false;
    }

    if (spt_find_page(spt, page->va) != NULL) {
        return false;
    }
    /**
     * hash_insert를 보면  내부에서 먼저 값을 찾고 없으면 NULL을 리턴한다(old_ptr 부분)
     * 즉 삽입 성공하면 리턴값이 NULL 아니면 이전에 기록된 ptr을 가져온다.
     */
    page->owner = thread_current();  // 교체 알고리즘에서 pml4 검사할 때 사용하는 역참조용 객체
    return hash_insert(&spt->spt_hash, &page->hash_elem) == NULL;
}

void spt_remove_page(struct supplemental_page_table *spt, struct page *page) {
    if (spt == NULL || page == NULL) {
        return;
    }
    hash_delete(&spt->spt_hash, &page->hash_elem);
    vm_dealloc_page(page);
}

// 시계 방향으로 돌기 위함
void advance_clock_hand() {
    clock_hand = list_next(clock_hand);
    if (clock_hand == list_end(&frame_table)) {
        clock_hand = list_begin(&frame_table);
    }
}

/* Get the struct frame, that will be evicted. */
static struct frame *vm_get_victim(void) {
    struct frame *victim = NULL;

    lock_acquire(&frame_table);

    if (list_empty(&frame_table)) {
        lock_release(&frame_table_lock);
        return NULL;
    }

    if (clock_hand == NULL) {
        clock_hand = list_begin(&frame_table);
    }

    while (true) {
        struct frame *f = list_entry(clock_hand, struct frame, elem);
        void *va = f->page->va;
        struct thread *t = f->page->owner;
        uint64_t *pml4 = t->pml4;

        if (pml4_is_accessed(pml4, va)) {
            pml4_set_accessed(pml4, va, false);
            advance_clock_hand();
        } else {
            victim = f;
            advance_clock_hand();
            break;
        }
    }

    lock_release(&frame_table);

    return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *vm_evict_frame(void) {
    struct frame *victim UNUSED = vm_get_victim();
    /* TODO: swap out the victim and return the evicted frame. */
    swap_out(victim->page);
    return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *vm_get_frame(void) {
    struct frame *frame = (struct frame *)malloc(sizeof(struct frame));

    ASSERT(frame != NULL);
    ASSERT(frame->page == NULL);

    frame->kva = palloc_get_page(PAL_USER);
    if (frame->kva == NULL) {
        frame = vm_evict_frame();
        frame->page = NULL;
        return frame;
    }

    lock_acquire(&frame_table_lock);
    list_push_back(&frame_table, &frame->elem);
    lock_release(&frame_table_lock);

    return frame;
}

/* Growing the stack. */
static void vm_stack_growth(void *addr UNUSED) {}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page *page UNUSED) {}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr UNUSED, bool user UNUSED,
                         bool write UNUSED, bool not_present UNUSED) {
    struct supplemental_page_table *spt UNUSED = &thread_current()->spt;
    struct page *page = NULL;
    /* TODO: Validate the fault */
    /* TODO: Your code goes here */

    return vm_do_claim_page(page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page) {
    destroy(page);
    free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void *va UNUSED) {
    struct page *page = NULL;

    page = spt_find_page(&thread_current()->spt, va);

    if (page != NULL) {
        return vm_do_claim_page(page);
    }

    return false;
}

/* Claim the PAGE and set up the mmu. */
static bool vm_do_claim_page(struct page *page) {
    struct frame *frame = vm_get_frame();

    // 페이지 할당 실패.....
    if (frame == NULL) {
        return false;
    }

    /* Set links */
    frame->page = page;
    page->frame = frame;

    /* 사용자 가상 주소를 커널 주소 맵핑 */
    if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable)) {
        return false;
    }

    return swap_in(page, frame->kva);
}
uint64_t hash_page_func(const struct hash_elem *e, void *aux) {
    struct page *p = hash_entry(e, struct page, hash_elem);
    return hash_bytes(&p->va, sizeof p->va);
}

bool page_less_func(const struct hash_elem *a, const struct hash_elem *b, void *aux) {
    struct page *ap = hash_entry(a, struct page, hash_elem);
    struct page *bp = hash_entry(b, struct page, hash_elem);
    return ap->va < bp->va;
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt UNUSED) {
    hash_init(spt, hash_page_func, page_less_func, NULL);
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table *dst UNUSED,
                                  struct supplemental_page_table *src UNUSED) {}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt UNUSED) {
    /* TODO: Destroy all the supplemental_page_table hold by thread and
     * TODO: writeback all the modified contents to the storage. */
}
