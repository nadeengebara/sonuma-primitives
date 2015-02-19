/**
 * soNUMA library functions implementation.
 *
 * ustiugov: tested with flexus (solaris) but still incompatible with Linux
 *
 * Copyright (C) EPFL. All rights reserved.
 * @authors daglis, novakovic, ustiugov
 */

#include <malloc.h>
#include <assert.h>
#include <sys/mman.h>

#include "sonuma.h"

//////////////////////// LEGACY FROM FARM ///////////////////////////
/*
//the assumption is that we have just one peer
volatile uint64_t rsq_head = 0; //don't worry about this
uint64_t rsq_head_prev = 0;
volatile uint64_t local_cred_cnt = 0; //this is the latest snapshot that we compare to the current state
volatile uint8_t remote_cred_cnt = BBUFF_SIZE;
volatile uint64_t lsq_tail = 0;

uint64_t bubbles;
uint64_t non_bubbles;
uint64_t rdom_bubbles[16];
//uint64_t spin_cycles;

uint64_t sent_b = 0;

uint8_t old_len = 0;

uint32_t pkt_rcvd = 0;
uint32_t pkt_snt = 0;

uint32_t opc = 0;

inline uint64_t return_bubbles() {
return bubbles;
}

inline uint64_t return_non_bubbles() {
return non_bubbles;
}

inline void print_rdom_bubbles() {
int i;
for(i = 0; i<16; i++)
printf("[rmc runtime] rdom_bubbles[%d] = %lu\n", i, rdom_bubbles[i]);
}

inline uint64_t return_spin_cycles() {
return spin_cycles;
}
*/
/////////////////////// END OF LEGACY ///////////////////////////////


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
    assert(buff_ptr != NULL);
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
    assert(ctx_ptr != NULL);
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

int rmc_init(rmc_wq_t *wq, rmc_cq_t *cq, uint8_t *ctx_mem) {
    qp_info_t qp_info;
    pthread_t rmc_thread;
    int ret;

    qp_info.wq = wq;
    qp_info.cq = cq;
    qp_info.ctx_mem = ctx_mem;

    return pthread_create(&rmc_thread, 
			  NULL, 
			  core_rmc_fun, 
			  (void *)&qp_info);    
}
