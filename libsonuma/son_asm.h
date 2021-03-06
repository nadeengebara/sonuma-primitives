/**
 * Some more assembly (non-magic!) calls for Flexus.
 * TODO: merge this file with magic_iface.h.
 *
 * Copyright (C) EPFL. All rights reserved.
 * @authors daglis
 */

#ifndef H_SON_ASM
#define H_SON_ASM

static inline __attribute__ ((always_inline))
    void create_wq_entry(uint8_t op, uint8_t SR, uint8_t cid, uint16_t nid,
            uint64_t buf_addr, uint64_t offset, uint64_t length,
            uint64_t wq_entry_addr) {
        __asm__ __volatile__ (
                //form first double word of WQ entry
                "sllx %5, 22, %%l4\n\t"
                "srlx %%l4, 8, %%l4\n\t"    //place buf_addr in the right location
                "sllx %1, 58, %%l0\n\t"        //place op in the right location
                "sllx %2, 57, %%l1\n\t"        //place SR in the right location
                "sllx %3, 10, %%l2\n\t"        //place cid in the right location
                "or %%g0, 1, %%l3\n\t"
                "sllx %%l3, 56, %%l3\n\t"    //place valid bit in the right location

                "or %4, %%l3, %%l3\n\t"        //place nid in the right location
                "or %%l1, %%l2, %%l2\n\t"
                "or %%l3, %%l4, %%l4\n\t"
                "or %%l2, %%l0, %%l2\n\t"
                "or %%l2, %%l4, %%l4\n\t"    //%l4 contains first WQ double word

                //form second double word of WQ entry
                "sllx %6, 24, %%l5\n\t"        //place offset in the right location
                "sethi %%hi(0x00FFFFFF), %%l0\n\t"
                "or %%l0, 0x3FF, %%l0\n\t"
                "and %7, %%l0, %%l6\n\t"     //place length in the right location
                "or %%l6, %%l5, %%l5\n\t"    //%l5 contains second WQ double word

                "stx %%l5, [%8+8]\n\t"    //store second double word (has to be stored first)
                "stx %%l4, [%8]\n\t"            //store first double word

                :    /* No output */
                : "r"(create_wq_entry), "r"(op), "r"(SR), "r"(cid), "r"(nid),
        "r"(buf_addr), "r"(offset), "r"(length), "r"(wq_entry_addr)    /*input registers*/
            : "%l0", "%l1", "%l2", "%l3", "%l4", "%l5", "%l6", "memory" /*clobbered registers*/
                );
    }
#endif /* H_SON_ASM */
