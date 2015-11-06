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
#include "../../libsonuma/sonuma.h"
#include "../../libsonuma/magic_iface.h"
#include <sys/pset.h>

#define ITERS 100000000
#define DATA_SIZE 1024	//in bytes
#define CTX_ID 0
#define DST_NID 1

//RMW for atomic lock acquirement
static inline __attribute__ ((always_inline))
    uint8_t acquire_lock(volatile uint8_t *address) {
        uint8_t ret_value;
        __asm__ __volatile__ (
                "ldstub [%1], %%l0\n\t"
                "mov %%l0, %0\n\t"  
                : "=r"(ret_value)    //output
                : "r"(address)    //input registers
            	: "%l0", "memory" //clobbered registers
                );
         return ret_value;
    }

//data object's struct
typedef struct data_object {
  uint64_t version;
  volatile uint8_t lock;
  uint32_t key;
  uint8_t value[DATA_SIZE];
  uint8_t padding[64-(DATA_SIZE+16)%64]; //All objects need to be cache-line-aligned
} data_object_t;

typedef struct {
	int id;
	int is_reader;
} parm;

uint8_t *lbuff;
data_object_t *ctxbuff;  
int iters, num_objects;
uint64_t thread_buf_size;
int readers, writers;

pthread_barrier_t   barrier; 

void * par_phase_write(void *arg) {
    parm *p=(parm *)arg;
     
    int i,j,luckyObj,offset;
    srand(p->id);		//remove this for lots of conflicts :-)
    pthread_barrier_wait (&barrier);
    if (!p->id) call_magic_2_64(1, ALL_SET, 1); //INIT DONE
     
    for (i = 0; i<iters; i++) {
	luckyObj = rand() % num_objects;
	offset = luckyObj * sizeof(data_object_t);
  	uint8_t prevLockVal;
	while (ctxbuff[luckyObj].lock);	//TTS
	do {
		prevLockVal = acquire_lock(&(ctxbuff[luckyObj].lock));	//Test-and-set
		if (prevLockVal) {
		//	call_magic_2_64(luckyObj, LOCK_SPINNING, prevLockVal);	//signal the completion of a write
			#ifdef MY_DEBUG
			printf("thread %d failed to grab lock of item %d! (lock value = %"PRIu8")\n", p->id, luckyObj, prevLockVal);
			usleep(10);
			#endif
		}
	} while (prevLockVal);
	for (j=0; j<DATA_SIZE; j++) {
		ctxbuff[luckyObj].value[j] ^= 1;
	}
	#ifdef MY_DEBUG
  	printf("version = %" PRIu64 ",\t\
	  lock = %" PRIu8 ",\t\
	  key = %" PRIu32 "\n", ctxbuff[luckyObj].version, ctxbuff[luckyObj].lock, ctxbuff[luckyObj].key);
	#endif
	ctxbuff[luckyObj].key ^= 7;  //random operation on object
	for (j=0; j<DATA_SIZE; j+=2) 
		ctxbuff[luckyObj].value[j] = (uint8_t)i;
	ctxbuff[luckyObj].version++;
	ctxbuff[luckyObj].lock = 0;	//unlock
	call_magic_2_64(luckyObj, OBJECT_WRITE, i);	//signal the completion of a write
	#ifdef MY_DEBUG
  	printf("Thread %d: \tprevious lock value = %" PRIu8 ",\t\
	  	new lock value = %" PRIu8 ",\t\
		new version = %" PRIu64 "\n", p->id, prevLockVal, ctxbuff[luckyObj].lock, ctxbuff[luckyObj].version);
	#endif
    }
    call_magic_2_64(0, BENCHMARK_END, 0);	//this threads completed its work and it's exiting
}

void * par_phase_read(void *arg) {
    parm *p=(parm *)arg;
    
    rmc_wq_t *wq;
    rmc_cq_t *cq;;

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


    int j,luckyObj,offset;
    srand(p->id);		//remove this for lots of conflicts :-)
    pthread_barrier_wait (&barrier);
    if (!p->id) call_magic_2_64(1, ALL_SET, 1); //INIT DONE
 
    uint8_t *lbuff_slot, *thread_buf_base;
    uint64_t ctx_offset, op_count = 0;
    uint32_t wq_head, cq_tail;
    thread_buf_base = lbuff + thread_buf_size * p->id; //this is the local buffer's base address for this thread

//reader kernel    
    uint8_t success = 1; 
    for (i = 0; i<iters; i++) {
	if (!success) {	//if previous read did not succeed, retry it
        	luckyObj = rand() % num_objects;
		offset = luckyObj * sizeof(data_object_t);
	        lbuff_slot = (uint8_t *)(thread_buf_base + ((op_count * sizeof(data_object_t)) % thread_buf_size));
       		ctx_offset = luckyObj * sizeof(data_object_t);
	}
        wq_head = wq->head;
        create_wq_entry(RMC_SABRE, wq->SR, CTX_ID, DST_NID, (uint64_t)lbuff_slot, ctx_offset, sizeof(data_object_t)>>6, (uint64_t)&(wq->q[wq_head]));
        call_magic_2_64(wq_head, NEWWQENTRY, op_count);
	wq->head =  wq->head + 1;
	if (wq->head >= MAX_NUM_WQ) {
 	    	wq->head = 0;
      		wq->SR ^= 1;
    	}
        //sync
        cq_tail = cq->tail;
        while(cq->q[cq_tail].SR != cq->SR) {}	//wait for request completion (sync mode)
	success = cq->q[cq_tail].success;
	if (success) {	//No atomicity violation!
        	call_magic_2_64(cq_tail, SABRE_SUCCESS, op_count);
		//touch the data
		uint64_t accum;
		for (j=0; j<DATA_SIZE; j+=2) 
			accum += (uint8_t)*(lbuff_slot + j);
	        *(lbuff_slot) = (uint8_t)accum;
	} else {	//Atomicity violation detected
        	call_magic_2_64(cq_tail, SABRE_ABORT, op_count);
	}
        wq->q[cq->q[cq_tail].tid].valid = 0;	//free WQ entry

       	cq->tail = cq->tail + 1;
       	if (cq->tail >= MAX_NUM_WQ) {
      		cq->tail = 0;
      		cq->SR ^= 1;
        }    
	op_count++;
	
    }
    call_magic_2_64(0, BENCHMARK_END, 0);	//this threads completed its work and it's exiting
    free(wq);
    free(cq);
}

int main(int argc, char **argv)
{
    int i, it_count = 0, retcode, num_threads; 
    iters = ITERS;

    int num_iter = (int)ITERS;
    if (argc != 4) {
        fprintf(stdout,"Usage: %s <num_objects> <num_readers> <num_writers>\n", argv[0]);
        return 1;  
    }
    num_objects = atoi(argv[1]);
    uint64_t ctx_size = num_objects*sizeof(data_object_t);
    readers = atoi(argv[2]) ;
    writers = atoi(argv[3]) ;
    num_threads = readers+writers;
    assert(num_threads <= 16);
    uint64_t buf_size = sizeof(data_object_t) * readers * MAX_NUM_WQ; 
    if (readers) thread_buf_size = buf_size / readers;
    else thread_buf_size = 0;
    
    //local buffer
    lbuff = memalign(PAGE_SIZE, buf_size*sizeof(uint8_t));
    if (thread_buf_size && lbuff == NULL) {
        fprintf(stdout, "Local buffer could not be allocated. Memalign returned %"PRIu64"\n", 0);
        return 1;
    } 
    retcode = mlock(lbuff, buf_size*sizeof(uint8_t));
    if (retcode != 0) fprintf(stdout, "Local buffer mlock returned %d (buffer size = %d bytes)\n", retcode, buf_size*sizeof(uint8_t));

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
    if (ctxbuff == NULL) {
        fprintf(stdout, "Context buffer could not be allocated. Memalign returned %"PRIu64"\n", 0);
        return 1;
    } 
    retcode = mlock(ctxbuff, ctx_size*sizeof(uint8_t));
    if (retcode != 0) fprintf(stdout, "Context buffer mlock returned %d\n", retcode);

    counter = 0;
    //initialize the context buffer
    ctxbuff[0].lock = 0;
    call_magic_2_64((uint64_t)ctxbuff, CONTEXTMAP, 0);
    for(i=0; i<num_objects; i++) {
        ctxbuff[i].lock = 0;
        ctxbuff[i].version = 0;
	ctxbuff[i].key = counter;
        counter++;
        call_magic_2_64((uint64_t)&(ctxbuff[i]), CONTEXT, counter);
    } 
    //fprintf(stdout, "Allocated %d pages for the context\n", counter);
    //}
    //register ctx and buffer sizes, only needed for the flexi version of the app; pass this info anyway
    call_magic_2_64(42, BUFFER_SIZE, buf_size);
    call_magic_2_64(42, CONTEXT_SIZE, ctx_size);
    fprintf(stdout,"Init done! Allocated %d objects of %d bytes (pure object data = %d, total size = %d bytes).\
		\nLocal buffer size = %d Bytes.\
		\nWill now allocate per-thread QPs and run with %d reader threads and %d writer threads.\
		\nEach thread will execute %d ops (reads or writes).\n", 
		num_objects, sizeof(data_object_t), DATA_SIZE, ctx_size, 
		buf_size,
		readers,writers,
		iters);

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
			printf("Bound writer thread %d to core %d\n", i, core);
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