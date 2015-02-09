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

#define DEBUG
// ustiugov: WARNING!!! DEBUG_PERF enables I/O in performance regions (it uses DLogPerf)! Do not enable during experiments!
//#define DEBUG_PERF

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

// global variables for sonuma operation counters
extern uint64_t op_count_issued;
extern uint64_t op_count_completed;

#define FLEXUS // ustiugov: comment out to get soNUMA for linux

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
void rmc_check_cq(rmc_wq_t *wq, rmc_cq_t *cq, async_handler *handler, void *owner);

/**
 * This func polls for a free entry in WQ and, then, adds a Remote Read request to WQ.
 */
void rmc_rread_async(rmc_wq_t *wq, uint64_t lbuff_slot, int snid, uint32_t ctx_id, uint64_t ctx_offset, uint64_t length);

/**
 * This func polls for a free entry in WQ and, then, adds a Remote Read request to WQ and waits for its completion.
 */
void rmc_rread_sync(rmc_wq_t *wq, uint64_t lbuff_slot, int snid, uint32_t ctx_id, uint64_t ctx_offset, uint64_t length);

/**
 * This func polls for a free entry in WQ and, then, adds a Remote Write request to WQ.
 */
void rmc_rwrite(rmc_wq_t *wq, uint64_t lbuff_slot, int snid, uint32_t ctx_id, uint64_t ctx_offset, uint64_t length);

#endif /* H_SONUMA */
