//#ifndef RMC_DEFS
//#define RMC_DEFS

#define version4_1	//soNUMA protocol version that is being used

#define MAX_NUM_WQ	128

//breakpoint IDs
#define WQUEUE  		1
#define CQUEUE  		2
#define BUFFER  		3
#define PTENTRY 		4
#define NEWWQENTRY		5
#define WQENTRYDONE 		6
#define ALL_SET			7
#define TID_COUNTERS		8
#define CONTEXT			9
#define CONTEXTMAP		10
#define WQHEAD			11
#define WQTAIL			12
#define CQHEAD			13
#define CQTAIL			14
#define SIM_BREAK		15
#define NETPIPE_START		16
#define NETPIPE_END		17
#define RMC_DEBUG_BP		18
#define PAGERANK_END		19
#define BUFFER_SIZE		20
#define CONTEXT_SIZE		21
#define NEWWQENTRY_START	22

	
//stuff for Page Walks
#define PT_I 3
#define PT_J 10
#define PT_K 4

#define PAGESIZE 8192
#define PAGE_SIZE 8192
#define PAGE_BITS 0xffffffffffffe000

//WQ entry field offsets - for non-compacted version
#define WQ_TYPE_OFFSET 		0
#define WQ_NID_OFFSET 		8
#define WQ_TID_OFFSET		16
#define WQ_CID_OFFSET		24
#define WQ_OFFSET_OFFSET	32
#define WQ_BUF_LENGTH_OFFSET	48
#define WQ_BUF_ADDRESS_OFFSET	64

//op types
#define RMC_READ  	1
#define RMC_WRITE 	2
#define RMC_RMW 	3
#define RMC_INVAL 	42

#define PADBYTES 60

#define WQ_COMPACTION

#ifndef WQ_COMPACTION
typedef struct wq_entry{
        volatile uint8_t op;
        uint8_t nid;
        uint8_t cid;
        uint64_t offset;
        uint64_t length;
        void* buff;
} wq_entry_t;

#else 

#ifdef version4_1
typedef struct wq_entry{
  //first double-word (8 bytes)
        uint8_t op : 6;		//up to 64 soNUMA ops
        uint8_t SR : 1;		//sense reverse bit
        uint8_t valid : 1;	//set with a new WQ entry, unset when entry completed. Required for pipelining async ops
        uint64_t buf_addr : 42;		
	uint8_t cid : 4;
        uint16_t nid : 10;
  //second double-word (8 bytes)
	uint64_t offset : 40;		
        uint64_t length : 24;	
} wq_entry_t;

#else
//THIS IS NOT PORTABLE - USE OF EXPLICIT BITMASKS IS PROBABLY A BETTER IDEA
typedef struct wq_entry{
        uint8_t op : 2;
        uint8_t SR : 1;		//sense reverse bit
        uint8_t cid : 4;
        uint16_t nid : 9;
        uint64_t buf_addr : 48;		//42 + 6 bits for alignment
	uint64_t offset : 40;		//37 + 3 bits for alignment
        uint64_t length : 24;		//19 + 5 bits for alignment - length in terms of blocks
} wq_entry_t;
#endif	//ifdef version4_1
#endif
typedef struct cq_entry{
	volatile uint8_t SR : 1; 	//sense reverse bit
    	volatile uint8_t tid : 7;	
} cq_entry_t;

typedef struct rmc_wq {
  wq_entry_t q[MAX_NUM_WQ];
  uint8_t head;	
  uint8_t SR : 1;	//sense reverse bit
} rmc_wq_t;

typedef struct rmc_cq {
  cq_entry_t q[MAX_NUM_WQ];
  uint8_t tail;		
  uint8_t SR : 1;	//sense reverse bit
} rmc_cq_t;

//#endif
