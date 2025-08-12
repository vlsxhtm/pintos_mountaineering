/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"

#include <inttypes.h>

#include "devices/disk.h"
#include "kernel/hash.h"
#include "kernel/list.h"
#include "string.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "vm/anon.h"
#include "vm/file.h"
#include "vm/inspect.h"
#include "vm/uninit.h"

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
    lock_init(&frame_table_lock);
    clock_hand = NULL;
    // disk_init();  // vm_anon_init()에서 swap 영역 지정할 때 사용하기 위해서 여기서 초기화함
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

        uninit_new(p, upage, init, type, aux, initializer);
        p->writable = writable;
        /* TODO: Insert the page into the spt. */
        return spt_insert_page(spt, p);
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
    fake_page.va = pg_round_down(va);
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

    lock_acquire(&frame_table_lock);

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

    lock_release(&frame_table_lock);

    return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/

static struct frame *vm_evict_frame(void) {
    struct frame *victim = vm_get_victim();
    if (victim == NULL || victim->page == NULL) {
        return NULL;
    }

    struct page *p = victim->page;
    struct thread *t = p->owner;

    swap_out(p);
    pml4_clear_page(t->pml4, p->va);
    p->frame = NULL;
    victim->page = NULL;
    return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *vm_get_frame(void) {
    struct frame *frame = (struct frame *)malloc(sizeof(struct frame));

    ASSERT(frame != NULL);
    frame->page = NULL;

    frame->kva = palloc_get_page(PAL_USER);
    if (frame->kva == NULL) {
        frame = vm_evict_frame();
        if (frame == NULL)
            return NULL;
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
bool vm_try_handle_fault(struct intr_frame *f, void *addr, bool user, bool write,
                         bool not_present) {
    void *va = pg_round_down(addr);
    struct supplemental_page_table *spt = &thread_current()->spt;
    struct page *page = spt_find_page(spt, va);
    if (user && is_kernel_vaddr(addr)) {
        return false;
    }
    if (addr < 0x400000 || (addr > USER_STACK)) {
        return false;
    }

    if (page == NULL) {
        // TODO: stack_growth 용
        return false;
    }
    if (write && !page->writable) {
        return false;
    }

    if (!not_present) {
        // TODO:cow 용
        return false;
    }
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

uint64_t hash_page_func(const struct hash_elem *e, void *aux UNUSED) {
    struct page *p = hash_entry(e, struct page, hash_elem);
    void *key = pg_round_down(p->va);
    return hash_bytes(&key, sizeof key);  // x86-64에선 8바이트
}

bool page_less_func(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED) {
    struct page *ap = hash_entry(a, struct page, hash_elem);
    struct page *bp = hash_entry(b, struct page, hash_elem);
    uintptr_t av = (uintptr_t)pg_round_down(ap->va);
    uintptr_t bv = (uintptr_t)pg_round_down(bp->va);
    return av < bv;
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt UNUSED) {
    hash_init(&spt->spt_hash, hash_page_func, page_less_func, NULL);
}

/* Copy supplemental page table from src to dst */
/*
 * 이 함수는 자식이 부모의 실행 컨텍스트를 상속해야 할 때 사용된다.
 * fork() 함수처럼 자식 프로세스가 생성될 때의 과정을 생각하면 더 이해하기 쉬울 것 같다.
 * src의 각각 페이지를 반복하면서 dst의 엔트리에 정확하게 복사한다.
 * 이 과정에서 초기화되지 않은 페이지들은 할당하고 바로 claim해야 한다.
 */
bool supplemental_page_table_copy(struct supplemental_page_table *dst UNUSED,
                                  struct supplemental_page_table *src UNUSED) {
    // hash_first/hash_next로 src->spt_hash의 모든 페이지 엔트리를 순회함
    // struct hash_iterator i;
    // hash_first(&i, &src->spt_hash);
    // while (hash_next(&i)) {
    //     // 매 반복마다 아래와 같은 데이터를 꺼냄
    //     struct page *parent_page = hash_entry(hash_cur(&i), struct page, hash_elem);
    //     enum vm_type type = page_get_type(parent_page);   // 부모 페이지의 타입(ANON/FILE/UNIT
    //     등) void *upage = parent_page->va;                    // 가상주소 bool writable =
    //     parent_page->writable;            // 쓰기 가능 여부(페이지 권한) vm_initializer *init =
    //     parent_page->uninit.init;  // UNINIT일 때 초기화 함수 void *aux =
    //     parent_page->uninit.aux;  // UNINIT일 때 보조 데이터 (레이지 세그먼트 용)

    //     // uninit.type 안에 특수 마커 플래그 VM_MARKER_0이 켜져 있느냐를 확인하는 거임
    //     if (parent_page->uninit.type & VM_MARKER_0) {
    //         // setup_stack(&thread_current()->tf);
    //         // 부모 페이지가 uninit 상태 일 때 실제 프레임은 없고, fault 때 로드될 예정임
    //     } else if (parent_page->operations->type == VM_UNINIT) {
    //         // 나중에 fault 나면 이렇게 채워라 하는 거임
    //         if (!vm_alloc_page_with_initializer(type, upage, writable, init, aux)) {
    //             return false;
    //         }
    //         // 이미 물리 프레임이 있던 페이지일 경우
    //     } else {
    //         // 자식 SPT에 실제 타입 페이지 엔트리를 만듦
    //         if (!vm_alloc_page(type, upage, writable)) {
    //             return false;
    //         }

    //         // 자식 쪽에서 프레임을 즉시 할당하고 매핑함
    //         if (!vm_claim_page(upage)) {
    //             return false;
    //         }
    //     }

    //     // 부모가 uninit 상태가 아니니까 부모 프레임에 실제 내용이 있을 때
    //     if (parent_page->operations->type != VM_UNINIT) {
    //         // 부모 프레임의 내용을 자식 프레임 kva로 그대로 복사함 (페이지 단위로)
    //         // 그로인해 자식의 페이지 내용이 부모와 동일해짐
    //         struct page *child_page = spt_find_page(dst, upage);
    //         memcpy(child_page->frame->kva, parent_page->frame->kva, PGSIZE);
    //     }
    // }

    // 요약
    // 1. 스택 마커면 스택 준비함
    // 2. UNINIT이면 vm_alloc_page_with_initializer로 lazy 메타 복제
    // 3. 그 외(이미 로드된 페이지)면 vm_alloc_page + vm_claim_page로 프레이 생성 후 memcpy로 내용
    // 복제 (3번에 이어지는 내용인데;; 줄바꿈됨)
    // 4. 모든 순회 끝나면 true 반환

    return true;
}

static void page_destroy_all(struct hash_elem *e, void *aux UNUSED) {
    struct page *page = hash_entry(e, struct page, hash_elem);

    /* dealloc을 해줌*/
    vm_dealloc_page(page);
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt UNUSED) {
    /* TODO: Destroy all the supplemental_page_table hold by thread and
     * TODO: writeback all the modified contents to the storage. */
    hash_destroy(&spt->spt_hash, page_destroy_all);
}
