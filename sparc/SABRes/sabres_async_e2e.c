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



///////////////////////////////////////////////////////////////////////
////////////////////////CRC stuff//////////////////////////////////////
static const uint64_t crc64_tab[256] = {
    0x0000000000000000ULL, 0x42F0E1EBA9EA3693ULL, 0x85E1C3D753D46D26ULL,
    0xC711223CFA3E5BB5ULL, 0x493366450E42ECDFULL, 0x0BC387AEA7A8DA4CULL,
    0xCCD2A5925D9681F9ULL, 0x8E224479F47CB76AULL, 0x9266CC8A1C85D9BEULL,
    0xD0962D61B56FEF2DULL, 0x17870F5D4F51B498ULL, 0x5577EEB6E6BB820BULL,
    0xDB55AACF12C73561ULL, 0x99A54B24BB2D03F2ULL, 0x5EB4691841135847ULL,
    0x1C4488F3E8F96ED4ULL, 0x663D78FF90E185EFULL, 0x24CD9914390BB37CULL,
    0xE3DCBB28C335E8C9ULL, 0xA12C5AC36ADFDE5AULL, 0x2F0E1EBA9EA36930ULL,
    0x6DFEFF5137495FA3ULL, 0xAAEFDD6DCD770416ULL, 0xE81F3C86649D3285ULL,
    0xF45BB4758C645C51ULL, 0xB6AB559E258E6AC2ULL, 0x71BA77A2DFB03177ULL,
    0x334A9649765A07E4ULL, 0xBD68D2308226B08EULL, 0xFF9833DB2BCC861DULL,
    0x388911E7D1F2DDA8ULL, 0x7A79F00C7818EB3BULL, 0xCC7AF1FF21C30BDEULL,
    0x8E8A101488293D4DULL, 0x499B3228721766F8ULL, 0x0B6BD3C3DBFD506BULL,
    0x854997BA2F81E701ULL, 0xC7B97651866BD192ULL, 0x00A8546D7C558A27ULL,
    0x4258B586D5BFBCB4ULL, 0x5E1C3D753D46D260ULL, 0x1CECDC9E94ACE4F3ULL,
    0xDBFDFEA26E92BF46ULL, 0x990D1F49C77889D5ULL, 0x172F5B3033043EBFULL,
    0x55DFBADB9AEE082CULL, 0x92CE98E760D05399ULL, 0xD03E790CC93A650AULL,
    0xAA478900B1228E31ULL, 0xE8B768EB18C8B8A2ULL, 0x2FA64AD7E2F6E317ULL,
    0x6D56AB3C4B1CD584ULL, 0xE374EF45BF6062EEULL, 0xA1840EAE168A547DULL,
    0x66952C92ECB40FC8ULL, 0x2465CD79455E395BULL, 0x3821458AADA7578FULL,
    0x7AD1A461044D611CULL, 0xBDC0865DFE733AA9ULL, 0xFF3067B657990C3AULL,
    0x711223CFA3E5BB50ULL, 0x33E2C2240A0F8DC3ULL, 0xF4F3E018F031D676ULL,
    0xB60301F359DBE0E5ULL, 0xDA050215EA6C212FULL, 0x98F5E3FE438617BCULL,
    0x5FE4C1C2B9B84C09ULL, 0x1D14202910527A9AULL, 0x93366450E42ECDF0ULL,
    0xD1C685BB4DC4FB63ULL, 0x16D7A787B7FAA0D6ULL, 0x5427466C1E109645ULL,
    0x4863CE9FF6E9F891ULL, 0x0A932F745F03CE02ULL, 0xCD820D48A53D95B7ULL,
    0x8F72ECA30CD7A324ULL, 0x0150A8DAF8AB144EULL, 0x43A04931514122DDULL,
    0x84B16B0DAB7F7968ULL, 0xC6418AE602954FFBULL, 0xBC387AEA7A8DA4C0ULL,
    0xFEC89B01D3679253ULL, 0x39D9B93D2959C9E6ULL, 0x7B2958D680B3FF75ULL,
    0xF50B1CAF74CF481FULL, 0xB7FBFD44DD257E8CULL, 0x70EADF78271B2539ULL,
    0x321A3E938EF113AAULL, 0x2E5EB66066087D7EULL, 0x6CAE578BCFE24BEDULL,
    0xABBF75B735DC1058ULL, 0xE94F945C9C3626CBULL, 0x676DD025684A91A1ULL,
    0x259D31CEC1A0A732ULL, 0xE28C13F23B9EFC87ULL, 0xA07CF2199274CA14ULL,
    0x167FF3EACBAF2AF1ULL, 0x548F120162451C62ULL, 0x939E303D987B47D7ULL,
    0xD16ED1D631917144ULL, 0x5F4C95AFC5EDC62EULL, 0x1DBC74446C07F0BDULL,
    0xDAAD56789639AB08ULL, 0x985DB7933FD39D9BULL, 0x84193F60D72AF34FULL,
    0xC6E9DE8B7EC0C5DCULL, 0x01F8FCB784FE9E69ULL, 0x43081D5C2D14A8FAULL,
    0xCD2A5925D9681F90ULL, 0x8FDAB8CE70822903ULL, 0x48CB9AF28ABC72B6ULL,
    0x0A3B7B1923564425ULL, 0x70428B155B4EAF1EULL, 0x32B26AFEF2A4998DULL,
    0xF5A348C2089AC238ULL, 0xB753A929A170F4ABULL, 0x3971ED50550C43C1ULL,
    0x7B810CBBFCE67552ULL, 0xBC902E8706D82EE7ULL, 0xFE60CF6CAF321874ULL,
    0xE224479F47CB76A0ULL, 0xA0D4A674EE214033ULL, 0x67C58448141F1B86ULL,
    0x253565A3BDF52D15ULL, 0xAB1721DA49899A7FULL, 0xE9E7C031E063ACECULL,
    0x2EF6E20D1A5DF759ULL, 0x6C0603E6B3B7C1CAULL, 0xF6FAE5C07D3274CDULL,
    0xB40A042BD4D8425EULL, 0x731B26172EE619EBULL, 0x31EBC7FC870C2F78ULL,
    0xBFC9838573709812ULL, 0xFD39626EDA9AAE81ULL, 0x3A28405220A4F534ULL,
    0x78D8A1B9894EC3A7ULL, 0x649C294A61B7AD73ULL, 0x266CC8A1C85D9BE0ULL,
    0xE17DEA9D3263C055ULL, 0xA38D0B769B89F6C6ULL, 0x2DAF4F0F6FF541ACULL,
    0x6F5FAEE4C61F773FULL, 0xA84E8CD83C212C8AULL, 0xEABE6D3395CB1A19ULL,
    0x90C79D3FEDD3F122ULL, 0xD2377CD44439C7B1ULL, 0x15265EE8BE079C04ULL,
    0x57D6BF0317EDAA97ULL, 0xD9F4FB7AE3911DFDULL, 0x9B041A914A7B2B6EULL,
    0x5C1538ADB04570DBULL, 0x1EE5D94619AF4648ULL, 0x02A151B5F156289CULL,
    0x4051B05E58BC1E0FULL, 0x87409262A28245BAULL, 0xC5B073890B687329ULL,
    0x4B9237F0FF14C443ULL, 0x0962D61B56FEF2D0ULL, 0xCE73F427ACC0A965ULL,
    0x8C8315CC052A9FF6ULL, 0x3A80143F5CF17F13ULL, 0x7870F5D4F51B4980ULL,
    0xBF61D7E80F251235ULL, 0xFD913603A6CF24A6ULL, 0x73B3727A52B393CCULL,
    0x31439391FB59A55FULL, 0xF652B1AD0167FEEAULL, 0xB4A25046A88DC879ULL,
    0xA8E6D8B54074A6ADULL, 0xEA16395EE99E903EULL, 0x2D071B6213A0CB8BULL,
    0x6FF7FA89BA4AFD18ULL, 0xE1D5BEF04E364A72ULL, 0xA3255F1BE7DC7CE1ULL,
    0x64347D271DE22754ULL, 0x26C49CCCB40811C7ULL, 0x5CBD6CC0CC10FAFCULL,
    0x1E4D8D2B65FACC6FULL, 0xD95CAF179FC497DAULL, 0x9BAC4EFC362EA149ULL,
    0x158E0A85C2521623ULL, 0x577EEB6E6BB820B0ULL, 0x906FC95291867B05ULL,
    0xD29F28B9386C4D96ULL, 0xCEDBA04AD0952342ULL, 0x8C2B41A1797F15D1ULL,
    0x4B3A639D83414E64ULL, 0x09CA82762AAB78F7ULL, 0x87E8C60FDED7CF9DULL,
    0xC51827E4773DF90EULL, 0x020905D88D03A2BBULL, 0x40F9E43324E99428ULL,
    0x2CFFE7D5975E55E2ULL, 0x6E0F063E3EB46371ULL, 0xA91E2402C48A38C4ULL,
    0xEBEEC5E96D600E57ULL, 0x65CC8190991CB93DULL, 0x273C607B30F68FAEULL,
    0xE02D4247CAC8D41BULL, 0xA2DDA3AC6322E288ULL, 0xBE992B5F8BDB8C5CULL,
    0xFC69CAB42231BACFULL, 0x3B78E888D80FE17AULL, 0x7988096371E5D7E9ULL,
    0xF7AA4D1A85996083ULL, 0xB55AACF12C735610ULL, 0x724B8ECDD64D0DA5ULL,
    0x30BB6F267FA73B36ULL, 0x4AC29F2A07BFD00DULL, 0x08327EC1AE55E69EULL,
    0xCF235CFD546BBD2BULL, 0x8DD3BD16FD818BB8ULL, 0x03F1F96F09FD3CD2ULL,
    0x41011884A0170A41ULL, 0x86103AB85A2951F4ULL, 0xC4E0DB53F3C36767ULL,
    0xD8A453A01B3A09B3ULL, 0x9A54B24BB2D03F20ULL, 0x5D45907748EE6495ULL,
    0x1FB5719CE1045206ULL, 0x919735E51578E56CULL, 0xD367D40EBC92D3FFULL,
    0x1476F63246AC884AULL, 0x568617D9EF46BED9ULL, 0xE085162AB69D5E3CULL,
    0xA275F7C11F7768AFULL, 0x6564D5FDE549331AULL, 0x279434164CA30589ULL,
    0xA9B6706FB8DFB2E3ULL, 0xEB46918411358470ULL, 0x2C57B3B8EB0BDFC5ULL,
    0x6EA7525342E1E956ULL, 0x72E3DAA0AA188782ULL, 0x30133B4B03F2B111ULL,
    0xF7021977F9CCEAA4ULL, 0xB5F2F89C5026DC37ULL, 0x3BD0BCE5A45A6B5DULL,
    0x79205D0E0DB05DCEULL, 0xBE317F32F78E067BULL, 0xFCC19ED95E6430E8ULL,
    0x86B86ED5267CDBD3ULL, 0xC4488F3E8F96ED40ULL, 0x0359AD0275A8B6F5ULL,
    0x41A94CE9DC428066ULL, 0xCF8B0890283E370CULL, 0x8D7BE97B81D4019FULL,
    0x4A6ACB477BEA5A2AULL, 0x089A2AACD2006CB9ULL, 0x14DEA25F3AF9026DULL,
    0x562E43B4931334FEULL, 0x913F6188692D6F4BULL, 0xD3CF8063C0C759D8ULL,
    0x5DEDC41A34BBEEB2ULL, 0x1F1D25F19D51D821ULL, 0xD80C07CD676F8394ULL,
    0x9AFCE626CE85B507ULL
};

/*
 *  * This a generic crc64() function, it takes seed as an argument,
 *   * and does __not__ xor at the end. Then individual users can do
 *    * whatever they need.
 *     */
uint64_t crc64(uint64_t seed, const unsigned char *data, size_t len, int m)
{
    uint64_t crc = seed;

    PASS2FLEXUS_MEASURE(m, MEASUREMENT, 20);
    while (len--) {
        int i = ((int) (crc >> 56) ^ *data++) & 0xFF;
        crc = crc64_tab[i] ^ (crc << 8);
    }
    PASS2FLEXUS_MEASURE(m, MEASUREMENT, 30);

    return crc;
}


///////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////

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
    int z = 1, z_temp = 5;
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

#ifdef FARM_VS
                  int out = farm_memcopy_asm((void*)app_buff, (void*)lbuff_slot, data_obj_size, op_count_completed);
#endif
#ifdef CRC_VS
            z_temp = crc64(12345678, (void*)lbuff_slot, data_obj_size, i);
            z += z_temp;
#endif

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
    printf("%d", z);
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

#ifdef FARM_VS
    printf("=======\nRunning with FARM-like versions check\n======\n");
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
    fprintf(stdout,"Init done! Allocated %d objects of %lu bytes (pure object data = %d, total size = %lu bytes).\nLocal buffer size = %lu Bytes.\nWill now allocate per-thread QPs and run with %d reader threads (ASYNC) and %d writer threads.\nEach thread will execute %d ops (reads or writes).\nObject strided access: %d Bytes\n", 
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

