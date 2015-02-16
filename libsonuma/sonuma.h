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

#include "RMCdefines.h"
#include "magic_iface.h"
#include "son_asm.h"

#define FLEXUS // ustiugov: comment out to get soNUMA for linux

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

typedef void (async_handler)(uint8_t tid, wq_entry_t head, void *owner);

/**
 * VM infra only!!!
 * This func opens connection with kernel driver (KAL).
 */
int kal_open(char *kal_name);

/**
 * This func registers WQ with KAL or Flexus.
 * Warning: it allocates memory for WQ and pins the memory
 *          to avoid swapping to the disk (pins only for Flexus)
 */
int kal_reg_wq(int fd, rmc_wq_t **wq_ptr);

/**
 * This func registers CQ with KAL or Flexus.
 * Warning: it allocates memory for WQ and pins the memory
 *          to avoid swapping to the disk (pins only for Flexus)
 */
int kal_reg_cq(int fd, rmc_cq_t **cq_ptr);

/**
 * This func registers local buffer with KAL or Flexus.
 * Warning: the func pins the memory to avoid swapping to
 *          the disk (only for Flexus); allocation is done within an app
 */
int kal_reg_lbuff(int fd, uint8_t **buff_ptr, uint32_t num_pages);

/**
 * This func registers context buffer with KAL or Flexus.
 * Warning: the func pins the memory to avoid swapping to
 *          the disk (only for Flexus); allocation is done within an app
 */
int kal_reg_ctx(int fd, uint8_t **ctx_ptr, uint32_t num_pages);

/**
 * This func signals Flexus to interrupt fast simulation and start clock precise modelling.
 */
void flexus_signal_all_set();

/**
 * This func checks completed requests in CQ.
 */
inline void rmc_check_cq(rmc_wq_t *wq, rmc_cq_t *cq, async_handler *handler, void *owner) __attribute__((always_inline));

/**
 * This func polls for a free entry in WQ and, then, adds a Remote Read request to WQ.
 */
inline void rmc_rread_async(rmc_wq_t *wq, uint64_t lbuff_slot, int snid, uint32_t ctx_id, uint64_t ctx_offset, uint64_t length) __attribute__((always_inline));

/**
 * This func polls for a free entry in WQ and, then, adds a Remote Read request to WQ and waits for its completion.
 */
inline void rmc_rread_sync(rmc_wq_t *wq, rmc_cq_t *cq, uint64_t lbuff_slot, int snid, uint32_t ctx_id, uint64_t ctx_offset, uint64_t length) __attribute__((always_inline));

/**
 * This func polls for a free entry in WQ and, then, adds a Remote Write request to WQ.
 */
inline void rmc_rwrite(rmc_wq_t *wq, uint64_t lbuff_slot, int snid, uint32_t ctx_id, uint64_t ctx_offset, uint64_t length) __attribute__((always_inline));


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

inline void rmc_rread_sync(rmc_wq_t *wq, rmc_cq_t *cq, uint64_t lbuff_slot, int snid, uint32_t ctx_id, uint64_t ctx_offset, uint64_t length) {
#ifdef FLEXUS
    DLogPerf("[sonuma] rmc_rread_sync called in Flexus mode.");
#else
    DLogPerf("[sonuma] rmc_rread_sync called in VM mode.");
#endif
    uint8_t wq_head = wq->head;
    uint8_t cq_tail = cq->tail;

#ifdef DEBUG_FLEXUS_STATS
    call_magic_2_64(wq_head, NEWWQENTRY_START, op_count_issued);
    DLogPerf("Added an entry to WQ %"PRIu64" time...", op_count_issued);
    op_count_issued++;
#endif

#ifdef FLEXUS
    create_wq_entry(RMC_READ, wq->SR, ctx_id, snid, lbuff_slot, ctx_offset, length, (uint64_t)&(wq->q[wq_head]));
#endif

#ifdef DEBUG_FLEXUS_STATS
    call_magic_2_64(wq_head, NEWWQENTRY, op_count_issued);
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
