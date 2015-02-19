/**
 * soNUMA library functions.
 *
 * Copyright (C) EPFL. All rights reserved.
 * @authors daglis, novakovic, ustiugov
 */

#ifndef H_SONUMA
#define H_SONUMA

#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "RMCdefines.h"
#include "magic_iface.h"

#ifndef FLEXUS
#include "soft_rmc.h"
#endif

//#include "son_asm.h"

//#define FLEXUS // ustiugov: comment out to get soNUMA for linux

#define DEBUG
// ustiugov: WARNING!!! DEBUG_PERF enables I/O in performance regions (it uses DLogPerf)!
//           Do not enable during experiments!
//#define DEBUG_PERF
// ustiugov: WARNING!!! DEBUG_STATS enable additional Flexus stats in measurement 
//           phase that impacts the measurements. Do not enable during experiments!
//#define DEBUG_FLEXUS_STATS

#ifdef DEBUG
#define DLog(M, ...) fprintf(stdout, "DEBUG %s:%d: " M "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define DLog(M, ...)
#endif

#ifdef DEBUG_PERF
#define DLogPerf(M, ...) fprintf(stdout, "DEBUG %s:%d: " M "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define DLogPerf(M, ...)
#endif

#ifdef DEBUG_FLEXUS_STATS
// global variables for sonuma operation counters
extern uint64_t op_count_issued;
extern uint64_t op_count_completed;
#endif

static pthread_t rmc_thread;

typedef void (async_handler)(uint8_t tid, wq_entry_t head, void *owner);

// global variable to switch Flexus to timing mode only once
int is_timing = 0;

/////////////////////// IMPLEMENTATION //////////////////////////////
int kal_open(char *kal_name) {
#ifdef FLEXUS
    DLog("[sonuma] kal_open called in FLEXUS mode. Do nothing.");
    return 0; // not used with Flexus
#else
    DLog("[sonuma] kal_open called in VM mode.");
    int fd;

    if ((fd=open(kal_name, O_RDWR|O_SYNC)) < 0) {
        return -1;
    }
    return fd;
#endif
}

int kal_reg_wq(int fd, rmc_wq_t **wq_ptr) {
    int i, retcode;
    // ustiugov: WARNING: due to some Flexus caveats we need a whole page
    //*wq_ptr = (rmc_wq_t *)memalign(PAGE_SIZE, sizeof(rmc_wq_t));
    *wq_ptr = (rmc_wq_t *)memalign(PAGE_SIZE, PAGE_SIZE);
    rmc_wq_t *wq = *wq_ptr;
    if (wq == NULL) {
        DLog("[sonuma] Work Queue could not be allocated.");
        return -1;
    }
    //retcode = mlock((void *)wq, sizeof(rmc_wq_t));
    retcode = mlock((void *)wq, PAGE_SIZE);
    if (retcode != 0) {
        DLog("[sonuma] WQueue mlock returned %d", retcode);
    } else {
        DLog("[sonuma] WQ was pinned successfully.");
    }

    //setup work queue
    wq->head = 0;
    wq->SR = 1;

    for(i=0; i<MAX_NUM_WQ; i++) {
        wq->q[i].SR = 0;
    }

#ifdef FLEXUS
    DLog("[sonuma] Call Flexus magic call (WQUEUE).");
    call_magic_2_64((uint64_t)wq, WQUEUE, MAX_NUM_WQ);
#else
    DLog("[sonuma] kal_reg_wq called in VM mode.");
    //posix_memalign((void **)wq, PAGE_SIZE, sizeof(rmc_wq_t));
    //if(ioctl(fd, KAL_REG_WQ, (void *)wq) == -1) {
    //  return -1;
    //}
#endif /* FLEXUS */

    return 0;
}

int kal_reg_cq(int fd, rmc_cq_t **cq_ptr) {
    int i, retcode;
    // ustiugov: WARNING: due to some Flexus caveats we need a whole page
    //*cq_ptr = (rmc_cq_t *)memalign(PAGE_SIZE, sizeof(rmc_cq_t));
    *cq_ptr = (rmc_cq_t *)memalign(PAGE_SIZE, PAGE_SIZE);
    rmc_cq_t *cq = *cq_ptr;
    if (cq == NULL) {
        DLog("[sonuma] Completion Queue could not be allocated.");
        return -1;
    }
    //retcode = mlock((void *)cq, sizeof(rmc_cq_t));
    retcode = mlock((void *)cq, PAGE_SIZE);
    if (retcode != 0) {
        DLog("[sonuma] CQueue mlock returned %d", retcode);
    } else {
        DLog("[sonuma] CQ was pinned successfully.");
    }

    cq->tail = 0;
    cq->SR = 1;

    for(i=0; i<MAX_NUM_WQ; i++) {
        cq->q[i].SR = 0;
    }
#ifdef FLEXUS
    DLog("[sonuma] Call Flexus magic call (CQUEUE).");
    call_magic_2_64((uint64_t)cq, CQUEUE, MAX_NUM_WQ);
#else
    DLog("[sonuma] kal_reg_cq called in VM mode.");
    //posix_memalign((void **)cq, PAGE_SIZE, sizeof(rmc_cq_t));
    //register completion queue
    //if (ioctl(fd, KAL_REG_CQ, (void *)cq) == -1) {
    //    return -1;
    //}
#endif /* FLEXUS */

    return 0;
}

int kal_reg_lbuff(int fd, uint8_t **buff_ptr, uint32_t num_pages) {
    //assert(buff_ptr != NULL);
    uint8_t *buff = *buff_ptr;
#ifdef FLEXUS
    int i, retcode;
    uint64_t buff_size = num_pages * PAGE_SIZE;
    // buffers allocation is done by app
    retcode = mlock((void *)buff, buff_size * sizeof(uint8_t));
    if (retcode != 0) {
        DLog("[sonuma] Local buffer %p mlock returned %d (buffer size = %"PRIu64" bytes)", *buff_ptr, retcode, buff_size);
    } else {
        DLog("[sonuma] Local buffer was pinned successfully.");
    }

    uint32_t counter = 0;
    //initialize the local buffer
    for(i=0; i<(buff_size*sizeof(uint8_t)); i++) {
        buff[i] = 0;
        counter = i*sizeof(uint8_t)/PAGE_SIZE;
        call_magic_2_64((uint64_t)&(buff[i] ), BUFFER, counter);
    }
    
    DLog("[sonuma] Call Flexus magic call (BUFFER_SIZE).");
    call_magic_2_64(42, BUFFER_SIZE, buff_size); // register local buffer
#else
    DLog("[sonuma] kal_reg_lbuff called in VM mode.");
    //tell the KAL how long is the buffer
    ((int *)buff)[0] = num_pages;

    //pin buffer's page frames
    if(ioctl(fd, KAL_PIN_BUFF, buff) == -1)
    {
        return -1;
    }

    ((int *)buff)[0] = 0x0;
#endif /* FLEXUS */

    return 0;
}

int kal_reg_ctx(int fd, uint8_t **ctx_ptr, uint32_t num_pages) {
    //assert(ctx_ptr != NULL);
    uint8_t *ctx = *ctx_ptr;
#ifdef FLEXUS
    int i, retcode, counter;
    int ctx_size = num_pages * PAGE_SIZE;
    // buffers allocation is done by app
    retcode = mlock((void *)ctx, ctx_size*sizeof(uint8_t));
    if (retcode != 0) {
        DLog("[sonuma] Context buffer mlock returned %d", retcode);
    } else {
        DLog("[sonuma] Context buffer was pinned successfully.");
    }

    counter = 0;
    //initialize the context buffer
    ctx[0] = DEFAULT_CTX_VAL;
    call_magic_2_64((uint64_t)ctx, CONTEXTMAP, 0);
    for(i=0; i<ctx_size; i+=PAGE_SIZE) {
        *(ctx + i) = DEFAULT_CTX_VAL;
        counter++;
        call_magic_2_64((uint64_t)&(ctx[i]), CONTEXT, counter);
    }
    DLog("[sonuma] Call Flexus magic call (CONTEXT_SIZE).");
    call_magic_2_64(42, CONTEXT_SIZE, ctx_size); // register ctx
#else
    DLog("[sonuma] kal_reg_ctx called in VM mode.");
    int tmp = ((int *)ctx)[0];

    ((int *)ctx)[0] = num_pages;

    //register context
    if (ioctl(fd, KAL_REG_CTX, ctx) == -1) {
        perror("kal ioctl failed");
        return -1;
    }

    ((int *)ctx)[0] = tmp;
#endif /* FLEXUS */

    return 0;
}

void flexus_signal_all_set() {
#ifdef FLEXUS
    if (is_timing == 0) {
        // global variables for sonuma operation counters
        op_count_issued = 0;
        op_count_completed = 0;
        
        DLog("[sonuma] Call Flexus magic call (ALL_SET).");
        call_magic_2_64(1, ALL_SET, 1);
        is_timing = 1;
    } else {
        DLog("[sonuma] (ALL_SET) magic call won't be called more than once.");
    }
#else
    DLog("[sonuma] flexus_signal_all_set called in VM mode. Do nothing.");
    // otherwise do nothing
#endif /* FLEXUS */
}

int rmc_init(int node_cnt, int this_nid, rmc_wq_t *wq, rmc_cq_t *cq, uint8_t *ctx_mem) {
    qp_info_t * qp_info = (qp_info_t *)malloc(sizeof(qp_info_t));
    int ret;

    qp_info->wq = wq;
    qp_info->cq = cq;
    qp_info->ctx_mem = ctx_mem;
    qp_info->node_cnt = node_cnt;
    qp_info->this_nid = this_nid;

    printf("[sonuma] activating RMC..\n");
    return pthread_create(&rmc_thread, 
			  NULL, 
			  core_rmc_fun, 
			  (void *)qp_info);    
}

void rmc_deinit() {
    deactivate_rmc();
    pthread_join(rmc_thread, NULL);
}

/**
 * VM infra only!!!
 * This func opens connection with kernel driver (KAL).
 */
//int kal_open(char *kal_name);

/**
 * This func registers WQ with KAL or Flexus.
 * Warning: it allocates memory for WQ and pins the memory
 *          to avoid swapping to the disk (pins only for Flexus)
 */
//int kal_reg_wq(int fd, rmc_wq_t **wq_ptr);

/**
 * This func registers CQ with KAL or Flexus.
 * Warning: it allocates memory for WQ and pins the memory
 *          to avoid swapping to the disk (pins only for Flexus)
 */
//int kal_reg_cq(int fd, rmc_cq_t **cq_ptr);

/**
 * This func registers local buffer with KAL or Flexus.
 * Warning: the func pins the memory to avoid swapping to
 *          the disk (only for Flexus); allocation is done within an app
 */
//int kal_reg_lbuff(int fd, uint8_t **buff_ptr, uint32_t num_pages);

/**
 * This func registers context buffer with KAL or Flexus.
 * Warning: the func pins the memory to avoid swapping to
 *          the disk (only for Flexus); allocation is done within an app
 */
//int kal_reg_ctx(int fd, uint8_t **ctx_ptr, uint32_t num_pages);

/**
 * This func signals Flexus to interrupt fast simulation and start clock precise modelling.
 */
//void flexus_signal_all_set();

/**
 * This func checks completed requests in CQ.
 */
//inline void rmc_check_cq(rmc_wq_t *wq, rmc_cq_t *cq, async_handler *handler, void *owner) __attribute__((always_inline));

/**
 * This func polls for a free entry in WQ and, then, adds a Remote Read request to WQ.
 */
//inline void rmc_rread_async(rmc_wq_t *wq, uint64_t lbuff_slot, int snid, uint32_t ctx_id, uint64_t ctx_offset, uint64_t length) __attribute__((always_inline));

/**
 * This func polls for a free entry in WQ and, then, adds a Remote Read request to WQ and waits for its completion.
 */
//inline void rmc_rread_sync(rmc_wq_t *wq, rmc_cq_t *cq, uint64_t lbuff_slot, int snid, uint32_t ctx_id, uint64_t ctx_offset, uint64_t length) __attribute__((always_inline));

/**
 * This func polls for a free entry in WQ and, then, adds a Remote Write request to WQ.
 */
//inline void rmc_rwrite(rmc_wq_t *wq, uint64_t lbuff_slot, int snid, uint32_t ctx_id, uint64_t ctx_offset, uint64_t length) __attribute__((always_inline));


///////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////// Inline methods implementation ///////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

inline void rmc_check_cq(rmc_wq_t *wq, rmc_cq_t *cq, async_handler *handler, void *owner) {
#ifdef FLEXUS
    //DLogPerf("[sonuma] rmc_check_cq called in Flexus mode."); // temporary disabled
#else
    DLogPerf("[sonuma] rmc_check_cq called in VM mode.");
#endif
    uint8_t tid;
    uint8_t wq_head = wq->head;
    uint8_t cq_tail = cq->tail;

    do { // in the upper loop we wait for a free entry in the WQ head
        // in the lower loop we iterate over completed entrie in the CQ
        while (cq->q[cq_tail].SR == cq->SR) {
            tid = cq->q[cq_tail].tid;
            wq->q[tid].valid = 0; // invalidate corresponding entry in WQ
#ifdef DEBUG_FLEXUS_STATS
            op_count_completed++;
#endif

            cq->tail = cq->tail + 1;

            // check if WQ reached its end
            if (cq->tail >= MAX_NUM_WQ) {
                cq->tail = 0;
                cq->SR ^= 1;
            }

#ifdef DEBUG_FLEXUS_STATS
            DLogPerf("[sonuma] Call Flexus magic call (WQENTRYDONE).");
            call_magic_2_64(tid, WQENTRYDONE, op_count_completed);
            DLogPerf("Checking CQ %"PRIu64" time...", op_count_completed);
#endif
            cq_tail = cq->tail;
            handler(tid, wq->q[wq_head], owner);
        }
    } while (wq->q[wq_head].valid);
}

inline void rmc_rread_async(rmc_wq_t *wq, uint64_t lbuff_slot, int snid, uint32_t ctx_id, uint64_t ctx_offset, uint64_t length) {
#ifdef FLEXUS
    DLogPerf("[sonuma] rmc_rread_async called in Flexus mode.");
#else
    DLogPerf("[sonuma] rmc_rread_async called in VM mode.");
#endif
    uint8_t wq_head = wq->head;

#ifdef FLEXUS
    create_wq_entry(RMC_READ, wq->SR, ctx_id, snid, lbuff_slot, ctx_offset, length, (uint64_t)&(wq->q[wq_head]));
#ifdef DEBUG_FLEXUS_STATS
    op_count_issued++;
    DLogPerf("Added an entry to WQ %"PRIu64" time...", op_count_issued);
    DLogPerf("[sonuma] Call Flexus magic call (NEWWQENTRY).");
    call_magic_2_64(wq_head, NEWWQENTRY, op_count_issued);
#endif
#else
    wq->q[wq_head].buf_addr = lbuff_slot;
    wq->q[wq_head].cid = ctx_id;
    wq->q[wq_head].offset = ctx_offset;
    if(length < 64)
	wq->q[wq_head].length = 64; //at least 64B
    else
	wq->q[wq_head].length = length; //specify the length of the transfer
    wq->q[wq_head].op = 'r';
    wq->q[wq_head].nid = snid;
    //soNUMA v2.1
    wq->q[wq_head].valid = 1;
    wq->q[wq_head].SR = wq->SR;
#endif

    wq->head =  wq->head + 1;
    // check if WQ reached its end
    if (wq->head >= MAX_NUM_WQ) {
        wq->head = 0;
        wq->SR ^= 1;
    }
}

inline void rmc_rread_sync(rmc_wq_t *wq, rmc_cq_t *cq, uint64_t lbuff_slot, int snid, uint32_t ctx_id, uint64_t ctx_offset, uint64_t length) {
#ifdef FLEXUS
    DLogPerf("[sonuma] rmc_rread_sync called in Flexus mode.");
#else
    DLogPerf("[sonuma] rmc_rread_sync called in VM mode.");
#endif
    uint8_t wq_head = wq->head;
    uint8_t cq_tail = cq->tail;

#ifdef FLEXUS
#ifdef DEBUG_FLEXUS_STATS
    call_magic_2_64(wq_head, NEWWQENTRY_START, op_count_issued);
    DLogPerf("Added an entry to WQ %"PRIu64" time...", op_count_issued);
    op_count_issued++;
#endif

    create_wq_entry(RMC_READ, wq->SR, ctx_id, snid, lbuff_slot, ctx_offset, length, (uint64_t)&(wq->q[wq_head]));

#ifdef DEBUG_FLEXUS_STATS
    call_magic_2_64(wq_head, NEWWQENTRY, op_count_issued);
#endif
#endif

    wq->head =  wq->head + 1;
    // check if WQ reached its end
    if (wq->head >= MAX_NUM_WQ) {
        wq->head = 0;
        wq->SR ^= 1;
    }

    cq_tail = cq->tail;

    // wait for a completion of the entry
    while(cq->q[cq_tail].SR != cq->SR) {
    }

    // mark the entry as invalid, i.e. completed
    wq->q[cq->q[cq_tail].tid].valid = 0;
#ifdef DEBUG_FLEXUS_STATS
    op_count_completed++;
    call_magic_2_64(cq_tail, WQENTRYDONE, op_count_completed);
#endif
    cq->tail = cq->tail + 1;

    // check if WQ reached its end
    if (cq->tail >= MAX_NUM_WQ) {
        cq->tail = 0;
        cq->SR ^= 1;
    }    

}

inline void rmc_rwrite(rmc_wq_t *wq, uint64_t lbuff_slot, int snid, uint32_t ctx_id, uint64_t ctx_offset, uint64_t length) {
#ifdef FLEXUS
    DLogPerf("[sonuma] rmc_rwrite called in Flexus mode.");
#else
    DLogPerf("[sonuma] rmc_rwrite called in VM mode.");
#endif
    uint8_t wq_head = wq->head;

    while (wq->q[wq_head].valid) {
        // wait for WQ head to be ready
    }

#ifdef FLEXUS
    create_wq_entry(RMC_WRITE, wq->SR, ctx_id, snid, lbuff_slot, ctx_offset, length, (uint64_t)&(wq->q[wq_head]));
#endif

#ifdef DEBUG_FLEXUS_STATS
    op_count_issued++;
    DLogPerf("Added an entry to WQ %"PRIu64" time...", op_count_issued);
    DLogPerf("[sonuma] Call Flexus magic call (NEWWQENTRY).");
    call_magic_2_64(wq_head, NEWWQENTRY, op_count_issued);
#endif

    wq->head =  wq->head + 1;
    // check if WQ reached its end
    if (wq->head >= MAX_NUM_WQ) {
        wq->head = 0;
        wq->SR ^= 1;
    }
}

#endif /* H_SONUMA */
