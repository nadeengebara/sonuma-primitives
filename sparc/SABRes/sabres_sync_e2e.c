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
#include <signal.h>
#include <time.h>
#include <assert.h>
#include <sys/mman.h>

#ifdef OLD_DEF
#include </net2/daglis/soNUMA/magic_iface.h>
#include </net2/daglis/soNUMA/RMCdefines.h>
#include "son_asm.h"
#else
    #include "../../libsonuma/sonuma.h"
    #include "../../libsonuma/magic_iface.h"
#endif /* OLD_DEF */

#include <sys/pset.h>

#ifdef OLD_DEF // old lib is used

    static inline __attribute__ ((always_inline))
    uint64_t  call_magic_sim_break(uint64_t son_function, uint64_t arg1, uint64_t arg2){
        uint64_t  ret_value;

        __asm__ __volatile__ (
                "mov %1, %%l0\n\t"
                "mov %2, %%l1\n\t"
                "mov %3, %%l2\n\t"
                "sethi 1, %%g0\n\t"
                "mov %%l0, %0\n\t"
                : "=r"(ret_value)     /* output registers*/
                : "r"(son_function), "r"(arg1), "r"(arg2)      /* input registers*/
                : "%l0", "%l1", "%l2"   /* clobbered registers*/
                );

        return ret_value;
    }

#define OBJECT_WRITE        26	//used by writers in SABRe experiments, to count number of object writes
#define LOCK_SPINNING	    27
#define CS_START            28
#define MEASUREMENT 99

    #define CREATE_WQ_ENTRY(...)    create_wq_entry2(__VA_ARGS__)
    #define PASS2FLEXUS_MEASURE(...)    call_magic_2_64(__VA_ARGS__)
    #define PASS2FLEXUS_DBG(...)
    #define PASS2FLEXUS_DBG_OLD(...)    call_magic_2_64(__VA_ARGS__)
    #define PASS2FLEXUS_MEASURE_OLD(...)    call_magic_2_64(__VA_ARGS__)
#else // new lib is used
    #define CREATE_WQ_ENTRY(...)    create_wq_entry(__VA_ARGS__)
    #define PASS2FLEXUS_MEASURE(...)    call_magic_2_64(__VA_ARGS__)
    #define PASS2FLEXUS_DBG(...)    call_magic_2_64(__VA_ARGS__)
    #define PASS2FLEXUS_MEASURE_OLD(...)
#endif
#define PASS2FLEXUS_MEASURE_COMMON(...)    call_magic_2_64(__VA_ARGS__)

#define DEBUG_ASSERT(...)
#define MIN(a,b) (((a)<(b))?(a):(b))
#define APP_BUFF_SIZE (PAGE_SIZE*2)

#define errExit(msg)    do { perror(msg); exit(EXIT_FAILURE); \
                           } while (0)

#define CLOCKID CLOCK_REALTIME
#define SIG SIGRTMIN

#define LLC_SIZE (4*1024*1024)

#define ITERS 1000000
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
#define MEASURE_TS //to measure the latency of the writers' critical section
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
int write_freq;

pthread_barrier_t   barrier; 

/** Copies a single data object.
 *  We assume data_object_t layout for dst.
 *  We assume data_object_t layout with cl_versions for src.
 *  Currently, we assume 8-byte versions for both the header and per-cl versions.
 */
__attribute__ ((noinline))
int farm_memcopy_asm(void *obj, void *buf, size_t total_size, int m) {
asm("");
    uintptr_t src = (uintptr_t)buf;
    const uintptr_t start_src = (uintptr_t)src;
    uintptr_t dst = (uintptr_t)obj;
    size_t size = total_size;
    int block_num = total_size/CACHE_LINE_SIZE;

#ifdef NO_SW_VERSION_CONTROL
    switch(memcpyID) {
        case 16:
            return my_memcopy((void *)dst, (void *)src, size, m);
            break;
        case 8:
            return my_memcopy8((void *)dst, (void *)src, size, m);
            break;
        case 4:
            return my_memcopy4((void *)dst, (void *)src, size, m);
            break;
        case 2:
            return my_memcopy2((void *)dst, (void *)src, size, m);
            break;
        default:
            assert(0);
    }
#else
    return farm_memcopy_versions((void *)dst, (void *)src, size, m);
#endif /* NO_SW_VERSION_CONTROL */
}


int farm_memcopy_versions(void *obj, void *buf, size_t total_size, int m) {
    uintptr_t src = (uintptr_t)buf;
    const uintptr_t start_src = (uintptr_t)src;
    uintptr_t dst = (uintptr_t)obj;
    size_t size = total_size;
    int block_num = total_size/CACHE_LINE_SIZE;

    data_object_t *my_obj = (data_object_t *)src;

    PASS2FLEXUS_MEASURE(m, MEASUREMENT, 20);
    if (my_obj->lock) {
        // the object is updating
        //assert(0);
        return 0;
    }

    version_t hdr_version = my_obj->version;
    version_t *hdr_version_ptr = &hdr_version;

    int i, k, p, ret_value;
    src += hdr_size;

    // first cache line
    __asm__ __volatile__ (
            // we assume HDR_SIZE = 16 bytes (version + lock + key)
            "ldd [%1], %%f0\n\t"
            "ldd [%1+8], %%f2\n\t"
            "ldd [%1+16], %%f4\n\t"
            "ldd [%1+24], %%f6\n\t"
            "ldd [%1+32], %%f8\n\t"
            "ldd [%1+40], %%f10\n\t"
//            "ldd [%1+48], %%f12\n\t"
//            "ldd [%1+56], %%f14\n\t"
            "std %%f0, [%2]\n\t"
            "std %%f2, [%2+8]\n\t"
            "std %%f4, [%2+16]\n\t"
            "std %%f6, [%2+24]\n\t"
            "std %%f8, [%2+32]\n\t"
            "std %%f10, [%2+40]\n\t"
//            "std %%f12, [%2+48]\n\t"
//            "std %%f14, [%2+56]\n\t"
            : "=r"(ret_value)     /* output registers*/
            : "r"(src), "r"(dst)      /* input registers*/
            : "%f0", "%f1", "%f2", "%f3", "%f4", "%f5", "%f6", "%f7",       /* clobbered registers*/
              "%f8", "%f9", "%f10", "%f11", "%f12", "%f13", "%f14", "%f15"  /* clobbered registers*/
            );
    // src points to the 2nd cache block
    src += CACHE_LINE_SIZE - hdr_size;
    dst += CACHE_LINE_SIZE - hdr_size;

    int total_chunks = size/(16*CACHE_LINE_SIZE);
        PASS2FLEXUS_MEASURE(i, MEASUREMENT, 21);

    for (k=0; k < total_chunks; k++ ) {
        // check the cl versions
        version_t cur_version_ptr = src;
        //printf("%x - %x - k=%d, size=%d, srcptr=%x, src_begin=%x\n", *(unsigned*)hdr_version_ptr, *(unsigned*)cur_version_ptr, k, size, src, start_src);

        uintptr_t chunk_end = MIN(src + (16*CACHE_LINE_SIZE), start_src+size);
        unsigned delta = chunk_end - src;
        //printf("outer loop: src=%x, end=%x\n", src, chunk_end);
        __asm__ __volatile__(
                "1:"
                //
                "ld [%1+4], %%i0\n\t"
                "ld [%2+4], %%i1\n\t"
                "cmp %%i0, %%i1\n\t"
                "bne 2f\n\t"
                "nop\n\t"
                //
                "add %2, 64, %2\n\t"
                "cmp %2, %3\n\t"
                "bne  1b\n\t" // go to the beginning of the loop
                "nop\n\t"

                "ba 3f\n\t"
                "nop\n\t"
                "2:\n\t" // version check failed
                "rett %%i7 + 8\n\t"
                "mov 0, %%o0\n\t"
                "3:\n\t" // versions are correct
                "sub %2, %4, %2\n\t" // this #!$%^ compiler assigns the same reg for cur_version_ptr and src
                    : "=r"(ret_value)     /* output registers*/
                    : "r"(start_src), "r"(cur_version_ptr), "r"(chunk_end), "r"(delta)      /* input registers*/
                       : "%o0", "%i7", "%l0", "%l1", "%l2", "%i0", "%i1", "%f0", "%f1", "%f2", "%f3", "%f4", "%f5", "%f6", "%f7",       /* clobbered registers*/
                       "%f8", "%f9", "%f10", "%f11", "%f12", "%f13", "%f14", "%f15"  /* clobbered registers*/
                );

        PASS2FLEXUS_MEASURE(i, MEASUREMENT, 22);
        // the rest
        for (p=0; p<8; p++) {
            //printf("inner loop: src=%x, end=%x\n", src, chunk_end);
            if ( (p == 7) && ( k==total_chunks-1) ) {
            //printf("last chunk: src=%x, end=%x\n", src, chunk_end);
                // skip the last cache block from the last chunk
            __asm__ __volatile__ (
                    // we assume version size = 8 bytes
                    "ldd [%1+8], %%f0\n\t"
                    "ldd [%1+16], %%f2\n\t"
                    "ldd [%1+24], %%f4\n\t"
                    "ldd [%1+32], %%f6\n\t"
                    "ldd [%1+40], %%f8\n\t"
                    "ldd [%1+48], %%f10\n\t"
                    "ldd [%1+56], %%f12\n\t"
                    "std %%f0, [%2]\n\t"
                    "std %%f2, [%2+8]\n\t"
                    "std %%f4, [%2+16]\n\t"
                    "std %%f6, [%2+24]\n\t"
                    "std %%f8, [%2+32]\n\t"
                    "std %%f10, [%2+40]\n\t"
                    "std %%f12, [%2+48]\n\t"
                    : "=r"(ret_value)     /* output registers*/
                    : "r"(src), "r"(dst)      /* input registers*/
                       : "%f0", "%f1", "%f2", "%f3", "%f4", "%f5", "%f6", "%f7",       /* clobbered registers*/
                       "%f8", "%f9", "%f10", "%f11", "%f12", "%f13"  /* clobbered registers*/
                           );
                break;
            }

            __asm__ __volatile__ (
                    // we assume version size = 8 bytes
                    "ldd [%1+8], %%f0\n\t"
                    "ldd [%1+16], %%f2\n\t"
                    "ldd [%1+24], %%f4\n\t"
                    "ldd [%1+32], %%f6\n\t"
                    "ldd [%1+40], %%f8\n\t"
                    "ldd [%1+48], %%f10\n\t"
                    "ldd [%1+56], %%f12\n\t"
                    //            "ldd [%1+56], %%f14\n\t"
                    "ldd [%1+72], %%f14\n\t"
                    "ldd [%1+80], %%f16\n\t"
                    "ldd [%1+88], %%f18\n\t"
                    "ldd [%1+96], %%f20\n\t"
                    "ldd [%1+104], %%f22\n\t"
                    "ldd [%1+112], %%f24\n\t"
                    "ldd [%1+120], %%f26\n\t"
                    //            "ldd [%1+56], %%f14\n\t"
                    "std %%f0, [%2]\n\t"
                    "std %%f2, [%2+8]\n\t"
                    "std %%f4, [%2+16]\n\t"
                    "std %%f6, [%2+24]\n\t"
                    "std %%f8, [%2+32]\n\t"
                    "std %%f10, [%2+40]\n\t"
                    "std %%f12, [%2+48]\n\t"
                    //            "std %%f14, [%2+56]\n\t"
                    "std %%f14, [%2+56]\n\t"
                    "std %%f16, [%2+64]\n\t"
                    "std %%f18, [%2+72]\n\t"
                    "std %%f20, [%2+80]\n\t"
                    "std %%f22, [%2+88]\n\t"
                    "std %%f24, [%2+96]\n\t"
                    "std %%f26, [%2+104]\n\t"
                    //            "std %%f14, [%2+56]\n\t"
                    : "=r"(ret_value)     /* output registers*/
                    : "r"(src), "r"(dst)      /* input registers*/
                       : "%f0", "%f1", "%f2", "%f3", "%f4", "%f5", "%f6", "%f7",       /* clobbered registers*/
                       "%f8", "%f9", "%f10", "%f11", "%f12", "%f13", "%f14", "%f15",  /* clobbered registers*/
                   "%f16", "%f17", "%f18", "%f19", "%f20", "%f21", "%f22", "%f23",  /* clobbered registers*/
                   "%f24", "%f25", "%f26", "%f27"  /* clobbered registers*/
                           );

            src += CACHE_LINE_SIZE*2;
            dst += (CACHE_LINE_SIZE - sizeof(version_t))*2;
            //printf("3src=%x\n\n", src);
        }
    }

    PASS2FLEXUS_MEASURE(m, MEASUREMENT, 30);

    // if we are here, all cache line versions match
    //exit(0);
    return 1;
}

__attribute__ ((noinline))
int my_memcopy2(void *dst, void *src, int size, int k) {
    int i=0, p, ret_value;
    PASS2FLEXUS_MEASURE(k, MEASUREMENT, 20);

    // by 1KB
    // each KB: first load first 8 bytes from 16 cache blocks, then the rest
    for (i=0; i<size/(16*CACHE_LINE_SIZE); i++) {
        __asm__ __volatile__ (
                "ldd [%1], %%f0\n\t"
                "ldd [%1+64], %%f2\n\t"
                : "=r"(ret_value)     /* output registers*/
                : "r"(src), "r"(dst)      /* input registers*/
                   : "%f0", "%f1", "%f2", "%f3", "%f4", "%f5", "%f6", "%f7",   /* clobbered registers*/
                   "%f8", "%f9", "%f10", "%f11", "%f12", "%f13", "%f14", "%f15",  /* clobbered registers*/
                   "%f16", "%f17", "%f18", "%f19", "%f20", "%f21", "%f22", "%f23",  /* clobbered registers*/
                   "%f24", "%f25", "%f26", "%f27", "%f28", "%f29", "%f30", "%f31"  /* clobbered registers*/
                       );

        dst+=1024;
        src+=1024;
    }
    PASS2FLEXUS_MEASURE(k, MEASUREMENT, 30);
    return 1;
}

__attribute__ ((noinline))
int my_memcopy4(void *dst, void *src, int size, int k) {
    int i=0, p, ret_value;
    PASS2FLEXUS_MEASURE(k, MEASUREMENT, 20);

    // by 1KB
    // each KB: first load first 8 bytes from 16 cache blocks, then the rest
    for (i=0; i<size/(16*CACHE_LINE_SIZE); i++) {
        __asm__ __volatile__ (
                "ldd [%1], %%f0\n\t"
                "ldd [%1+64], %%f2\n\t"
                "ldd [%1+128], %%f4\n\t"
                "ldd [%1+192], %%f6\n\t"
                : "=r"(ret_value)     /* output registers*/
                : "r"(src), "r"(dst)      /* input registers*/
                   : "%f0", "%f1", "%f2", "%f3", "%f4", "%f5", "%f6", "%f7",   /* clobbered registers*/
                   "%f8", "%f9", "%f10", "%f11", "%f12", "%f13", "%f14", "%f15",  /* clobbered registers*/
                   "%f16", "%f17", "%f18", "%f19", "%f20", "%f21", "%f22", "%f23",  /* clobbered registers*/
                   "%f24", "%f25", "%f26", "%f27", "%f28", "%f29", "%f30", "%f31"  /* clobbered registers*/
                       );

        dst+=1024;
        src+=1024;
    }
    PASS2FLEXUS_MEASURE(k, MEASUREMENT, 30);
    return 1;
}

__attribute__ ((noinline))
int my_memcopy8(void *dst, void *src, int size, int k) {
    int i=0, p, ret_value;
    PASS2FLEXUS_MEASURE(k, MEASUREMENT, 20);

    // by 1KB
    // each KB: first load first 8 bytes from 16 cache blocks, then the rest
    for (i=0; i<size/(16*CACHE_LINE_SIZE); i++) {
        __asm__ __volatile__ (
                "ldd [%1], %%f0\n\t"
                "ldd [%1+64], %%f2\n\t"
                "ldd [%1+128], %%f4\n\t"
                "ldd [%1+192], %%f6\n\t"
                "ldd [%1+256], %%f8\n\t"
                "ldd [%1+320], %%f10\n\t"
                "ldd [%1+384], %%f12\n\t"
                "ldd [%1+448], %%f14\n\t"
                : "=r"(ret_value)     /* output registers*/
                : "r"(src), "r"(dst)      /* input registers*/
                   : "%f0", "%f1", "%f2", "%f3", "%f4", "%f5", "%f6", "%f7",   /* clobbered registers*/
                   "%f8", "%f9", "%f10", "%f11", "%f12", "%f13", "%f14", "%f15",  /* clobbered registers*/
                   "%f16", "%f17", "%f18", "%f19", "%f20", "%f21", "%f22", "%f23",  /* clobbered registers*/
                   "%f24", "%f25", "%f26", "%f27", "%f28", "%f29", "%f30", "%f31"  /* clobbered registers*/
                       );

        dst+=1024;
        src+=1024;
    }
    PASS2FLEXUS_MEASURE(k, MEASUREMENT, 30);
    return 1;
}

__attribute__ ((noinline))
int my_memcopy(void *dst, void *src, int size, int k) {
    int i=0, p, ret_value;
    PASS2FLEXUS_MEASURE(k, MEASUREMENT, 20);

    // by 1KB
    // each KB: first load first 8 bytes from 16 cache blocks, then the rest
    for (i=0; i<size/(16*CACHE_LINE_SIZE); i++) {
        __asm__ __volatile__ (
                //"LOAD_SEQ:"
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
#ifndef ZERO_COPY
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
#endif /* ZERO_COPY */
                : "=r"(ret_value)     /* output registers*/
                : "r"(src), "r"(dst)      /* input registers*/
                   : "%f0", "%f1", "%f2", "%f3", "%f4", "%f5", "%f6", "%f7",   /* clobbered registers*/
                   "%f8", "%f9", "%f10", "%f11", "%f12", "%f13", "%f14", "%f15",  /* clobbered registers*/
                   "%f16", "%f17", "%f18", "%f19", "%f20", "%f21", "%f22", "%f23",  /* clobbered registers*/
                   "%f24", "%f25", "%f26", "%f27", "%f28", "%f29", "%f30", "%f31"  /* clobbered registers*/
                       );

        // by 2 cache blocks
#ifndef ZERO_COPY
        for (p=0; p < 8; p++) {
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
                    : "=r"(ret_value)     /* output registers*/
                    : "r"(src), "r"(dst)      /* input registers*/
                       : "%f0", "%f1", "%f2", "%f3", "%f4", "%f5", "%f6", "%f7",   /* clobbered registers*/
                       "%f8", "%f9", "%f10", "%f11", "%f12", "%f13", "%f14", "%f15",  /* clobbered registers*/
                       "%f16", "%f17", "%f18", "%f19", "%f20", "%f21", "%f22", "%f23",  /* clobbered registers*/
                       "%f24", "%f25", "%f26", "%f27", "%f28", "%f29", "%f30", "%f31"  /* clobbered registers*/
                           );
            dst+=128;
            src+=128;
        }
#else
        dst+=1024;
        src+=1024;
#endif /* ZERO_COPY */
    }
    PASS2FLEXUS_MEASURE(k, MEASUREMENT, 30);
    return 1;
}

/*
static void write_stuff(int sig, siginfo_t *si, void * uc) {
    int i,j,luckyObj;
    uint8_t prevLockVal;
    luckyObj = rand() % num_objects;
    uintptr_t obj_ptr = ctxbuff + luckyObj*data_obj_size;
    data_object_t *my_obj = (data_object_t *)obj_ptr;
    while (my_obj->lock);	//TTS
    do {
        prevLockVal = acquire_lock(&(my_obj->lock));	//Test-and-set
        if (prevLockVal) {
            call_magic_2_64(luckyObj, LOCK_SPINNING, prevLockVal);	//signal the completion of a write
#ifdef MY_DEBUG
            printf("hread %d failed to grab lock of item %d! (lock value = %"PRIu8")\n", p->id, luckyObj, prevLockVal);
            usleep(10);
#endif
        }
    } while (prevLockVal);
#ifdef MEASURE_TS
    call_magic_2_64(luckyObj, CS_START, i);	//signal the beginning of the CS
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
    call_magic_2_64(luckyObj, OBJECT_WRITE, i);	//signal the completion of a write
#ifdef MY_DEBUG
    printf("Thread %d: \tprevious lock value = %" PRIu8 ",\t\
            new lock value = %" PRIu8 ",\t\
            new version = %" PRIu64 "\n", p->id, prevLockVal, my_obj->lock, my_obj->version);
#endif
}

void * par_phase_write(void *arg) {
    parm *p=(parm *)arg;
     
    timer_t timerid;
    struct sigevent sev;
    struct itimerspec its;
    long long freq_nanosecs;
    sigset_t mask;
    struct sigaction sa;

    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = write_stuff;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIG, &sa, NULL) == -1)
        errExit("sigaction");



    sigemptyset(&mask);
    sigaddset(&mask, SIG);
    if (sigprocmask(SIG_SETMASK, &mask, NULL) == -1)
        errExit("sigprocmask");



    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIG;
    sev.sigev_value.sival_ptr = &timerid;
    if (timer_create(CLOCKID, &sev, &timerid) == -1)
        errExit("timer_create");

    printf("timer ID is 0x%lx\n", (long) timerid);



    freq_nanosecs = write_freq;
    its.it_value.tv_sec = freq_nanosecs / 1000000000;
    its.it_value.tv_nsec = freq_nanosecs % 1000000000;
    its.it_interval.tv_sec = its.it_value.tv_sec;
    its.it_interval.tv_nsec = its.it_value.tv_nsec;

    if (timer_settime(timerid, 0, &its, NULL) == -1)
        errExit("timer_settime");
 
    printf("Unblocking signal %d\n", SIG);
    if (sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1)
        errExit("sigprocmask");

    srand(p->id);		//remove this for lots of conflicts :-)
    pthread_barrier_wait (&barrier);
    if (!p->id) call_magic_2_64(1, ALL_SET, 1); //INIT DONE

    while(1) {}


    return NULL;
}
*/

__attribute__ ((noinline))
int update_cl_versions(void *obj, int size, int new_version) {
    int i=0, p, ret_value;
    uintptr_t cl_vers = (uintptr_t)obj;
    cl_vers += 4;
   
    // first 16 cache blocks
    __asm__ __volatile__ (
            //"stw %1, [%2]\n\t" // skip the first cache block
            "stw %1, [%2+64]\n\t"
            "stw %1, [%2+128]\n\t"
            "stw %1, [%2+192]\n\t"
            "stw %1, [%2+256]\n\t"
            "stw %1, [%2+320]\n\t"
            "stw %1, [%2+384]\n\t"
            "stw %1, [%2+448]\n\t"
            "stw %1, [%2+512]\n\t"
            "stw %1, [%2+576]\n\t"
            "stw %1, [%2+640]\n\t"
            "stw %1, [%2+704]\n\t"
            "stw %1, [%2+768]\n\t"
            "stw %1, [%2+832]\n\t"
            "stw %1, [%2+896]\n\t"
            "stw %1, [%2+960]\n\t"
            : "=r"(ret_value)     /* output registers*/
            : "r"(new_version), "r"(cl_vers)      /* input registers*/
                );
        cl_vers+=1024;

    for (i=1; i<size/(16*CACHE_LINE_SIZE); i++) {
        __asm__ __volatile__ (
                "stw %1, [%2]\n\t"
                "stw %1, [%2+64]\n\t"
                "stw %1, [%2+128]\n\t"
                "stw %1, [%2+192]\n\t"
                "stw %1, [%2+256]\n\t"
                "stw %1, [%2+320]\n\t"
                "stw %1, [%2+384]\n\t"
                "stw %1, [%2+448]\n\t"
                "stw %1, [%2+512]\n\t"
                "stw %1, [%2+576]\n\t"
                "stw %1, [%2+640]\n\t"
                "stw %1, [%2+704]\n\t"
                "stw %1, [%2+768]\n\t"
                "stw %1, [%2+832]\n\t"
                "stw %1, [%2+896]\n\t"
                "stw %1, [%2+960]\n\t"
                : "=r"(ret_value)     /* output registers*/
                : "r"(new_version), "r"(cl_vers)      /* input registers*/
                       );
        cl_vers+=1024;
    }
    return 1;
}

void * par_phase_write(void *arg) {
    parm *p=(parm *)arg;
    int i,j,luckyObj;
    uint8_t prevLockVal;

    int z = 235, k;
    srand(p->id);		//remove this for lots of conflicts :-)
    pthread_barrier_wait (&barrier);
    if (!p->id) PASS2FLEXUS_MEASURE_COMMON(1, ALL_SET, 1); //INIT DONE

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
                printf("hread %d failed to grab lock of item %d! (lock value = %"PRIu8")\n", p->id, luckyObj, prevLockVal);
                usleep(10);
#endif
            }
        } while (prevLockVal);
#ifdef MEASURE_TS
        PASS2FLEXUS_MEASURE(luckyObj, CS_START, i);	//signal the beginning of the CS
#endif
        /*uintptr_t data_ptr = obj_ptr + sizeof(obj_hdr_t);
        for (j=0; j<data_obj_size; j++) {
            *(uint8_t *)(data_ptr+j) ^= 1;
        }
        for (j=0; j<data_obj_size; j+=OBJECT_BYTE_STRIDE) 
            *(uint8_t *)(data_ptr+j) = (uint8_t)i;
        */
#ifdef MY_DEBUG
        printf("version = %" PRIu64 ",\t\
                lock = %" PRIu8 ",\t\
                key = %" PRIu32 "\n", my_obj->version, my_obj->lock, my_obj->key);
        printf("OLD cl_version[15] = %d, cl_version[7] = %d\n", *(int*)(obj_ptr+64*15+4), *(int*)(obj_ptr+64*7+4));
#endif
        // FIXME
        my_obj->key ^= 7;  //random operation on object
        my_obj->version++;
        update_cl_versions( (void*)obj_ptr, data_obj_size, (int)(my_obj->version) );
#ifdef MY_DEBUG
        printf("NEW cl_version[15] = %d, cl_version[7] = %d\n", *(int*)(obj_ptr+64*15+4), *(int*)(obj_ptr+64*7+4));
#endif
        my_obj->lock = 0;	//unlock
        PASS2FLEXUS_MEASURE(luckyObj, OBJECT_WRITE, i);	//signal the completion of a write
        
        for(k=0; k<write_freq/2; k++) {
            z = k*z;
        }
        
#ifdef MY_DEBUG
        printf("Thread %d: \tprevious lock value = %" PRIu8 ",\t\
                new lock value = %" PRIu8 ",\t\
                new version = %" PRIu64 "\n", p->id, prevLockVal, my_obj->lock, my_obj->version);
#endif
    }
    PASS2FLEXUS_DBG(0, BENCHMARK_END, 0);	//this threads completed its work and it's exiting

    printf("%d\n", z);
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
    if (!p->id) call_magic_sim_break(1, 1, 1);
    if (!p->id) call_magic_2_64(1, ALL_SET, 1); //INIT DONE
 
    uint8_t *lbuff_slot, *thread_buf_base;
    uint64_t ctx_offset, op_count = 0;
    uint32_t wq_head, cq_tail;
    thread_buf_base = lbuff + thread_buf_size * p->id; //this is the local buffer's base address for this thread

//reader kernel    
int k = 0, z = 1;
    uint8_t success; 
    for (i = 0; i<iters; i++) {

        success = 0;
        PASS2FLEXUS_MEASURE(i, MEASUREMENT, 0);
        //luckyObj = rand() % num_objects;
        lbuff_slot = (uint8_t *)(thread_buf_base + ((op_count * data_obj_size) % thread_buf_size));
        ctx_offset = (op_count%num_objects) * data_obj_size;
        wq_head = wq->head;

        while (!success) {	//read the object again if it's not consistent
        PASS2FLEXUS_MEASURE(i, MEASUREMENT, 1);

#if defined(NO_SW_VERSION_CONTROL) && defined(ZERO_COPY)
            CREATE_WQ_ENTRY(RMC_SABRE, wq->SR, CTX_ID, DST_NID, (uint64_t)lbuff_slot, ctx_offset, payload_cache_blocks, (uint64_t)&(wq->q[wq_head]));
#else
        PASS2FLEXUS_MEASURE(i, MEASUREMENT, 2);
            CREATE_WQ_ENTRY(RMC_READ, wq->SR, CTX_ID, DST_NID, (uint64_t)lbuff_slot, ctx_offset, payload_cache_blocks, (uint64_t)&(wq->q[wq_head]));
#endif
            call_magic_2_64(wq_head, NEWWQENTRY, op_count);
            wq->head =  wq->head + 1;
            if (wq->head >= MAX_NUM_WQ) {
                wq->head = 0;
                wq->SR ^= 1;
            }
            //sync
        PASS2FLEXUS_MEASURE(i, MEASUREMENT, 3);
            cq_tail = cq->tail;
            while(cq->q[cq_tail].SR != cq->SR) {}	//wait for request completion (sync mode)
#if defined(NO_SW_VERSION_CONTROL) && defined(ZERO_COPY)
            success = cq->q[cq_tail].success;
#else
            success = 1; // remote read always succeeds
        PASS2FLEXUS_MEASURE(i, MEASUREMENT, 4);
#endif

#ifndef OLD_DEF
            wq->q[cq->q[cq_tail].tid].valid = 0;	//free WQ entry
#endif

            cq->tail = cq->tail + 1;
            if (cq->tail >= MAX_NUM_WQ) {
                cq->tail = 0;
                cq->SR ^= 1;
            }  
            
            call_magic_2_64(cq_tail, WQENTRYDONE, op_count);
        PASS2FLEXUS_MEASURE(i, MEASUREMENT, 5);
 
            int out = farm_memcopy_asm((void*)app_buff, (void*)lbuff_slot, data_obj_size, i);
        PASS2FLEXUS_MEASURE(i, MEASUREMENT, 35);
            success = out;
            PASS2FLEXUS_MEASURE(i, MEASUREMENT, 40);
            //assert(out==1);

            if (success) {	//No atomicity violation!
                PASS2FLEXUS_DBG(cq_tail, SABRE_SUCCESS, op_count);
            } else {	//Atomicity violation detected
                PASS2FLEXUS_DBG(cq_tail, SABRE_ABORT, op_count);
            }

            op_count++;

        }
        PASS2FLEXUS_MEASURE(i, MEASUREMENT, 40);

    }
    printf("%d", z);
    PASS2FLEXUS_DBG(0, BENCHMARK_END, 0);	//this thread completed its work and it's exiting
    free(wq);
    free(cq);

    return NULL;
}

int main(int argc, char **argv)
{
    int i, retcode, num_threads; 
    iters = ITERS;

    if (!(argc == 7)) {
        fprintf(stdout,"Usage: %s <ctx_buff_size (in KB)> <num_readers> <num_writers> <obj_size> <memcpyID=16, 8, 4, 2> <write freq in ns>\n", argv[0]);
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
    memcpyID = atoi(argv[5]);
    write_freq = atoll(argv[6]);
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
    call_magic_2_64(42, BUFFER_SIZE, 0);//buf_size);
    call_magic_2_64(42, CONTEXT_SIZE, 0);//ctx_size);
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

