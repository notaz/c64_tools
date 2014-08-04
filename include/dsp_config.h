/* ----
 * ---- file   : dsp_config.h
 * ---- author : Bastian Spiegel <bs@tkscript.de>
 * ---- legal  : (c) 2013 by Bastian Spiegel. 
 * ----          Distributed under terms of the LESSER GNU GENERAL PUBLIC LICENSE (LGPL). See 
 * ----          http://www.gnu.org/licenses/licenses.html#LGPL or COPYING_LGPL for further information.
 * ----
 * ---- info   : Common DSP types and structs. This is part of the "c64_tools" package.
 * ----
 * ---- changed: 06Sep2013, 08Sep2013, 18Sep2013, 03Oct2013, 16Oct2013, 18Oct2013, 19Oct2013
 * ----          21Oct2013, 01Nov2013, 02Nov2013, 08Dec2013, 11Dec2013, 13Dec2013
 * ----
 * ----
 */

#ifndef __C64_TOOLS_DSP_CONFIG_H__
#define __C64_TOOLS_DSP_CONFIG_H__

#include "cplusplus_begin.h"


/* If defined, reset mailbox before sending a message 
 *
 *  (note) this was necessary in early releases of c64_tools but
 *          seems to be obsolete now.
 *
 *  (note) interestingly, having this enabled results in ~3% _lower_ messaging latency (!?!)
 *
 *  (note) after toggling this defined, both GPP+DSP binaries have to be rebuilt
 */
#define DSP_MAILBOX_RESET defined


#define C64_IMAGE_SITE_PATH  "/lib/dsp/"
/*#define C64_IMAGE_SITE_PATH  "/lib/c64_tools/images/"*/


/* (note) in order to change the max. size of the DSP image, the kernel needs to be updated as well
 *         so the CMA will return a memory block at a fixed physical address (dsp_config.reset_vector.phys_addr)
 */
#define DSP_MAX_IMAGE_SIZE  (4u * 1024u * 1024u)


/* Only relevant if static memory configuration is used ("USE_CMA_FOR_POOLS" undefined in "kmod/dev.c") 
 *
 *  The total size must not exceed (32MB - DSP_MAX_IMAGE_SIZE)
 */
#define DSP_STATICMEM_CACHE_NONE_SIZE  ( 6u * 1024u * 1024u)
#define DSP_STATICMEM_CACHE_RW_SIZE    (10u * 1024u * 1024u)
#define DSP_STATICMEM_CACHE_R_SIZE     ( 6u * 1024u * 1024u)
#define DSP_STATICMEM_CACHE_W_SIZE     ( 6u * 1024u * 1024u)


/* L1DSRAM userspace partitioning (24k) */
#define DSP_L1SRAM_SCRATCH_SIZE  (16u * 1024u)

/* Granularity of dynamic L1SRAM allocations. */
#define DSP_L1SRAM_ALLOC_GRANULARITY  (64u)
#define DSP_L1SRAM_ALLOC_GRANULARITY_SHIFT  (6u)

/* Aligned "to-dsp" / "to-gpp" fastcall register size */
#define DSP_FSHM_TOTAL_REG_SIZE  ( ( ((2u * DSP_FSHM_REG_SIZE) + (DSP_L1SRAM_ALLOC_GRANULARITY - 1u)) >> DSP_L1SRAM_ALLOC_GRANULARITY_SHIFT ) << DSP_L1SRAM_ALLOC_GRANULARITY_SHIFT )


/* Maximum number of dynamically allocated L1SRAM blocks (default=31 => 1984 bytes) */
#define DSP_L1SRAM_MAX_DYNAMIC_ALLOCATIONS  ((DSP_L1SRAM_FSHM_SIZE - DSP_L1SRAM_SCRATCH_SIZE - DSP_FSHM_TOTAL_REG_SIZE) / DSP_L1SRAM_ALLOC_GRANULARITY)


/* Granularity of dynamic L2SRAM allocations. */
#define DSP_L2SRAM_ALLOC_GRANULARITY  (64u)
#define DSP_L2SRAM_ALLOC_GRANULARITY_SHIFT  (6u)

/* Maximum number of dynamically allocated L2SRAM blocks (default=1536 => 96 KBytes) */
#define DSP_L2SRAM_MAX_DYNAMIC_ALLOCATIONS  (DSP_L2SRAM_FSHM_SIZE >> DSP_L2SRAM_ALLOC_GRANULARITY_SHIFT)

 

#define DSP_RESET_VECTOR_CODESIZE  (0x00000080u)  // size of RESET_VECTOR memory region used for boot code
                                                  //  (actually only 32 bytes are used)
#define DSP_RESET_VECTOR_MINSIZE   (0x00001000u)  // minimum size of RESET_VECTOR memory region

////#define DSP_RESET_VECTOR_MSGSIZE   (DSP_RESET_VECTOR_MINSIZE - DSP_RESET_VECTOR_CODESIZE)


#define DSP_PAGESIZE  (4096u)  // (todo) use sysconf(_SC_PAGE_SIZE) [linux]
#define DSP_PAGESHIFT (12u)

#define DSP_PAGESIZE_ROUND(a) ( ( ((a)+(DSP_PAGESIZE-1)) >> DSP_PAGESHIFT ) << DSP_PAGESHIFT )

#define DSP_PAGESIZE_ALIGN(a) ( ( (a) >> DSP_PAGESHIFT ) << DSP_PAGESHIFT )


#define DSP_HUGETLB_PAGESIZE  (2u * 1024u * 1024u)
#define DSP_HUGETLB_PAGESHIFT (21u)
#define DSP_HUGETLB_MAXPAGES  (128u)


#define DSP_FASTCALL_TIMEOUT  (0x02000000u)


/* printf() ringbuffer */
#define DSP_LOGBUF_BASE_GPP  (0x86002000u)  /* must match MLB_LOGBUF memory in link_core.cmd */
#define DSP_LOGBUF_SIZE      (16u * 1024u)

/* Maximum size of a single ringbuffer log entry */
#define DSP_LOGBUF_LINE_SIZE (512u)


typedef struct {
   dsp_mem_region_t reset_vector;  // contains just a jmp to boot.o64P in .sysinit section (code generated by GPP)
                                   //  must be aligned to 4k page boundary
   dsp_mem_region_t ram;           // DSP image code/data and DSP-side heap
} dsp_config_t;


// (note) config MUST match DSPImage memory config (see <prj>.tci) or hell might break loose
// (note) see "dsp_config.c"
S_EXTERN dsp_config_t dsp_config;


#include "cplusplus_end.h"

#endif /* __C64_TOOLS_DSP_CONFIG_H__ */