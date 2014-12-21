/**
 * Magic calls to Flexus.
 *
 * Copyright (C) EPFL. All rights reserved.
 * @authors daglis
 */

#ifndef MAGIC_IFACE_H
#define MAGIC_IFACE_H

//#ifdef __cplusplus
//extern "C" {
//#endif

// TODO: cleanup obsolete functions

static uint32_t global1;
static uint32_t global2;

static uint32_t  __attribute__ ((noinline)) stupid_function(void){

	uint32_t dummy = 1;
	uint32_t tmp_1 = global1 << 4;
	uint32_t tmp_2 = global2 * 4;
	uint32_t rr = rand();

	if(rr % 37 == 0)
		dummy *= 2;
	else
		dummy++;

	if(tmp_1 > dummy){
		tmp_1 = tmp_2 + (tmp_1*11) + (rr + tmp_2) + (tmp_1 * rr);
		global2 = rr;
	}
	else {
		tmp_1 = 3*tmp_2 + (tmp_2*11) + (rr + tmp_1) + (tmp_2 * tmp_1);
		global1 = rr;
	}

	return tmp_1;
}

static inline __attribute__ ((always_inline))
void  call_magic_0_no_ret_32( uint32_t lba_function){
    __asm__ __volatile__ (
        "mov %0, %%l0\n\t"
        "sethi 42, %%g0\n\t"
		:
        : "r"(lba_function)
		: "%l0"
    );
}

static inline __attribute__ ((always_inline))
void  call_magic_0_no_ret_64( uint64_t lba_function){
    __asm__ __volatile__ (
        "mov %0, %%l0\n\t"
        "sethi 42, %%g0\n\t"
		:
        : "r"(lba_function)
		: "%l0"
    );
}

static inline __attribute__ ((always_inline))
uint32_t  call_magic_0_32( uint32_t lba_function){
    uint32_t  ret_value;

    __asm__ __volatile__ (
        "mov %1, %%l0\n\t"
        "sethi 42, %%g0\n\t"
        "mov %%l0, %0\n\t"  :
        "=r"(ret_value)     :
        "r"(lba_function) :
        "%l0"
    );

    return ret_value;
}

static inline __attribute__ ((always_inline))
uint64_t  call_magic_0_64( uint64_t lba_function){
    uint64_t  ret_value;

    __asm__ __volatile__ (
        "mov %1, %%l0\n\t"
        "sethi 42, %%g0\n\t"
        "mov %%l0, %0\n\t"  :
        "=r"(ret_value)     :
        "r"(lba_function) :
        "%l0"
    );

    return ret_value;
}

static inline __attribute__ ((always_inline))
uint32_t  call_magic_1_32(uint32_t lba_function, uint32_t arg1){
        uint32_t  ret_value;

	__asm__ __volatile__ (
        "mov %1, %%l0\n\t"
        "mov %2, %%l1\n\t"
        "sethi 42, %%g0\n\t"
        "mov %%l0, %0\n\t"
        : "=r"(ret_value)
        : "r"(lba_function), "r"(arg1)
        : "%l0", "%l1"
    );

    return ret_value;
}

static inline __attribute__ ((always_inline))
uint64_t  call_magic_1_64(uint64_t lba_function, uint64_t arg1){
        uint64_t  ret_value;

	__asm__ __volatile__ (
        "mov %1, %%l0\n\t"
        "mov %2, %%l1\n\t"
        "sethi 42, %%g0\n\t"
        "mov %%l0, %0\n\t"
        : "=r"(ret_value)
        : "r"(lba_function), "r"(arg1)
        : "%l0", "%l1"
    );

    return ret_value;
}

static inline __attribute__ ((always_inline))
uint32_t  call_magic_2_32(uint32_t lba_function, uint32_t arg1, uint32_t arg2){
        uint32_t  ret_value;

   __asm__ __volatile__ (
        "mov %1, %%l0\n\t"
        "mov %2, %%l1\n\t"
        "mov %3, %%l2\n\t"
        "sethi 42, %%g0\n\t"
        "mov %%l0, %0\n\t"
        : "=r"(ret_value)     /* output registers*/
        : "r"(lba_function), "r"(arg1), "r"(arg2)      /* input registers*/
        : "%l0", "%l1", "%l2"   /* clobbered registers*/
    );

    return ret_value;
}

static inline __attribute__ ((always_inline))
uint64_t  call_magic_2_64(uint64_t son_function, uint64_t arg1, uint64_t arg2){
        uint64_t  ret_value;

   __asm__ __volatile__ (
        "mov %1, %%l0\n\t"
        "mov %2, %%l1\n\t"
        "mov %3, %%l2\n\t"
        "sethi 42, %%g0\n\t"
        "mov %%l0, %0\n\t"
        : "=r"(ret_value)     /* output registers*/
        : "r"(son_function), "r"(arg1), "r"(arg2)      /* input registers*/
        : "%l0", "%l1", "%l2"   /* clobbered registers*/
    );

    return ret_value;
}

static inline __attribute__ ((always_inline))
uint64_t  call_magic_3_64(uint64_t son_function, uint64_t arg1, uint64_t arg2, uint64_t arg3){
        uint64_t  ret_value;

   __asm__ __volatile__ (
        "mov %1, %%l0\n\t"
        "mov %2, %%l1\n\t"
        "mov %3, %%l2\n\t"
        "mov %4, %%l3\n\t"
        "sethi 42, %%g0\n\t"
        "mov %%l0, %0\n\t"
        : "=r"(ret_value)     /* output registers*/
        : "r"(son_function), "r"(arg1), "r"(arg2), "r"(arg3)      /* input registers*/
        : "%l0", "%l1", "%l2", "%l3"   /* clobbered registers*/
    );

    return ret_value;
}

static inline __attribute__ ((always_inline))
void lbacall_return(void) {
	asm volatile ("restore\n\t");

	do {
		asm volatile ("sethi 100, %g0\n\t");
		asm volatile ("nop\n\t");
		asm volatile ("nop\n\t");
	}
	while (stupid_function());
}

/**************************** GET VALUES FROM SIMULATOR ******************************/
// The following correspondance must hold in breakpoint_tracker.cc and in v9Decoder code,
// for both the trace and timing simulators
//      Functionality      ---->  Magic Instruction Number
// LBA_GET_FUNCTION_ARG0				101
// LBA_GET_FUNCTION_ARG1				102
// LBA_GET_FUNCTION_ARG2				103
// LBA_GET_FUNCTION_RET0				104
// LBA_GET_LG_INDEX						105
// LBA_FLUSH_WRITES						106

static inline __attribute__ ((always_inline))
uint64_t lbacall_get_function_arg0 (void){
	uint64_t ret_value;

	asm volatile (
		"sethi 101, %%g0\n\t"
		"mov %%l0, %0\n\t"  :
		"=r"(ret_value)     :
		:
		"%l0"
		);
	return ret_value;
}

static inline __attribute__ ((always_inline))
uint64_t lbacall_get_function_arg1 (void){
	uint64_t ret_value;

	asm volatile (
		"sethi 102, %%g0\n\t"
		"mov %%l0, %0\n\t"  :
		"=r"(ret_value)     :
		:
		"%l0"
		);
	return ret_value;
}

static inline __attribute__ ((always_inline))
uint64_t lbacall_get_function_arg2 (void){
	uint64_t ret_value;

	asm volatile (
		"sethi 103, %%g0\n\t"
		"mov %%l0, %0\n\t"  :
		"=r"(ret_value)     :
		:
		"%l0"
		);
	return ret_value;
}

static inline __attribute__ ((always_inline))
uint64_t lbacall_get_function_ret0 (void){
	uint64_t ret_value;

	asm volatile (
		"sethi 104, %%g0\n\t"
		"mov %%l0, %0\n\t"  :
		"=r"(ret_value)     :
		:
		"%l0"
		);
	return ret_value;
}

static inline __attribute__ ((always_inline))
uint32_t lbacall_get_lg_index(void){
	uint32_t ret_value;

	asm volatile (
		"sethi 105, %%g0\n\t"
		"mov %%l0, %0\n\t"  :
		"=r"(ret_value)     :
		:
		"%l0"
		);
	return ret_value;
}

static inline __attribute__ ((always_inline))
void lbacall_flush_writes(void){

	asm volatile ("sethi 106, %g0\n\t" );
}
/*
 * Simics::API::conf_object_t * aCpu = Simics::API::SIM_current_processor();
   Simics::API::SIM_write_register( aCpu , Simics::API::SIM_get_register_number ( aCpu , "l0") , (uint64_t) eventPC );
*/

//to be fixed
//static inline uint64_t lbacall_get_funcsys_comment (void (* ptr) , uint64_t size ){
//	return call_magic_2( LBACALL_GET_FUNCSYS_COMMENT, (uint64_t) ptr, (uint64_t) size );
//}

//#ifdef __cplusplus
//} /* closing brace for extern "C" */
//#endif

#endif /* MAGIC_IFACE_H */
