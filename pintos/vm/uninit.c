/* uninit.c: Implementation of uninitialized page.
 *
 * All of the pages are born as uninit page. When the first page fault occurs,
 * the handler chain calls uninit_initialize (page->operations.swap_in).
 * The uninit_initialize function transmutes the page into the specific page
 * object (anon, file, page_cache), by initializing the page object,and calls
 * initialization callback that passed from vm_alloc_page_with_initializer
 * function.
 * */

#include "vm/uninit.h"

#include "vm/vm.h"

static bool uninit_initialize(struct page *page, void *kva);
static void uninit_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations uninit_ops = {
    .swap_in = uninit_initialize,
    .swap_out = NULL,
    .destroy = uninit_destroy,
    .type = VM_UNINIT,
};

/* DO NOT MODIFY this function */
void uninit_new(struct page *page, void *va, vm_initializer *init, enum vm_type type, void *aux,
                bool (*initializer)(struct page *, enum vm_type, void *)) {
    ASSERT(page != NULL);

    *page = (struct page){.operations = &uninit_ops,
                          .va = va,
                          .frame = NULL, /* no frame for now */
                          .uninit = (struct uninit_page){
                              .init = init,
                              .type = type,
                              .aux = aux,
                              .page_initializer = initializer,
                          }};
}

bool fragment_uninit_initialize(struct page *page, void *kva) {
    return uninit_initialize(page, kva);
}

/* Initalize the page on first fault */
static bool uninit_initialize(struct page *page, void *kva) {
    struct uninit_page *uninit = &page->uninit;

    /* Fetch first, page_initialize may overwrite the values */
    vm_initializer *init = uninit->init;
    void *aux = uninit->aux;

    /* TODO: You may need to fix this function. */
    return uninit->page_initializer(page, uninit->type, kva) && (init ? init(page, aux) : true);
}

/* Free the resources hold by uninit_page. Although most of pages are transmuted
 * to other page objects, it is possible to have uninit pages when the process
 * exit, which are never referenced during the execution.
 * PAGE will be freed by the caller. */
/* page 자체를 지우는 것이 아니라, uninit 상태의 페이지가
 * 내부적으로 들고 있던 리소스들을 정리해야 한다. */
static void uninit_destroy(struct page *page) {
    struct uninit_page *uninit UNUSED = &page->uninit;

    ASSERT(page->frame == NULL);

    if (page->uninit.aux != NULL) {
        free(page->uninit.aux);
    }

    /* page free 금지, vm_dealloc_page()금지 ㅜ*/
    switch (VM_TYPE(page_get_type(page))) {
        case VM_ANON:
            palloc_free_page(page);
            break;
        case VM_FILE:

            break;
        default:
            page->uninit.aux = NULL;
    }
}
