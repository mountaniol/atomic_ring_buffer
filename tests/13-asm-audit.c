/* Anchor binary for the codegen audit: taking every audited function's
 * address forces LTO to keep a standalone out-of-line copy of each - the
 * code an external caller of libringbuf.a actually executes. */
#include "ring_buf.h"

/* volatile: the loads below cannot be folded, so every address must be
 * materialized and every function emitted out of line */
void *volatile rb_keep[] = {
    (void *)rb_push_int,   (void *)rb_pull_int,
    (void *)rb_push_int_burst, (void *)rb_pull_int_burst,
    (void *)rb_push_ptr,   (void *)rb_pull_ptr,
    (void *)rb_batch_size,
    (void *)rb_push_wait,  (void *)rb_pull_wait, (void *)rb_wake,
};

int main(void)
{
    int live = 0;
    for (unsigned i = 0; i < sizeof(rb_keep) / sizeof(rb_keep[0]); i++)
        live += rb_keep[i] != 0;
    return live == 10 ? 0 : 1;
}
