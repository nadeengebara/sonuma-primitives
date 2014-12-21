#ifndef H_SONUMA
#define H_SONUMA

#include "RMCdefines.h"
#include "magic_iface.h"
#include "son_asm.h"

#define FLEXUS // ustiugov: comment out to get soNUMA for linux

// global variables for sonuma operation counters
uint64_t op_count_issued = 0, op_count_completed = 0;

typedef void (async_handler)(uint8_t tid, wq_entry_t head, void *owner);

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

/**
 * VM infra only!!!
 *
 * This func opens connection with kernel driver (KAL).
 */
inline int kal_open(char *kal_name) {
#ifdef FLEXUS
    return 0; // not used with Flexus
#else
    int fd;

    if ((fd=open(kal_name, O_RDWR|O_SYNC)) < 0) {
        return -1;
    }
    return fd;
#endif
}

/**
 * This func registers WQ with KAL or Flexus.
 * Warning: it allocates memory for WQ and pins the memory
 *          to avoid swapping to the disk (pins only for Flexus)
 */
int kal_reg_wq(int fd, rmc_wq_t **wq_ptr) {
    int i, retcode;
    rmc_wq_t *wq = *wq_ptr;
    wq = (rmc_wq_t *)memalign(PAGE_SIZE, sizeof(rmc_wq_t));
    if (wq == NULL) {
        fprintf(stdout, "Work Queue could not be allocated. Memalign returned %"PRIu64"\n", 0);
        return 1;
    }
    retcode = mlock(*wq, sizeof(rmc_wq_t));
    if (retcode != 0) fprintf(stdout, "WQueue mlock returned %d\n", retcode);

    //setup work queue
    wq->head = 0;
    wq->SR = 1;

    for(i=0; i<MAX_NUM_WQ; i++) {
        wq->q[i].SR = 0;
    }

#ifdef FLEXUS
    call_magic_2_64((uint64_t)wq, WQUEUE, MAX_NUM_WQ);
#else
    posix_memalign((void **)wq, PAGE_SIZE, sizeof(rmc_wq_t));
    if(ioctl(fd, KAL_REG_WQ, (void *)wq) == -1) {
        return -1;
    }
#endif /* FLEXUS */

    return 0;
}

/**
 * This func registers CQ with KAL or Flexus.
 * Warning: it allocates memory for WQ and pins the memory
 *          to avoid swapping to the disk (pins only for Flexus)
 */
int kal_reg_cq(int fd, rmc_cq_t **cq_ptr) {
    int i, retcode;
    rmc_cq_t *cq = *cq_ptr;
    //  *cq = (rmc_cq_t *)memalign(PAGE_SIZE, sizeof(rmc_cq_t));
    cq = (rmc_cq_t *)memalign(PAGE_SIZE, sizeof(rmc_cq_t));
    if (cq == NULL) {
        fprintf(stdout, "Completion Queue could not be allocated. Memalign returned %"PRIu64"\n", 0);
        return 1;
    }
    retcode = mlock(cq, sizeof(rmc_cq_t));
    if (retcode != 0) fprintf(stdout, "CQueue mlock returned %d\n", retcode);

    cq->tail = 0;
    cq->SR = 1;

    for(i=0; i<MAX_NUM_WQ; i++) {
        cq->q[i].SR = 0;
    }
#ifdef FLEXUS
    call_magic_2_64((uint64_t)cq, CQUEUE, MAX_NUM_WQ);
#else
    posix_memalign((void **)cq, PAGE_SIZE, sizeof(rmc_cq_t));
    //register completion queue
    if (ioctl(fd, KAL_REG_CQ, (void *)cq) == -1) {
        return -1;
    }
#endif /* FLEXUS */

    return 0;
}

/**
 * This func registers local buffer with KAL or Flexus.
 * Warning: the func pins the memory to avoid swapping to
 *          the disk (only for Flexus); allocation is done within an app
 */
int kal_reg_lbuff(int fd, char **buff_ptr, uint32_t num_pages) {
    char *buff = *buff_ptr;
#ifdef FLEXUS
    int i, retcode;
    int buff_size = num_pages * PAGE_SIZE;
    // buffers allocation is done by app
    assert(buff != NULL);
    retcode = mlock(buff, buff_size * sizeof(char));
    if (retcode != 0) fprintf(stdout, "Local buffer mlock returned %d (buffer size = %d bytes)\n", retcode, PAGE_SIZE);

    uint32_t counter = 0;
    //initialize the local buffer
    for(i=0; i<(buff_size*sizeof(char)); i++) {
        buff[i] = 0;
        counter = i*sizeof(char)/PAGE_SIZE;
        call_magic_2_64((uint64_t)&(buff[i] ), BUFFER, counter);
    }
    call_magic_2_64(42, BUFFER_SIZE, buff_size); // register local buffer
#else
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

/**
 * This func registers context buffer with KAL or Flexus.
 * Warning: the func pins the memory to avoid swapping to
 *          the disk (only for Flexus); allocation is done within an app
 */
int kal_reg_ctx(int fd, volatile char **ctx_ptr, uint32_t num_pages) {
    char *ctx = *ctx_ptr;
#ifdef FLEXUS
    int i, retcode, counter;
    int ctx_size = num_pages * PAGE_SIZE;
    // buffers allocation is done by app
    assert(ctx != NULL);
    retcode = mlock(ctx, ctx_size*sizeof(char));
    if (retcode != 0) fprintf(stdout, "Context buffer mlock returned %d\n", retcode);

    counter = 0;
    //initialize the context buffer
    ctx[0] = DEFAULT_CTX_VAL;
    call_magic_2_64((uint64_t)ctx, CONTEXTMAP, 0);
    for(i=0; i<ctx_size; i+=PAGE_SIZE) {
        *(ctx + i) = DEFAULT_CTX_VAL;
        counter++;
        call_magic_2_64((uint64_t)&(ctx[i]), CONTEXT, counter);
    }
    call_magic_2_64(42, CONTEXT_SIZE, ctx_size); // register ctx
#else
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

void kal_signal_all_set() {
    call_magic_2_64(1, ALL_SET, 1);
}

/**
 * This func checks completed requests in CQ.
 */
void rmc_check_cq(rmc_wq_t *wq, rmc_cq_t *cq, async_handler *handler, void *owner) {
    uint8_t tid;
    uint8_t wq_head = wq->head;
    uint8_t cq_tail = cq->tail;

    if (cq->q[cq_tail].SR == cq->SR) {
        tid = cq->q[cq_tail].tid;
        wq->q[tid].valid = 0; // invalidate corresponding entry in WQ
        // TODO: counting operations executed properly
        op_count_completed++;

        cq->tail = cq->tail + 1;

        // check if WQ reached its end
        if (cq->tail >= MAX_NUM_WQ) {
            cq->tail = 0;
            cq->SR ^= 1;
        }

        call_magic_2_64(tid, WQENTRYDONE, op_count_completed);
        cq_tail = cq->tail;
        handler(tid, wq->q[wq_head], owner);
    }
}

void rmc_rread_async(rmc_wq_t *wq, char *lbuff_slot, int snid, uint32_t ctx_id, uint64_t ctx_offset, uint64_t length) {
    uint8_t wq_head = wq->head;

    while (wq->q[wq_head].valid) {
        // wait for WQ head to be ready
    }
    create_wq_entry(RMC_READ, wq->SR, ctx_id, snid, (uint64_t)lbuff_slot, ctx_offset, length, (uint64_t)&(wq->q[wq_head]));
    call_magic_2_64(wq_head, NEWWQENTRY, op_count_issued);

    wq->head =  wq->head + 1;
    // check if WQ reached its end
    if (wq->head >= MAX_NUM_WQ) {
        wq->head = 0;
        wq->SR ^= 1;
    }
}

#endif /* H_SONUMA */
