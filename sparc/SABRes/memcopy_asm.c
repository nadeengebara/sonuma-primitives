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

#define PASS2FLEXUS_MEASURE(...)    call_magic_2_64(__VA_ARGS__)
#define DEBUG_ASSERT(...)
#define MIN(a,b) (((a)<(b))?(a):(b))
#define APP_BUFF_SIZE (PAGE_SIZE*2)

#define LLC_SIZE (4*1024*1024)

#define ITERS 100000000
//#define DATA_SIZE 1024	//in bytes <- ustiugov: add to compiler options -D DATA_SIZE=<value>
#define OBJECT_BYTE_STRIDE 128  //how much of the object will be touched by readers/writers (128 -> 50% (half of the blocks))
#define CTX_ID 0
#define DST_NID 1

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

//RMW for atomic lock acquirement
static inline uint8_t acquire_lock(volatile uint8_t *ptr) {
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
uint16_t readers, writers;

pthread_barrier_t   barrier; 

inline int fast_memcpy_from_nam_buf(void *obj, void *buf, uintptr_t nam, size_t total_size) {
    uintptr_t src = (uintptr_t)buf;
    uintptr_t dst = (uintptr_t)obj;
    size_t size = total_size;

    /////////
    int count = 0;
    /////////

#ifdef NO_SW_VERSION_CONTROL
    memcpy((void *)dst, (void *)src, size);
    return 1;
#else

    // calculate the first address in the second cache line, using the nam pointer
    // as we might be copying data from a differently aligned RDMA read buffer
    uintptr_t first_cl_end = (nam + CACHE_LINE_SIZE) & ~(CACHE_LINE_SIZE - 1);
    //size_t copy_size = std::min(first_cl_end - nam, size);
    size_t copy_size = MIN(first_cl_end - nam, size);
    //DEBUG_ASSERT((sizeof(size_t) == 8) && (copy_size & 0x7) == 0 && (dst & 0x7) == 0);
    DEBUG_ASSERT((sizeof(size_t) == 8) && (copy_size & 0x7) == 0 && (dst & 0x7) == 0);

    // Now get the version. 
    header_t *hdr = (header_t *)dst;


    size -= copy_size;
    first_cl_end = src + copy_size;
    while (src < first_cl_end) {
        *(size_t*)dst = *(size_t*)src;
        dst+=8;
        src+=8;
    }

    // Now get the version. Because the version is the first word in the
    // header we know that it has been copied to the output buffer.
    // if object is being updated, return immediately
    if(version_is_updating(hdr->version)) {
        count++;
#ifndef FAKE_REMOTE_READS
        return 0;
#endif
    }

    // This is the version we expect in each cache line.
    nam_cl_version_t hdrver = version_get_cl_version_bits(hdr->version);

    while (1) {
        if (size >= DATA_BYTES_PER_CACHE_LINE) {
            // check the version and return 0 immediately if it doesn't match
            if(*(nam_cl_version_t *)src != hdrver) {
                count++;
#ifndef FAKE_REMOTE_READS
                return 0;
#endif
            }

            DEBUG_ASSERT((sizeof(unsigned short) == 2) && ((dst & 0x1) == 0) && ((src & 0x1) == 0) );

            src += sizeof(nam_cl_version_t);

            size_t i;
            for (i=0; i < DATA_BYTES_PER_CACHE_LINE; i+=2) {
                *((unsigned short*)(dst + i)) = *((unsigned short*)(src + i));
            }
            dst += DATA_BYTES_PER_CACHE_LINE;
            src += DATA_BYTES_PER_CACHE_LINE; 
            size -= DATA_BYTES_PER_CACHE_LINE;
        } else {
            break;
        }
    }

    // check the cache line version in the last cache line
    if(size > 0) {
        // check the version and return 0 immediately if it doesn't match
        if(*(nam_cl_version_t *)src != hdrver) {
            count++;
#ifndef FAKE_REMOTE_READS
            return 0;
#endif
        }

        src += sizeof(nam_cl_version_t);
    }

    // deal with the last cache line

    DEBUG_ASSERT((size & 0x1) == 0);
    while (size > 0) {
        *(unsigned short*)dst = *(unsigned short*)src;
        size -= 2;
        dst += 2;
        src += 2;
    }

    // if we are here, all cache line versions match
    return 1;
#endif /* NO_SW_VERSION_CONTROL */
}

void my_memcopy(void *dst, void *src, int size) {
    int i;
    for (i=0; i < size; i+=8) {
        *(size_t*)dst = *(size_t*)src;
        dst+=8;
        src+=8;
    }
}

void my_memcopy_asm(void *dst, void *src, int size) {
    int i, ret_value;
    for (i=0; i < size; i+=CACHE_LINE_SIZE) {
        __asm__ __volatile__ (
                "ldd [%1], %%f0\n\t"
                "ldd [%1+8], %%f2\n\t"
                "ldd [%1+16], %%f4\n\t"
                "ldd [%1+24], %%f6\n\t"
                "ldd [%1+32], %%f8\n\t"
                "ldd [%1+40], %%f10\n\t"
                "ldd [%1+48], %%f12\n\t"
                "ldd [%1+56], %%f14\n\t"
                "std %%f0, [%2]\n\t"
                "std %%f2, [%2+8]\n\t"
                "std %%f4, [%2+16]\n\t"
                "std %%f6, [%2+24]\n\t"
                "std %%f8, [%2+32]\n\t"
                "std %%f10, [%2+40]\n\t"
                "std %%f12, [%2+48]\n\t"
                "std %%f14, [%2+56]\n\t"
                : "=r"(ret_value)     /* output registers*/
                : "r"(src), "r"(dst)      /* input registers*/
                : "%f0", "%f1", "%f2", "%f3", "%f4", "%f5", "%f6", "%f7"   /* clobbered registers*/
                );
        //*(size_t*)dst = *(size_t*)src;
        dst+=CACHE_LINE_SIZE;
        src+=CACHE_LINE_SIZE;
    }
}

//static inline __attribute__ ((always_inline))
void my_memcopy_asm_unroll(void *dst, void *src, int size, int k) {
    int i, ret_value;
    PASS2FLEXUS_MEASURE(k, MEASUREMENT, 20);
    for (i=0; i < 8; i++) {
        // by 8 byte
        __asm__ __volatile__ (
                "ldd [%1], %%f0\n\t"
                "ldd [%1+64], %%f2\n\t"
                "ldd [%1+128], %%f4\n\t"
                "ldd [%1+192], %%f6\n\t"
                "ldd [%1+256], %%f8\n\t"
                "ldd [%1+320], %%f10\n\t"
                "ldd [%1+384], %%f12\n\t"
                "ldd [%1+448], %%f14\n\t"
                "ldd [%1+512], %%f16\n\t"
                "ldd [%1+576], %%f18\n\t"
                "ldd [%1+640], %%f20\n\t"
                "ldd [%1+704], %%f22\n\t"
                "ldd [%1+768], %%f24\n\t"
                "ldd [%1+832], %%f26\n\t"
                "ldd [%1+896], %%f28\n\t"
                "ldd [%1+960], %%f30\n\t"
                "std %%f0, [%2]\n\t"
                "std %%f2, [%2+64]\n\t"
                "std %%f4, [%2+128]\n\t"
                "std %%f6, [%2+192]\n\t"
                "std %%f8, [%2+256]\n\t"
                "std %%f10, [%2+320]\n\t"
                "std %%f12, [%2+384]\n\t"
                "std %%f14, [%2+448]\n\t"
                "std %%f16, [%2+512]\n\t"
                "std %%f18, [%2+576]\n\t"
                "std %%f20, [%2+640]\n\t"
                "std %%f22, [%2+704]\n\t"
                "std %%f24, [%2+768]\n\t"
                "std %%f26, [%2+832]\n\t"
                "std %%f28, [%2+896]\n\t"
                "std %%f30, [%2+960]\n\t"
                : "=r"(ret_value)     /* output registers*/
                : "r"(src), "r"(dst)      /* input registers*/
                : "%f0", "%f1", "%f2", "%f3", "%f4", "%f5", "%f6", "%f7"   /* clobbered registers*/
                );
        //*(size_t*)dst = *(size_t*)src;
        dst+=8;
        src+=8;
    }
    PASS2FLEXUS_MEASURE(k, MEASUREMENT, 30);
}

void my_memcopy_asm_unroll_coalesc(void *dst, void *src, int size, int k) {
    int i=0, ret_value;
    PASS2FLEXUS_MEASURE(k, MEASUREMENT, 20);
        // by 8 byte
        __asm__ __volatile__ (
                "ldd [%1], %%f0\n\t"
                "ldd [%1+64], %%f2\n\t"
                "ldd [%1+128], %%f4\n\t"
                "ldd [%1+192], %%f6\n\t"
                "ldd [%1+256], %%f8\n\t"
                "ldd [%1+320], %%f10\n\t"
                "ldd [%1+384], %%f12\n\t"
                "ldd [%1+448], %%f14\n\t"
                "ldd [%1+512], %%f16\n\t"
                "ldd [%1+576], %%f18\n\t"
                "ldd [%1+640], %%f20\n\t"
                "ldd [%1+704], %%f22\n\t"
                "ldd [%1+768], %%f24\n\t"
                "ldd [%1+832], %%f26\n\t"
                "ldd [%1+896], %%f28\n\t"
                "ldd [%1+960], %%f30\n\t"
                "std %%f0, [%2]\n\t"
                "std %%f2, [%2+64]\n\t"
                "std %%f4, [%2+128]\n\t"
                "std %%f6, [%2+192]\n\t"
                "std %%f8, [%2+256]\n\t"
                "std %%f10, [%2+320]\n\t"
                "std %%f12, [%2+384]\n\t"
                "std %%f14, [%2+448]\n\t"
                "std %%f16, [%2+512]\n\t"
                "std %%f18, [%2+576]\n\t"
                "std %%f20, [%2+640]\n\t"
                "std %%f22, [%2+704]\n\t"
                "std %%f24, [%2+768]\n\t"
                "std %%f26, [%2+832]\n\t"
                "std %%f28, [%2+896]\n\t"
                "std %%f30, [%2+960]\n\t"
                : "=r"(ret_value)     /* output registers*/
                : "r"(src), "r"(dst)      /* input registers*/
                : "%f0", "%f1", "%f2", "%f3", "%f4", "%f5", "%f6", "%f7"   /* clobbered registers*/
                );
        //*(size_t*)dst = *(size_t*)src;
    for (i=0; i < 4; i++) {
        __asm__ __volatile__ (
//                "ldd [%1], %%f0\n\t"
                "ldd [%1+8], %%f2\n\t"
                "ldd [%1+16], %%f4\n\t"
                "ldd [%1+24], %%f6\n\t"
                "ldd [%1+32], %%f8\n\t"
                "ldd [%1+40], %%f10\n\t"
                "ldd [%1+48], %%f12\n\t"
                "ldd [%1+56], %%f14\n\t"

                "ldd [%1+72], %%f16\n\t"
                "ldd [%1+80], %%f18\n\t"
                "ldd [%1+88], %%f20\n\t"
                "ldd [%1+96], %%f22\n\t"
                "ldd [%1+104], %%f24\n\t"
                "ldd [%1+112], %%f26\n\t"
                "ldd [%1+120], %%f28\n\t"
//                "ldd [%1+], %%f30\n\t"
//                "std %%f0, [%2]\n\t"
                "std %%f2, [%2+8]\n\t"
                "std %%f4, [%2+16]\n\t"
                "std %%f6, [%2+24]\n\t"
                "std %%f8, [%2+32]\n\t"
                "std %%f10, [%2+40]\n\t"
                "std %%f12, [%2+48]\n\t"
                "std %%f14, [%2+56]\n\t"

                "std %%f16, [%2+72]\n\t"
                "std %%f18, [%2+80]\n\t"
                "std %%f20, [%2+88]\n\t"
                "std %%f22, [%2+96]\n\t"
                "std %%f24, [%2+104]\n\t"
                "std %%f26, [%2+112]\n\t"
                "std %%f28, [%2+120]\n\t"
//                "std %%f30, [%2+960]\n\t"
                : "=r"(ret_value)     /* output registers*/
                : "r"(src), "r"(dst)      /* input registers*/
                : "%f0", "%f1", "%f2", "%f3", "%f4", "%f5", "%f6", "%f7"   /* clobbered registers*/
                );
        dst+=128;
        src+=128;
    }
    PASS2FLEXUS_MEASURE(k, MEASUREMENT, 30);
}

void * par_phase_write(void *arg) {
    parm *p=(parm *)arg;
     
    int i,j,luckyObj;
    srand(p->id);		//remove this for lots of conflicts :-)
    pthread_barrier_wait (&barrier);
    if (!p->id) call_magic_2_64(1, ALL_SET, 1); //INIT DONE
     
    uint8_t prevLockVal;
    for (i = 0; i<iters; i++) {
	luckyObj = rand() % num_objects;
	while (ctxbuff[luckyObj].lock);	//TTS
	do {
		prevLockVal = acquire_lock(&(ctxbuff[luckyObj].lock));	//Test-and-set
		if (prevLockVal) {
			call_magic_2_64(luckyObj, LOCK_SPINNING, prevLockVal);	//signal the completion of a write
			#ifdef MY_DEBUG
			printf("thread %d failed to grab lock of item %d! (lock value = %"PRIu8")\n", p->id, luckyObj, prevLockVal);
			usleep(10);
			#endif
		}
	} while (prevLockVal);
        #ifdef MEASURE_TS
	call_magic_2_64(luckyObj, CS_START, i);	//signal the beginning of the CS
        #endif
	for (j=0; j<DATA_SIZE; j++) {
		ctxbuff[luckyObj].value[j] ^= 1;
	}
	#ifdef MY_DEBUG
  	printf("version = %" PRIu64 ",\t\
	  lock = %" PRIu8 ",\t\
	  key = %" PRIu32 "\n", ctxbuff[luckyObj].version, ctxbuff[luckyObj].lock, ctxbuff[luckyObj].key);
	#endif
	ctxbuff[luckyObj].key ^= 7;  //random operation on object
	for (j=0; j<DATA_SIZE; j+=OBJECT_BYTE_STRIDE) 
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

    return NULL;
}

void * par_phase_read(void *arg) {
    parm *p=(parm *)arg;
    
    rmc_wq_t *wq;
    rmc_cq_t *cq;;

    uint8_t *app_buff;
    app_buff = memalign(PAGE_SIZE, APP_BUFF_SIZE);
    unsigned payload_cache_blocks = sizeof(data_object_t)>>6;

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
    int obj_buf_size_ = num_objects*sizeof(data_object_t);
    for(i=0; i<obj_buf_size_; i+=64) {
        temp += ( (uint8_t *)(ctxbuff) )[i];
    }
    for(i=0; i<APP_BUFF_SIZE; i+=64) {
        temp += ( (uint8_t *)(app_buff) )[i];
    }
    //printf("LOL\n");
    printf("%x", temp);
    //////////////////////////////

    int j,luckyObj;
    srand(p->id);		//remove this for lots of conflicts :-)
    pthread_barrier_wait (&barrier);
    if (!p->id) call_magic_2_64(1, ALL_SET, 1); //INIT DONE
 
    uint8_t *lbuff_slot, *thread_buf_base;
    uint64_t ctx_offset, op_count = 0;
    uint32_t wq_head, cq_tail;
    thread_buf_base = lbuff + thread_buf_size * p->id; //this is the local buffer's base address for this thread

//reader kernel    
int k = 0, z = 1;
    uint8_t success; 
    for (i = 0; i<iters; i++) {
/*
        success = 0;
        PASS2FLEXUS_MEASURE(i, MEASUREMENT, 0);
        luckyObj = rand() % num_objects;
        lbuff_slot = (uint8_t *)(thread_buf_base + ((luckyObj * sizeof(data_object_t)) % thread_buf_size));
        ctx_offset = luckyObj * 1024;//sizeof(data_object_t);
        wq_head = wq->head;
*/

        //my_memcopy_asm_unroll((void*)app_buff, (void*)&( ctxbuff[(i*7)%num_objects] ), 1024, i);
        my_memcopy_asm_unroll_coalesc((void*)app_buff, (void*)&( ctxbuff[(i*7)%num_objects] ), 1024, i);


        for(k=0; k<300; k++) {
            z = k*z;
        }

/*
        while (!success) {	//read the object again if it's not consistent
            create_wq_entry(RMC_SABRE, wq->SR, CTX_ID, DST_NID, (uint64_t)lbuff_slot, ctx_offset, payload_cache_blocks, (uint64_t)&(wq->q[wq_head]));
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
            wq->q[cq->q[cq_tail].tid].valid = 0;	//free WQ entry

            cq->tail = cq->tail + 1;
            if (cq->tail >= MAX_NUM_WQ) {
                cq->tail = 0;
                cq->SR ^= 1;
            }    
            PASS2FLEXUS_MEASURE(i, MEASUREMENT, 10);
            if (success) {	//No atomicity violation!
                call_magic_2_64(cq_tail, SABRE_SUCCESS, op_count);

                PASS2FLEXUS_MEASURE(i, MEASUREMENT, 20);
                fast_memcpy_from_nam_buf((void*)app_buff, (void*)lbuff_slot, (uintptr_t) lbuff_slot, payload_cache_blocks*CACHE_LINE_SIZE);
                PASS2FLEXUS_MEASURE(i, MEASUREMENT, 30);
                //
                //touch the data
                uint64_t accum;
                for (j=0; j<DATA_SIZE; j+=OBJECT_BYTE_STRIDE) 
                accum += (uint64_t)*(lbuff_slot + j);
                 *(lbuff_slot) = accum;
                 ///
            } else {	//Atomicity violation detected
                call_magic_2_64(cq_tail, SABRE_ABORT, op_count);
            }

            op_count++;

        }
*/
        //PASS2FLEXUS_MEASURE(i, MEASUREMENT, 40);

    }
    printf("%d", z);
    call_magic_2_64(0, BENCHMARK_END, 0);	//this threads completed its work and it's exiting
    free(wq);
    free(cq);

    return NULL;
}

int main(int argc, char **argv)
{
    int i, retcode, num_threads; 
    iters = ITERS;

    if (argc != 4) {
        fprintf(stdout,"Usage: %s <num_objects> <num_readers> <num_writers>\n", argv[0]);
        return 1;  
    }
#ifndef DATA_SIZE
    assert(0); // add to compiler options -D DATA_SIZE=<value>
#endif
#ifdef NO_SW_VERSION_CONTROL
    fprintf(stdout,"Running without SW versions memcopy\n");
#else
    fprintf(stdout,"Running with SW versions memcopy\n");
#endif
    fprintf(stdout,"Application buffer size is %d bytes\n", APP_BUFF_SIZE);
    assert(sizeof(data_object_t)%64 == 0);
    num_objects = atoi(argv[1]);
    uint64_t ctx_size = num_objects*sizeof(data_object_t);
    readers = atoi(argv[2]) ;
    writers = atoi(argv[3]) ;
    num_threads = readers+writers;
    assert(num_threads <= 16);
    uint64_t buf_size = sizeof(data_object_t) * readers * MAX_NUM_WQ; 
    if (readers) thread_buf_size = buf_size / readers;
    else thread_buf_size = 0;

    //////////////////////////////////////////
    /*
    unsigned *dst = memalign(PAGE_SIZE, PAGE_SIZE);
    unsigned *src = memalign(PAGE_SIZE, PAGE_SIZE);
    memset(src, 0xab, PAGE_SIZE);
    //my_memcopy_asm_unroll(dst, src, 1024);
    //my_memcopy_asm_unroll_coalesc(dst, src, 1024, 0);
    //my_memcopy_asm(dst, src, 1024);
    memcpy(dst, src, 1024);

    for (i=0; i<512; i+=2) {
        printf("%x - %x\n", dst[i], src[i]);
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
    if (ctxbuff == NULL) {
        fprintf(stdout, "Context buffer could not be allocated. Memalign failed.\n");
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
    fprintf(stdout,"Init done! Allocated %d objects of %lu bytes (pure object data = %d, total size = %lu bytes).\nLocal buffer size = %lu Bytes.\nWill now allocate per-thread QPs and run with %d reader threads (SYNC) and %d writer threads.\nEach thread will execute %d ops (reads or writes).\nObject strided access: %d Bytes\n", 
		num_objects, sizeof(data_object_t), DATA_SIZE, ctx_size, 
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
