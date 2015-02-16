/** 
 * OBSOLETE! use sonuma_rread_async.c instead
 */


#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <sys/mman.h>
#include "libsonuma/sonuma.h"

#define ITERS 100000

rmc_wq_t *wq;
rmc_cq_t *cq;

uint64_t op_count_issued;
uint64_t op_count_completed;

int main(int argc, char **argv)
{
    op_count_issued = 0, op_count_completed = 0;
    int i, j, retcode;
    uint8_t tid;

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
        fprintf(stdout, "Local buffer could not be allocated. Memalign returned %"PRIu64"\n", 0);
        return 1;
    }
    retcode = mlock(lbuff, buf_size*sizeof(uint8_t));
    if (retcode != 0) fprintf(stdout, "Local buffer mlock returned %d (buffer size = %d bytes)\n", retcode, buf_size*sizeof(uint8_t));

    uint32_t counter = 0;
    //initialize the local buffer
    for(i=0; i<(buf_size*sizeof(uint8_t)); i++) {
        lbuff[i] = 0;
        counter = i*sizeof(uint8_t)/PAGE_SIZE;
        call_magic_2_64((uint64_t)&(lbuff[i]), BUFFER, counter);
    }

    //context buffer - exposed to remote nodes

    //if (snid == 1) {    //WARNING: Only app that is given snid = 1 registers context
    ctx = memalign(PAGE_SIZE, ctx_size*sizeof(uint8_t));
    if (ctx == NULL) {
        fprintf(stdout, "Context buffer could not be allocated. Memalign returned %"PRIu64"\n", 0);
        return 1;
    }
    retcode = mlock(ctx, ctx_size*sizeof(uint8_t));
    if (retcode != 0) fprintf(stdout, "Context buffer mlock returned %d\n", retcode);

    counter = 0;
    //initialize the context buffer
    ctx[0] = 123;
    call_magic_2_64((uint64_t)ctx, CONTEXTMAP, 0);
    for(i=0; i<ctx_size; i+=PAGE_SIZE) {
        *(ctx + i) = 123;
        counter++;
        call_magic_2_64((uint64_t)&(ctx[i]), CONTEXT, counter);
    }
    //}
    //allocate queues

    wq = memalign(PAGE_SIZE, PAGE_SIZE);
    if (wq == NULL) {
        fprintf(stdout, "Work Queue could not be allocated. Memalign returned %"PRIu64"\n", 0);
        return 1;
    }
    retcode = mlock(wq, PAGE_SIZE);
    if (retcode != 0) fprintf(stdout, "WQueue mlock returned %d\n", retcode);

    cq = memalign(PAGE_SIZE, PAGE_SIZE);
    if (cq == NULL) {
        fprintf(stdout, "Completion Queue could not be allocated. Memalign returned %"PRIu64"\n", 0);
        return 1;
    }
    retcode = mlock(cq, PAGE_SIZE);
    if (retcode != 0) fprintf(stdout, "CQueue mlock returned %d\n", retcode);

    //setup work queue
    wq->head = 0;
    wq->SR = 1;

    for(i=0; i<MAX_NUM_WQ; i++) {
        wq->q[i].SR = 0;
    }
    call_magic_2_64((uint64_t)wq, WQUEUE, MAX_NUM_WQ);

    //fprintf(stdout, "Starting address of wq is %"PRIu64"\nAddresses of first entry's fields are:\n\op=%"PRIu64"\nnid=%"PRIu64"\ntid=%"PRIu64"\ncid=%"PRIu64"\noffset=%"PRIu64"\nlength=%"PRIu64"\nbuff=%"PRIu64, (uint64_t)wq, (uint64_t)&(wq->q[0].op), (uint64_t)&(wq->q[0].nid), (uint64_t)&(wq->q[0].tid), (uint64_t)&(wq->q[0].cid), (uint64_t)&(wq->q[0].offset), (uint64_t)&(wq->q[0].length), (uint64_t)&(wq->q[0].buff));
    /* fprintf(stdout, "Starting address of wq is %"PRIu64"\n", (uint64_t)wq);

       fprintf(stdout, "[uB] size of the WQ is %d\n", sizeof(rmc_wq_t));
       fprintf(stdout, "[uB] size of a single WQ entry is %d\n", sizeof(wq_entry_t));
       fprintf(stdout, "[uB] size of local buffer is %d\n", buf_size);
       fprintf(stdout, "[uB] size of context is %d\n", ctx_size);
       */
    //setup completion queue
    cq->tail = 0;
    cq->SR = 1;

    for(i=0; i<MAX_NUM_WQ; i++) {
        cq->q[i].SR = 0;
    }
    call_magic_2_64((uint64_t)cq, CQUEUE, MAX_NUM_WQ);
    //  fprintf(stdout, "Starting address of cq is %"PRIu64"\n", (uint64_t)cq);

    //register ctx and buffer sizes, needed for the flexi version of the app
    call_magic_2_64(42, BUFFER_SIZE, buf_size);
    call_magic_2_64(42, CONTEXT_SIZE, ctx_size);

    fprintf(stdout,"Init done! Will execute %"PRIu64" WQ operations - ASYNC!\n NOTE: This app is in FLEXI mode! (snid = %d)\n", num_iter, snid);
    call_magic_2_64(1, ALL_SET, 1);

    //uB kernel
    uint8_t wq_head, cq_tail;

    while(op_count_completed < num_iter) {
        //sync
        //fprintf(stdout,"wq_head is %"PRIu8"\n", wq->head);
        wq_head = wq->head;
        cq_tail = cq->tail;

        do {
            while (cq->q[cq_tail].SR == cq->SR) {
                tid = cq->q[cq_tail].tid;
#ifdef version2_1
                wq->q[tid].valid = 0;
#endif
                //do whatever is needed with tid here
                op_count_completed++;

                cq->tail = cq->tail + 1;
                if (cq->tail >= MAX_NUM_WQ) {
                    cq->tail = 0;
                    cq->SR ^= 1;
                }
                call_magic_2_64(tid, WQENTRYDONE, op_count_completed);
                cq_tail = cq->tail;
            }
#ifdef version2_1
        } while (wq->q[wq_head].valid);
#else
    } while (wq->SR != cq->SR);
#endif

    //schedule
    lbuff_slot = op_count_issued;    //(void *)(lbuff + ((op_count_issued * SLOT_SIZE) % buf_size));
    ctx_offset = op_count_issued + ((snid-1) << 20);// + op_count_issued * SLOT_SIZE) % ctx_size;
    //fprintf(stdout,"buf offset = %"PRIu64"\n", ctx_offset);
    wq_head = wq->head;

    create_wq_entry(RMC_READ, wq->SR, 0, snid, (uint64_t)lbuff_slot, ctx_offset, 42, (uint64_t)&(wq->q[wq_head]));
    op_count_issued++;
    call_magic_2_64(wq_head, NEWWQENTRY, op_count_issued);

    wq->head =  wq->head + 1;
    if (wq->head >= MAX_NUM_WQ) {
        wq->head = 0;
        wq->SR ^= 1;
    }
}

free(lbuff);
free(ctx);
free(wq);
free(cq);

return 0;
}
