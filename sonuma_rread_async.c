#define __STDC_FORMAT_MACROS
#include "libsonuma/sonuma.h"

#define ITERS 100000

rmc_wq_t *wq;
rmc_cq_t *cq;

uint64_t op_count_issued;
uint64_t op_count_completed;

void handler(uint8_t tid, wq_entry_t head, void *owner) {
    // do nothing
}

int main(int argc, char **argv)
{
    int num_iter = (int)ITERS;
    if (argc != 4) {
        fprintf(stdout,"Usage: ./uBench <target_nid> <context_size> <buffer_size>\n");
        return 1;
    }
    int snid = atoi(argv[1]);
    uint64_t ctx_size = atoi(argv[2]);
    uint64_t buf_size = atoi(argv[3]);

    uint8_t *lbuff, *ctx;
    uint64_t lbuff_slot;
    uint64_t ctx_offset;

    //local buffer
    lbuff = memalign(PAGE_SIZE, buf_size*sizeof(uint8_t));
    if (lbuff == NULL) {
        fprintf(stdout, "Local buffer could not be allocated.\n");
        return 1;
    }

    uint32_t num_pages =  buf_size * sizeof(uint8_t) / PAGE_SIZE;
    fprintf(stdout, "Local buffer was allocated by address %p, number of pages is %d\n", lbuff, num_pages);

    kal_reg_lbuff(0, &lbuff, num_pages);
    fprintf(stdout, "Local buffer was registered.\n");

    ctx = memalign(PAGE_SIZE, ctx_size*sizeof(uint8_t));
    if (ctx == NULL) {
        fprintf(stdout, "Context buffer could not be allocated.\n");
        return 1;
    }
    kal_reg_ctx(0, &ctx, ctx_size*sizeof(uint8_t) / PAGE_SIZE);
    fprintf(stdout, "Ctx buffer was registered.\n");

    kal_reg_wq(0, &wq);
    fprintf(stdout, "WQ was registered.\n");

    kal_reg_cq(0, &cq);
    fprintf(stdout, "CQ was registered.\n");

    fprintf(stdout,"Init done! Will execute %d WQ operations - ASYNC!\n NOTE: This app is in FLEXI mode! (snid = %d)\n", num_iter, snid);
    flexus_signal_all_set();

    //uB kernel
    while(op_count_completed < num_iter) {
        rmc_check_cq(wq, cq, &handler, NULL);
        lbuff_slot = op_count_issued;    //(void *)(lbuff + ((op_count_issued * SLOT_SIZE) % buf_size));
        ctx_offset = op_count_issued + ((snid-1) << 20);// + op_count_issued * SLOT_SIZE) % ctx_size;
        rmc_rread_async(wq, lbuff_slot, snid, 0, ctx_offset, 42);
    }

free(lbuff);
free(ctx);
free(wq);
free(cq);

return 0;
}
