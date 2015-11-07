/**
 * soNUMA defines and structures.
 *
 * Copyright (C) EPFL. All rights reserved.
 * @authors daglis, novakovic, ustiugov
 */

#ifndef H_RMC_DEFINES
#define H_RMC_DEFINES

//#define FLEXI_MODE  // do not use flexi mode unless for flexus ubenches
#define FLEXUS
#define PROTOCOL_v2_2

#ifdef FLEXUS
//#ifdef PROTOCOL_v2_2
//#define MAX_NUM_WQ 256
//#else
#define MAX_NUM_WQ 128
//#endif
#else
#define MAX_NUM_WQ 100
#endif

#define DEFAULT_CTX_VAL 123

//breakpoint IDs
#define WQUEUE              1
#define CQUEUE              2
#define BUFFER              3
#define PTENTRY             4
#define NEWWQENTRY          5
#define WQENTRYDONE         6
#define ALL_SET             7
#define TID_COUNTERS        8
#define CONTEXT             9
#define CONTEXTMAP          10
#define WQHEAD              11
#define WQTAIL              12
#define CQHEAD              13
#define CQTAIL              14
#define SIM_BREAK           15
#define NETPIPE_START       16
#define NETPIPE_END         17
#define RMC_DEBUG_BP        18
#define BENCHMARK_END       19
#define BUFFER_SIZE         20
#define CONTEXT_SIZE        21
#define NEWWQENTRY_START    22
#define NEW_SABRE           23
#define SABRE_SUCCESS       24
#define SABRE_ABORT         25
#define OBJECT_WRITE        26	//used by writers in SABRe experiments, to count number of object writes
#define LOCK_SPINNING	    27
#define CS_START            28
#define MEASUREMENT	    99

//PT parameters for Page Walks
#define PT_I 3
#define PT_J 10
#define PT_K 4

#ifdef FLEXUS
#define PAGE_SIZE 8192
#define PAGE_BITS 0xffffffffffffe000
#else
#define PAGE_SIZE 4096
#define PAGE_BITS 0xfffffffffffff000
#endif

//WQ entry field offsets - for non-compacted version
#define WQ_TYPE_OFFSET          0
#define WQ_NID_OFFSET           8
#define WQ_TID_OFFSET           16
#define WQ_CID_OFFSET           24
#define WQ_OFFSET_OFFSET        32
#define WQ_BUF_LENGTH_OFFSET    48
#define WQ_BUF_ADDRESS_OFFSET   64

//op types
#define RMC_READ        1
#define RMC_WRITE       2
#define RMC_RMW         3
#define RMC_SABRE	4
#define RMC_INVAL       42
#define PADBYTES        60

////////////////////////// KAL DEFINES/////////////////////////////////
#define KAL_REG_WQ      1
#define KAL_UNREG_WQ    6
#define KAL_REG_CQ      5
#define KAL_REG_CTX     3
#define KAL_PIN_BUFF    4
#define KAL_PIN         14

#define RMC_KILL        10

#define BLOCK_SIZE      64
#define BBUFF_SIZE      16
#define PL_SIZE         60 //payload size
/* [ustiugov] Not needed now
#define RW_THR      16//16384//4096//16 //16384//256 //8192 //256    //daglis TODO: Why is this only 16? (was 1K in the paper)
#define RREAD       1
#define RWRITE      0
*/
///////////////////////////////////////////////////////////////////////

#ifdef FLEXUS
typedef struct wq_entry{
    //first double-word (8 bytes)
    uint8_t op : 6;        //up to 64 soNUMA ops
    uint8_t SR : 1;        //sense reverse bit
    uint8_t valid : 1;    //set with a new WQ entry, unset when entry completed. Required for pipelining async ops
    uint64_t buf_addr : 42;
    uint8_t cid : 4;
    uint16_t nid : 10;
    //second double-word (8 bytes)
    uint64_t offset : 40;
    uint64_t length : 24;
} wq_entry_t;
#else
typedef struct wq_entry{
    //first double-word (8 bytes)
    uint8_t op;        //up to 64 soNUMA ops
    volatile uint8_t SR;        //sense reverse bit
    volatile uint8_t valid;    //set with a new WQ entry, unset when entry completed. Required for pipelining async ops
    uint64_t buf_addr;
    uint8_t cid;
    uint16_t nid;
    //second double-word (8 bytes)
    uint64_t offset;
    uint64_t length;
} wq_entry_t;
#endif /* FLEXUS */


#ifdef FLEXUS
#ifdef PROTOCOL_v2_2
typedef struct cq_entry{
    volatile uint8_t SR : 1;     //sense reverse bit
    volatile uint8_t success : 7; //only one bit actually used. 0 means failure
    volatile uint8_t tid;
} cq_entry_t;
#else
typedef struct cq_entry{
    volatile uint8_t SR : 1;     //sense reverse bit
    volatile uint8_t tid : 7;
} cq_entry_t;
#endif
#else
typedef struct cq_entry{
    volatile uint8_t SR;     //sense reverse bit
    volatile uint8_t tid;
} cq_entry_t;
#endif

#ifdef FLEXUS
typedef struct rmc_wq {
    wq_entry_t q[MAX_NUM_WQ];
    uint8_t head;
    uint8_t SR : 1;    //sense reverse bit
} rmc_wq_t;
#else
typedef struct rmc_wq {
    wq_entry_t q[MAX_NUM_WQ];
    uint8_t head;
    volatile uint8_t SR;    //sense reverse bit
} rmc_wq_t;
#endif

#ifdef FLEXUS
typedef struct rmc_cq {
    cq_entry_t q[MAX_NUM_WQ];
    uint8_t tail;
    uint8_t SR : 1;    //sense reverse bit
} rmc_cq_t;
#else
typedef struct rmc_cq {
    cq_entry_t q[MAX_NUM_WQ];
    uint8_t tail;
    volatile uint8_t SR;    //sense reverse bit
} rmc_cq_t;
#endif

typedef struct qp_info {
    int node_cnt;
    int this_nid;
} qp_info_t;

#endif /* H_RMC_DEFINES */
