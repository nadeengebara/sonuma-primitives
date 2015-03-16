#define __STDC_FORMAT_MACROS
#include "libsonuma/sonuma.h"
#include <unistd.h>
#include <sys/mman.h>
#include <cassert>

#define ITERS           100000
// SLOT_SIZE must be >= OBJ_READ_SIZE
#define SLOT_SIZE       128
#define OBJ_READ_SIZE   128
// only a single context is used
#define CTX_0           0

rmc_wq_t *wq;
rmc_cq_t *cq;

const char* br_points[] = {
    "BLANK",            // 0 
    "WQUEUE",           // 1 
    "CQUEUE",           // 2 
    "BUFFER",           // 3 
    "PTENTRY",          // 4 
    "NEWWQENTRY",       // 5 
    "WQENTRYDONE",      // 6 
    "ALL_SET",          // 7 
    "TID_COUNTERS",     // 8 
    "CONTEXT",          // 9 
    "CONTEXTMAP",       // 10
    "WQHEAD",           // 11
    "WQTAIL",           // 12
    "CQHEAD",           // 13
    "CQTAIL",           // 14
    "SIM_BREAK",        // 15
    "NETPIPE_START",    // 16
    "NETPIPE_END",      // 17
    "RMC_DEBUG_BP",     // 18
    "PAGERANK_END",     // 19
    "BUFFER_SIZE",      // 20
    "CONTEXT_SIZE",     // 21
    "NEWWQENTRY_START"  // 22
}; 

#ifdef DEBUG_FLEXUS_STATS
// global variables from sonuma.h
uint64_t op_count_issued;
uint64_t op_count_completed;
#endif

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
    fprintf(stdout, "Ctx buffer was registered by addr=%p, ctx_size=%d, %d pages.\n", ctx, ctx_size, ctx_size*sizeof(uint8_t) / PAGE_SIZE);

    kal_reg_wq(0, &wq);
    fprintf(stdout, "WQ was registered.\n");

    kal_reg_cq(0, &cq);
    fprintf(stdout, "CQ was registered.\n");

    fprintf(stdout,"Init 0 done! ");
    call_magic_sim_break(3, 2, 1);
    fprintf(stdout,"Init done! Will execute %d WQ operations - SYNC! (snid = %d)\n", num_iter, snid);
    flexus_signal_all_set(1);

    //uB kernel
    for(size_t i = 0; i < num_iter; i++) {
        lbuff_slot = (void *)( lbuff + ((i * SLOT_SIZE) % (buf_size - SLOT_SIZE)) );
        ctx_offset = (i * SLOT_SIZE) % (ctx_size - SLOT_SIZE);
        rmc_rread_sync(wq, cq, lbuff_slot, snid, CTX_0, ctx_offset, OBJ_READ_SIZE);
#ifdef DEBUG_PERF
        assert(((uint64_t *)lbuff_slot)[0] == 0x7B7B7B7B7B7B7B7B); // all bytes should be equal to DEFAULT_CTX_VALUE=0x7B
#endif
        DLogPerf("first byte: %"PRIu64, ((uint64_t *)lbuff_slot)[0]);
    }

free(lbuff);
free(ctx);
free(wq);
free(cq);

return 0;
}
