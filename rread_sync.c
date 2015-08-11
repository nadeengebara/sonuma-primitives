/** 
 * OBSOLETE! use sonuma_rread_sync.c instead
 */

//ALEX: This is a flexible sync rread microbenchmark. It slightly modifies the soNUMA protocol and assumes the simulator
//knows we are running this flexible protocol version. Instead of context offset and buffer address, the app gives a
//single offset number. The simulator decides what this means: depending on the operation size (64B, 128B, etc..) it
//multiplies this size with the offset and decides what the real target address is for the buffer and the context

#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <sys/mman.h>
#include "libsonuma/sonuma.h"
#include "magic_iface.h"

#define ITERS 100000

rmc_wq_t *wq;
rmc_cq_t *cq;;

uint32_t op_count = 0;


int main(int argc, char **argv)
{
    int i, it_count = 0, retcode; 

    int num_iter = (int)ITERS;
    if (argc != 4) {
        fprintf(stdout,"Usage: ./uBench <target_nid> <context_size> <buffer_size>\n");
        return 1;  
    }
    int snid = atoi(argv[1]);
    uint64_t ctx_size = atoi(argv[2]);
    uint64_t buf_size = atoi(argv[3]);

    uint8_t *lbuff, *ctxbuff;  
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
        PASS2FLEXUS_CONFIG((uint64_t)&(lbuff[i]), BUFFER, counter);
    }

    //context buffer - exposed to remote nodes
    //if (snid == 1) {//WARNING: Only app that is given snid = 1 registers context
    ctxbuff = memalign(PAGE_SIZE, ctx_size*sizeof(uint8_t));
    if (ctxbuff == NULL) {
        fprintf(stdout, "Context buffer could not be allocated. Memalign returned %"PRIu64"\n", 0);
        return 1;
    } 
    retcode = mlock(ctxbuff, ctx_size*sizeof(uint8_t));
    if (retcode != 0) fprintf(stdout, "Context buffer mlock returned %d\n", retcode);

    counter = 0;
    //initialize the context buffer
    ctxbuff[0] = 123;
    PASS2FLEXUS_CONFIG((uint64_t)ctxbuff, CONTEXTMAP, 0);
    for(i=0; i<ctx_size; i+=PAGE_SIZE) {
        *(ctxbuff + i) = 123;
        counter++;
        PASS2FLEXUS_CONFIG((uint64_t)&(ctxbuff[i]), CONTEXT, counter);
    } 
    //fprintf(stdout, "Allocated %d pages for the context\n", counter);
    //}

    //allocate queues
    //wq = memalign(PAGE_SIZE, sizeof(rmc_wq_t));
    wq = memalign(PAGE_SIZE, PAGE_SIZE);//Should allocate full page
    if (wq == NULL) {
        fprintf(stdout, "Work Queue could not be allocated. Memalign returned %"PRIu64"\n", 0);
        return 1;
    } 
    //retcode = mlock(wq, sizeof(rmc_wq_t));
    retcode = mlock(wq, PAGE_SIZE);
    if (retcode != 0)  fprintf(stdout, "WQueue mlock returned %d\n", retcode);

    //cq = memalign(PAGE_SIZE, sizeof(rmc_cq_t));//Should allocate full page
    cq = memalign(PAGE_SIZE, PAGE_SIZE);
    if (cq == NULL) {
        fprintf(stdout, "Completion Queue could not be allocated. Memalign returned %"PRIu64"\n", 0);
        return 1;
    } 
    //retcode = mlock(cq, sizeof(rmc_cq_t));
    retcode = mlock(cq, PAGE_SIZE);
    if (retcode != 0) fprintf(stdout, "CQueue mlock returned %d\n", retcode);

    uint8_t operation = (uint8_t)RMC_INVAL;
    //setup work queue
    wq->head = 0;
    wq->SR = 1;

    for(i=0; i<MAX_NUM_WQ; i++) {
        wq->q[i].SR = 0;
    }
    PASS2FLEXUS_CONFIG((uint64_t)wq, WQUEUE, MAX_NUM_WQ);

    //  fprintf(stdout, "Starting address of wq is %"PRIu64"\nAddresses of first entry's fields are:\nop=%"PRIu64"\nnid=%"PRIu64"\ntid=%"PRIu64"\ncid=%"PRIu64"\noffset=%"PRIu64"\nlength=%"PRIu64"\nbuff=%"PRIu64, (uint64_t)wq, (uint64_t)&(wq->q[0].op), (uint64_t)&(wq->q[0].nid), (uint64_t)&(wq->q[0].tid), (uint64_t)&(wq->q[0].cid), (uint64_t)&(wq->q[0].offset), (uint64_t)&(wq->q[0].length), (uint64_t)&(wq->q[0].buff));
    /*
       fprintf(stdout, "[uB] size of the WQ is %d\n", sizeof(rmc_wq_t));
       fprintf(stdout, "[uB] size of the CQ is %d\n", sizeof(rmc_cq_t));
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
    PASS2FLEXUS_CONFIG((uint64_t)cq, CQUEUE, MAX_NUM_WQ);

    //register ctx and buffer sizes, needed for the flexi version of the app
    PASS2FLEXUS_CONFIG(42, BUFFER_SIZE, buf_size);
    PASS2FLEXUS_CONFIG(42, CONTEXT_SIZE, ctx_size);

    fprintf(stdout,"Init done! Will execute %"PRIu64" WQ operations - SYNC!\n NOTE: This app is in FLEXI mode!", num_iter);
    PASS2FLEXUS_CONFIG(1, ALL_SET, 1);

    //uB kernel
    uint8_t wq_head; 
    uint32_t cq_tail;
    uint8_t tid;

    //fprintf(stdout,"WQ head is %"PRIu64" and WQ tail is %"PRIu64"\n", wq->head, wq->tail);
    //fprintf(stdout,"The address of the WQ structure is %"PRIu64", the address of WQ head is %"PRIu64", and the address of WQ tail is %"PRIu64"\n",(uint64_t)wq, &(wq->head), &(wq->tail));
    /* if (snid == 0) {
       fprintf(stdout, "This node is a server\n");
       while (1) ;//this node will just be a server
       }*/
    while(op_count < num_iter) {
        lbuff_slot = op_count;//(lbuff + ((op_count * SLOT_SIZE) % buf_size));
        ctx_offset = op_count;//((snid << 16) + op_count * SLOT_SIZE) % ctx_size;
        //////////////////////
        wq_head = wq->head;
        //fprintf(stdout, "wq_head is %d, and Queue Entry that was just enqueued has: \n op = %d\n, SR = %d\n, cid = %d\n, nid = %d\n, buf_addr=%ld\n, offset=%ld\n, length = %ld\n", wq_head, wq->q[wq_head].op, wq->q[wq_head].SR, wq->q[wq_head].cid, wq->q[wq_head].nid, wq->q[wq_head].buf_addr, wq->q[wq_head].offset, wq->q[wq_head].length);
        //fprintf(stdout, "About to write a WQ entry with \n  op = %d\n, SR = %d\n, cid = 0\n, nid = %d\n, buf_addr=%ld\n, offset=%ld\n, length = 42\n", RMC_READ, wq->SR, snid, lbuff_slot, ctx_offset);
        PASS2FLEXUS_CONFIG(wq_head, NEWWQENTRY_START, op_count);
        create_wq_entry(RMC_READ, wq->SR, 0, snid, (uint64_t)lbuff_slot, ctx_offset, 42, (uint64_t)&(wq->q[wq_head]));
        // create_wq_entry(1, 0, 3, 4, 5, 6, 7, (uint64_t)&(wq->q[wq_head]));
        PASS2FLEXUS_CONFIG(wq_head, NEWWQENTRY, op_count);

        //  fprintf(stdout, "wq_head is %d, and Queue Entry that was just enqueued has: \n op = %d\n, SR = %d\n, cid = %d\n, nid = %d\n, buf_addr=%ld\n, offset=%ld\n, length = %ld, valid = %d\n", wq_head, wq->q[wq_head].op, wq->q[wq_head].SR, wq->q[wq_head].cid, wq->q[wq_head].nid, wq->q[wq_head].buf_addr, wq->q[wq_head].offset, wq->q[wq_head].length, wq->q[wq_head].valid);

        wq->head =  wq->head + 1;
        if (wq->head >= MAX_NUM_WQ) {
            wq->head = 0;
            wq->SR ^= 1;
        }

        // fprintf(stdout,"WQ head is %"PRIu64" and WQ tail is %"PRIu64"\n", wq->head, wq->tail);
        // fprintf(stdout, "CQ tid of first entry is at %"PRIu64", buff of first entry is at %"PRIu64\
        "\nop tid second entry is at %"PRIu64", buff of second entry is at %"PRIu64"\n", \
            &(cq->q[0].tid), &(cq->q[0].buff), &(cq->q[1].tid), &(cq->q[1].buff));
        //sync
        cq_tail = cq->tail;
        //    fprintf(stdout,"Will now poll CQ address: %"PRIu64"\n", &(cq->q[cq_tail]));
        while(cq->q[cq_tail].SR != cq->SR) {
            //  fprintf(stdout,"cq->q[%"PRIu8"].SR = %"PRIu8"\tcq->q[%"PRIu8"].tid = %"PRIu8"\n", cq_tail, cq->q[cq_tail].SR, cq_tail, cq->q[cq_tail].tid);
        } 
#ifdef version4_1
        wq->q[cq->q[cq_tail].tid].valid = 0;
#endif
        PASS2FLEXUS_CONFIG(cq_tail, WQENTRYDONE, op_count);

        //  lbuff_slot = (uint8_t*)wq->q[cq->q[cq_tail].tid].buf_addr;
        //  fprintf(stdout,"Reading local buffer. Value: %"PRIu8"\n", *lbuff_slot);
        cq->tail = cq->tail + 1;
        if (cq->tail >= MAX_NUM_WQ) {
            cq->tail = 0;
            cq->SR ^= 1;
        }    
        /////////////////////
        op_count++;
    }

    free(lbuff);
    free(wq);
    free(cq);

    return 0;
}
