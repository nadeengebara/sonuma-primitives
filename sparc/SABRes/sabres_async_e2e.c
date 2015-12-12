//Multithreaded SABRe microbenchmark. Allocation of single context (containing the dataset) and a single local buffer
//(to be partitioned and shared by the threads). Each thread has its own QP.
//There are reader and writer threads (number of each is an input parameter).
//Writers only issue random writes to objects, readers issue SYNCHRONOUS reads to remote memory.
//Threads are pinned to cores.

#define __STDC_FORMAT_MACROS
#define _GNU_SOURCE
#include <pthread.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/pset.h>

#ifdef OLD_DEF
#include </net2/daglis/soNUMA/magic_iface.h>
#include </net2/daglis/soNUMA/RMCdefines.h>
#include "son_asm.h"
#else
    #include "../../libsonuma/sonuma.h"
    #include "../../libsonuma/magic_iface.h"
#endif /* OLD_DEF */

#define PASS2FLEXUS_MEASURE(...)    call_magic_2_64(__VA_ARGS__)
#define DEBUG_ASSERT(...)
#define MIN(a,b) (((a)<(b))?(a):(b))
#define APP_BUFF_SIZE (PAGE_SIZE*2)

#define LLC_SIZE (4*1024*1024)

#define ITERS 100000000
#define OBJECT_BYTE_STRIDE 128  //how much of the object will be touched by readers/writers (128 -> 50% (half of the blocks))
#define CTX_ID 0
#define DST_NID 1

#define CONC_ASYNC_OPS 10  //number of concurrent reads in flight (per reader)

#ifdef OLD_DEF // old lib is used
    #define CREATE_WQ_ENTRY(...)    create_wq_entry2(__VA_ARGS__)
    #define PASS2FLEXUS_MEASURE(...)
    #define PASS2FLEXUS_DBG(...)
    #define PASS2FLEXUS_DBG_OLD(...)    call_magic_2_64(__VA_ARGS__)
    #define PASS2FLEXUS_MEASURE_OLD(...)    call_magic_2_64(__VA_ARGS__)

    #define RMC_SABRE RMC_READ
#else // new lib is used
    #define CREATE_WQ_ENTRY(...)    create_wq_entry(__VA_ARGS__)
    #define PASS2FLEXUS_MEASURE(...)    call_magic_2_64(__VA_ARGS__)
    #define PASS2FLEXUS_DBG(...)    call_magic_2_64(__VA_ARGS__)
    #define PASS2FLEXUS_MEASURE_OLD(...)
#endif

/////////////////////////////////// FARM's good stuff /////////////////////////////////////
#define CACHE_LINE_SIZE 64
typedef uintptr_t nam_version_t;
typedef uint16_t nam_cl_version_t;

const nam_version_t UPDATING_MASK = 2;
const nam_version_t RESERVED_BITS = 2;

const size_t DATA_BYTES_PER_CACHE_LINE = CACHE_LINE_SIZE - sizeof(nam_cl_version_t);

//FARM header of an object
typedef struct Header {
  nam_version_t version;
  uint64_t incarnation;
} header_t;

int version_is_updating(nam_version_t version) {
  return (version & UPDATING_MASK) != 0;
}

inline nam_cl_version_t version_get_cl_version_bits(nam_version_t version) {
  return (nam_cl_version_t)(version >> RESERVED_BITS);
}
////////////////////////////////////////////////////////////////////////////////////////////
//#define MEASURE_TS //to measure the latency of the writers' critical section
//#define MY_DEBUG


typedef uint64_t version_t;
typedef volatile uint8_t my_lock_t;
typedef uint32_t my_key_t;
typedef uint64_t cl_version_t;
const int hdr_size = sizeof(version_t) + 4 + sizeof(my_key_t);

//RMW for atomic lock acquirement
static inline uint8_t acquire_lock(my_lock_t *ptr) {
        uint8_t ret_value;
        asm volatile(
                "ldstub [%1], %0\n\t"
                : "=r"(ret_value)    //output
                : "r"(ptr)    //input registers
            	: "memory" //clobbered registers
                );
         return ret_value;
    }

//data object's struct
typedef struct data_object {
  version_t version;    // 8 bytes
  my_lock_t lock;          // 4 bytes
  my_key_t key;            // 4 bytes
/*
  uint8_t value[DATA_SIZE];
  uint8_t padding[64-(DATA_SIZE+16)%64]; //All objects need to be cache-line-aligned
*/
} data_object_t;

typedef data_object_t obj_hdr_t;

/*
typedef struct app_object {
  my_key_t key;
  uint8_t value[DATA_SIZE];
  uint8_t padding[64-(DATA_SIZE+4)%64]; //All objects need to be cache-line-aligned
} app_object_t;
*/

typedef struct {
	int id;
	int is_reader;
} parm;

uint8_t *lbuff;
uintptr_t ctxbuff;  
int iters, num_objects;
uint64_t thread_buf_size;
uint16_t readers, writers;
unsigned data_obj_size;
int memcpyID = 16;

pthread_barrier_t   barrier; 

void * par_phase_write(void *arg) {
    parm *p=(parm *)arg;
     
    int i,j,luckyObj;
    srand(p->id);		//remove this for lots of conflicts :-)
    pthread_barrier_wait (&barrier);
    if (!p->id) call_magic_2_64(1, ALL_SET, 1); //INIT DONE
     
    uint8_t prevLockVal;
    for (i = 0; i<iters; i++) {
	luckyObj = rand() % num_objects;
        uintptr_t obj_ptr = ctxbuff + luckyObj*data_obj_size;
        data_object_t *my_obj = (data_object_t *)obj_ptr;
	while (my_obj->lock);	//TTS
	do {
		prevLockVal = acquire_lock(&(my_obj->lock));	//Test-and-set
		if (prevLockVal) {
			PASS2FLEXUS_DBG(luckyObj, LOCK_SPINNING, prevLockVal);	//signal the completion of a write
			#ifdef MY_DEBUG
			printf("thread %d failed to grab lock of item %d! (lock value = %"PRIu8")\n", p->id, luckyObj, prevLockVal);
			usleep(10);
			#endif
		}
	} while (prevLockVal);
        #ifdef MEASURE_TS
	PASS2FLEXUS_DBG(luckyObj, CS_START, i);	//signal the beginning of the CS
        #endif
        uintptr_t data_ptr = obj_ptr + sizeof(obj_hdr_t);
	for (j=0; j<data_obj_size; j++) {
		*(uint8_t *)(data_ptr+j) ^= 1;
	}
	#ifdef MY_DEBUG
  	printf("version = %" PRIu64 ",\t\
	  lock = %" PRIu8 ",\t\
	  key = %" PRIu32 "\n", my_obj->version, my_obj->lock, my_obj->key);
	#endif
        // FIXME
	my_obj->key ^= 7;  //random operation on object
	for (j=0; j<data_obj_size; j+=OBJECT_BYTE_STRIDE) 
		*(uint8_t *)(data_ptr+j) = (uint8_t)i;
	my_obj->version++;
	my_obj->lock = 0;	//unlock
	PASS2FLEXUS_DBG(luckyObj, OBJECT_WRITE, i);	//signal the completion of a write
	#ifdef MY_DEBUG
  	printf("Thread %d: \tprevious lock value = %" PRIu8 ",\t\
	  	new lock value = %" PRIu8 ",\t\
		new version = %" PRIu64 "\n", p->id, prevLockVal, my_obj->lock, my_obj->version);
	#endif
    }
    PASS2FLEXUS_DBG(0, BENCHMARK_END, 0);	//this threads completed its work and it's exiting

    return NULL;
}

void * par_phase_read(void *arg) {
    parm *p=(parm *)arg;
    
    rmc_wq_t *wq;
    rmc_cq_t *cq;;

    uint8_t *app_buff;
    app_buff = memalign(PAGE_SIZE, APP_BUFF_SIZE);
    unsigned payload_cache_blocks = data_obj_size>>6;

    if (app_buff == NULL) {
        fprintf(stdout, "App buffer could not be allocated. Malloc returned %"PRIu64"\n", 0);
        exit(1);
    } 

    //init stuff - allocate queues, one QP per thread
    //wq = memalign(PAGE_SIZE, sizeof(rmc_wq_t));
    wq = memalign(PAGE_SIZE, PAGE_SIZE);//Should allocate full page
    if (wq == NULL) {
        fprintf(stdout, "Work Queue could not be allocated. Memalign returned %"PRIu64"\n", 0);
        exit(1);
    } 
    //retcode = mlock(wq, sizeof(rmc_wq_t));
    int retcode = mlock(wq, PAGE_SIZE);
    if (retcode != 0)  fprintf(stdout, "WQueue mlock returned %d\n", retcode);

    //cq = memalign(PAGE_SIZE, sizeof(rmc_cq_t));//Should allocate full page
    cq = memalign(PAGE_SIZE, PAGE_SIZE);
    if (cq == NULL) {
        fprintf(stdout, "Completion Queue could not be allocated. Memalign returned %"PRIu64"\n", 0);
        exit(1);
    } 
    //retcode = mlock(cq, sizeof(rmc_cq_t));
    retcode = mlock(cq, PAGE_SIZE);
    if (retcode != 0) fprintf(stdout, "CQueue mlock returned %d\n", retcode);

    uint8_t operation = (uint8_t)RMC_INVAL;
    //setup work queue
    wq->head = 0;
    wq->SR = 1;
    
    int i;
    for(i=0; i<MAX_NUM_WQ; i++) {
        wq->q[i].SR = 0;
    }
    call_magic_2_64((uint64_t)wq, WQUEUE, MAX_NUM_WQ);

    //setup completion queue
    cq->tail = 0;
    cq->SR = 1;

    for(i=0; i<MAX_NUM_WQ; i++) {
        cq->q[i].SR = 0;
    }
    call_magic_2_64((uint64_t)cq, CQUEUE, MAX_NUM_WQ);

    //////////////////////////////
    uint64_t *bogus_buff;
    uint64_t temp;
    bogus_buff = memalign(PAGE_SIZE, LLC_SIZE);
    for(i=0; i<LLC_SIZE; i+=64) {
        temp += ( (uint8_t *)(bogus_buff) )[i];
    }
/*
    int obj_buf_size_ = num_objects*sizeof(data_object_t);
    for(i=0; i<obj_buf_size_; i+=64) {
        temp += ( (uint8_t *)(ctxbuff) )[i];
    }
*/
    for(i=0; i<APP_BUFF_SIZE; i+=64) {
        temp += ( (uint8_t *)(app_buff) )[i];
    }
    printf("%x", temp);
    //////////////////////////////

    int j,luckyObj;
    srand(p->id);		//remove this for lots of conflicts :-)
    pthread_barrier_wait (&barrier);
    if (!p->id) call_magic_2_64(1, ALL_SET, 1); //INIT DONE
 
    uint8_t *lbuff_slot, *thread_buf_base;
    uint64_t ctx_offset, op_count_issued = 0, op_count_completed = 0;
    uint32_t wq_head, cq_tail;
    thread_buf_base = lbuff + thread_buf_size * p->id; //this is the local buffer's base address for this thread

//reader kernel    
    uint8_t success = 0, tid; 
    while (op_count_completed<iters) {

        //first, check for complete entries
        wq_head = wq->head;
        cq_tail = cq->tail;

#ifndef OLD_DEF
        do {
            while (cq->q[cq_tail].SR == cq->SR) {
                tid = cq->q[cq_tail].tid;
                wq->q[tid].valid = 0;
                //do whatever is needed with tid here
	        success = cq->q[cq_tail].success;
 		//process read data AND schedule another request
                if (success) {	//No atomicity violation!
                    PASS2FLEXUS_DBG(cq_tail, SABRE_SUCCESS, op_count_completed);
	        } else {	//Atomicity violation detected
                    PASS2FLEXUS_DBG(cq_tail, SABRE_ABORT, op_count_completed);
                    //need to reschedule same op
	        }              
		 op_count_completed++;

                cq->tail = cq->tail + 1;
                if (cq->tail >= MAX_NUM_WQ) {
                    cq->tail = 0;
                    cq->SR ^= 1;
                }
                cq_tail = cq->tail;
            }
        } while (wq->q[wq_head].valid);
                
            luckyObj = rand() % num_objects;
	    lbuff_slot = (uint8_t *)(thread_buf_base + ((op_count_issued * data_obj_size) % thread_buf_size));
       	    ctx_offset = luckyObj * data_obj_size;

            wq_head = wq->head;
            CREATE_WQ_ENTRY(RMC_SABRE, wq->SR, CTX_ID, DST_NID, (uint64_t)lbuff_slot, ctx_offset, data_obj_size>>6, (uint64_t)&(wq->q[wq_head]));
            call_magic_2_64(wq_head, NEWWQENTRY, op_count_issued);
            op_count_issued++;
	    wq->head =  wq->head + 1;
	    if (wq->head >= MAX_NUM_WQ) {
 	    	wq->head = 0;
      		wq->SR ^= 1;
    	    }
#else
	do {
	  if (wq->q[wq_head].SR != (uint8_t)wq->SR) {
		 while (cq->q[cq_tail].SR == cq->SR) {
		  tid = cq->q[cq_tail].tid;
		  //handler(tid);
		  op_count_completed++;
		  	
		  cq->tail = cq->tail + 1;
	  	  if (cq->tail >= MAX_NUM_WQ) {
			cq->tail = 0;
			cq->SR ^= 1;
		  }    
		  
		  call_magic_2_64(tid, WQENTRYDONE, op_count_completed);
		  cq_tail = cq->tail;
		}
	   } 
	} while (wq->SR != cq->SR);     
      
            luckyObj = rand() % num_objects;
	    lbuff_slot = (uint8_t *)(thread_buf_base + ((op_count_issued * data_obj_size) % thread_buf_size));
       	    ctx_offset = luckyObj * data_obj_size;

            wq_head = wq->head;
            CREATE_WQ_ENTRY(RMC_SABRE, wq->SR, CTX_ID, DST_NID, (uint64_t)lbuff_slot, ctx_offset, data_obj_size>>6, (uint64_t)&(wq->q[wq_head]));
            call_magic_2_64(wq_head, NEWWQENTRY, op_count_issued);
            op_count_issued++;
	
	  wq->head =  wq->head + 1;
      	  if (wq->head >= MAX_NUM_WQ) {
	  	wq->head = 0;
		wq->SR ^= 1;
	  }  
#endif
    }
/*
        do {
            while (cq->q[cq_tail].SR == cq->SR) {
                tid = cq->q[cq_tail].tid;
                wq->q[tid].valid = 0;
                //do whatever is needed with tid here
	        success = cq->q[cq_tail].success;
                op_count_completed++;

                cq->tail = cq->tail + 1;
                if (cq->tail >= MAX_NUM_WQ) {
                    cq->tail = 0;
                    cq->SR ^= 1;
                }
                cq_tail = cq->tail;
                
                //process read data AND schedule another request
                if (success) {	//No atomicity violation!
                    call_magic_2_64(cq_tail, SABRE_SUCCESS, op_count_completed);
	            //touch the data
                    /
	            uint64_t accum;
		    for (j=0; j<DATA_SIZE; j+=OBJECT_BYTE_STRIDE) 
			accum += (uint64_t)*(lbuff_slot + j);
	            *(lbuff_slot) = accum;
		    //prepare new req
                    /
                    luckyObj = rand() % num_objects;
                    ctx_offset = luckyObj * data_obj_size;
	        } else {	//Atomicity violation detected
                    call_magic_2_64(cq_tail, SABRE_ABORT, op_count_completed);
                    //need to reschedule same op
                    ctx_offset = wq->q[tid].offset;
	        }
	        lbuff_slot = (uint8_t *)(thread_buf_base + ((op_count_issued * data_obj_size) % thread_buf_size));
	        wq_head = wq->head;
	        create_wq_entry(RMC_SABRE, wq->SR, CTX_ID, DST_NID, (uint64_t)lbuff_slot, ctx_offset, data_obj_size>>6, (uint64_t)&(wq->q[wq_head]));
	        call_magic_2_64(wq_head, NEWWQENTRY, op_count_issued);
		op_count_issued++;
		wq->head =  wq->head + 1;
		if (wq->head >= MAX_NUM_WQ) {
 		    	wq->head = 0;
      			wq->SR ^= 1;
    		}
            }
        } while (wq->q[wq_head].valid);

	//issue a bunch or requests. Control number of outstanding requests
        while (op_count_issued - op_count_completed < CONC_ASYNC_OPS) { 
            luckyObj = rand() % num_objects;
	    lbuff_slot = (uint8_t *)(thread_buf_base + ((op_count_issued * data_obj_size) % thread_buf_size));
       	    ctx_offset = luckyObj * data_obj_size;

            wq_head = wq->head;
            create_wq_entry(RMC_SABRE, wq->SR, CTX_ID, DST_NID, (uint64_t)lbuff_slot, ctx_offset, data_obj_size>>6, (uint64_t)&(wq->q[wq_head]));
            call_magic_2_64(wq_head, NEWWQENTRY, op_count_issued);
            op_count_issued++;
	    wq->head =  wq->head + 1;
	    if (wq->head >= MAX_NUM_WQ) {
 	    	wq->head = 0;
      		wq->SR ^= 1;
    	    }
        }
    }
*/
    PASS2FLEXUS_DBG(0, BENCHMARK_END, 0);	//this thread completed its work and it's exiting
    free(wq);
    free(cq);

    return NULL;
}

int main(int argc, char **argv)
{
    int i, retcode, num_threads; 
    iters = ITERS;

    if (!(argc == 5 || argc == 6)) {
        fprintf(stdout,"Usage: %s <ctx_buff_size (in KB)> <num_readers> <num_writers> <obj_size> [<memcpyID=16(default), 8, 4, 2>]\n", argv[0]);
        return 1;  
    }
#if defined(NO_SW_VERSION_CONTROL) && defined(ZERO_COPY)
    printf("Running sabres E2E.\n");
#elif !defined(NO_SW_VERSION_CONTROL) && !defined(ZERO_COPY)
    printf("Running farm-like E2E.\n");
#else
#error "Both NO_SW_VERSION_CONTROL and ZERO_COPY must be defined"
#endif
    fprintf(stdout,"Application buffer size is %d bytes\n", APP_BUFF_SIZE);
    uint64_t ctx_size = atoi(argv[1])*1024;
    data_obj_size = atoi(argv[4]);
    fprintf(stdout,"Data object size is %d bytes, ctx size is %d bytes\n", data_obj_size, ctx_size);
    num_objects = ctx_size/data_obj_size;
    assert( (data_obj_size % 1024) == 0 );
    readers = atoi(argv[2]) ;
    writers = atoi(argv[3]) ;
    if (argc == 6)
        memcpyID = atoi(argv[5]);
    num_threads = readers+writers;
    assert(num_threads <= 16);
    assert(sizeof(obj_hdr_t) == 16);
    uint64_t buf_size = data_obj_size * readers * MAX_NUM_WQ; 
    if (readers) thread_buf_size = buf_size / readers;
    else thread_buf_size = 0;

    //////////////////////////////////////////
/*
    unsigned *dst = memalign(PAGE_SIZE, PAGE_SIZE);
    unsigned *src = memalign(PAGE_SIZE, PAGE_SIZE);
    memset(src, 0xab, PAGE_SIZE);
    //my_memcopy(dst, src, 8192, 0);
    //memcpy(dst, src, 1024);
    int l = farm_memcopy_asm(dst, src, 8192, 0);
    printf("\n\n%d\n", l);

    for (i=0; i<4096; i++) {
//        printf("%x - %x\n", dst[i], src[i]);
    }
    exit(0);
*/

    //////////////////////////////////////////
    
    //local buffer
    lbuff = memalign(PAGE_SIZE, buf_size*sizeof(uint8_t));
    if (thread_buf_size && lbuff == NULL) {
        fprintf(stdout, "Local buffer could not be allocated. Memalign failed.\n");
        return 1;
    } 
    retcode = mlock(lbuff, buf_size*sizeof(uint8_t));
    if (retcode != 0) fprintf(stdout, "Local buffer mlock returned %d (buffer size = %lu bytes)\n", retcode, buf_size*sizeof(uint8_t));

    uint32_t counter = 0;
    //initialize the local buffer
    for(i=0; i<(buf_size*sizeof(uint8_t)); i+=PAGE_SIZE) {
        lbuff[i] = 0;
        counter = i*sizeof(uint8_t)/PAGE_SIZE;
        call_magic_2_64((uint64_t)&(lbuff[i]), BUFFER, counter);
    }

    //context buffer - exposed to remote nodes
    //if (snid == 1) {//WARNING: Only app that is given snid = 1 registers context
    ctxbuff = memalign(PAGE_SIZE, ctx_size);
    memset(ctxbuff, 0, ctx_size);
    if (ctxbuff == NULL) {
        fprintf(stdout, "Context buffer could not be allocated. Memalign failed.\n");
        return 1;
    } 
    retcode = mlock(ctxbuff, ctx_size*sizeof(uint8_t));
    if (retcode != 0) fprintf(stdout, "Context buffer mlock returned %d\n", retcode);

    counter = 0;
    //initialize the context buffer
    uintptr_t ctx_ptr = ctxbuff;
    ((data_object_t *)ctx_ptr)->lock = 0;
    call_magic_2_64((uint64_t)ctx_ptr, CONTEXTMAP, 0);
    for(i=0; i<num_objects; i++) {
        ((data_object_t *)ctx_ptr)->version = 0;
        ((data_object_t *)ctx_ptr)->lock = 0;
	((data_object_t *)ctx_ptr)->key = counter;
/*        printf("%p: %p, %p, %p\n", ctx_ptr, &(((data_object_t *)ctx_ptr)->version), 
                &(((data_object_t *)ctx_ptr)->lock),
                &(((data_object_t *)ctx_ptr)->key));
*/
        counter++;
        call_magic_2_64((uint64_t)ctx_ptr, CONTEXT, counter);
        ctx_ptr += data_obj_size;
    }
    //fprintf(stdout, "Allocated %d pages for the context\n", counter);
    //}
    //register ctx and buffer sizes, only needed for the flexi version of the app; pass this info anyway
    call_magic_2_64(42, BUFFER_SIZE, buf_size);
    call_magic_2_64(42, CONTEXT_SIZE, ctx_size);
    fprintf(stdout,"Init done! Allocated %d objects of %lu bytes (pure object data = %d, total size = %lu bytes).\nLocal buffer size = %lu Bytes.\nWill now allocate per-thread QPs and run with %d reader threads (SYNC) and %d writer threads.\nEach thread will execute %d ops (reads or writes).\nObject strided access: %d Bytes\n", 
		num_objects, data_obj_size, data_obj_size, ctx_size, 
		buf_size,
		readers,writers,
		iters,OBJECT_BYTE_STRIDE);

    //Now prepare the threads
    pthread_t *threads;
    pthread_attr_t pthread_custom_attr;
    parm *p;  

    threads=(pthread_t *)malloc(num_threads*sizeof(*threads));
    pthread_barrier_init (&barrier, NULL, num_threads);
    pthread_attr_init(&pthread_custom_attr);
    
    p=(parm *)malloc(sizeof(parm)*num_threads);
    int reader_prio[16]={0,4,8,12,1,5,9,13,2,6,10,14,3,7,11,15};
    int rd_idx=0, wr_idx=15, core;
    /* Start up thread */
    for (i=0; i<num_threads; i++) {
	p[i].id=i;
	if (i<readers) {
		p[i].is_reader = 1;
		core = reader_prio[rd_idx];
		rd_idx++;
		pthread_create(&threads[i], &pthread_custom_attr, par_phase_read, (void *)(p+i));
		int error = processor_bind(P_LWPID, threads[i], core, NULL);
		if (error) {
			printf("Could not bind reader thread %d to core %d! (error %d)\n", i, core, error);
	      	} else {
			#ifdef MY_DEBUG
			printf("Bound reader thread %d to core %d\n", i, core);
			#endif
       	 	}
        } else {
		p[i].is_reader = 0;
                core = reader_prio[wr_idx];
		wr_idx--;
		pthread_create(&threads[i], &pthread_custom_attr, par_phase_write, (void *)(p+i));
	        int error = processor_bind(P_LWPID, threads[i], core, NULL);
		if (error) {
			printf("Could not bind writer thread %d to core %d! (error %d)\n", i, core, error);
	      	} else {
			#ifdef MY_DEBUG
			printf("Bound writer thread %d to core %d\n", i, core);
			#endif
       	 	}
        }	
    }

    /* Synchronize the completion of each thread. */
    for (i=0; i<num_threads; i++) {
	pthread_join(threads[i],NULL);
    }
    free(p);
    free(lbuff);
    free(ctxbuff);

    return 0;
}

