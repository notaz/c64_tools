/* ----
 * ---- file   : mlb.c
 * ---- author : Bastian Spiegel <bs@tkscript.de>
 * ---- legal  : (c) 2013 by Bastian Spiegel.
 * ----          Distributed under terms of the MIT LICENSE (MIT).
 * ----
 * ---- Permission is hereby granted, free of charge, to any person obtaining a copy
 * ---- of this software and associated documentation files (the "Software"), to deal
 * ---- in the Software without restriction, including without limitation the rights
 * ---- to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * ---- copies of the Software, and to permit persons to whom the Software is
 * ---- furnished to do so, subject to the following conditions:
 * ----
 * ---- The above copyright notice and this permission notice shall be included in
 * ---- all copies or substantial portions of the Software.
 * ----
 * ---- THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * ---- IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * ---- FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * ---- AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * ---- LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * ---- OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * ---- THE SOFTWARE.
 * ----
 * ---- info   : Mailbox handler / component dispatcher. This is part of the "c64_tools" package.
 * ----
 * ---- changed: 10Sep2013, 11Sep2013, 12Sep2013, 13Sep2013, 14Sep2013, 15Sep2013, 17Sep2013
 * ----          19Sep2013, 20Sep2013, 27Sep2013, 02Oct2013, 03Oct2013, 04Oct2013, 05Oct2013
 * ----          13Oct2013, 20Oct2013, 21Oct2013, 23Oct2013, 24Nov2013, 11Dec2013, 13Dec2013
 * ----          15Dec2013, 16Jan2014
 * ----
 * ----
 */

#define ENABLE_COMPONENT_DISPATCHER defined

#define ENABLE_IPC_DEBUG defined

#define FASTCALL_TIMEOUT (0x80000000u)


#include <stdio.h>
#include <string.h>  /* for memset() */
#include <stdarg.h>

#include <std.h>
#include <tsk.h>
#include <hwi.h>
#include <bcache.h>
#include <c64.h>

#include "../../../include/types.h"
#include "../../../include/arch/dm3730/dm3730.h"
#include "../../../include/dsp_common.h"
#include "../../../include/dsp_config.h"

#include "../include/int.h"
#include "../include/com.h"
#include "../include/mlb.h"
#include "../include/dma.h"
#include "../include/syscalls.h"
#include "../../components/core/core.h"


#pragma DATA_ALIGN(mlb_sram,  32);
#pragma DATA_SECTION(mlb_sram,  ".sec_mlb_sram");
volatile unsigned char mlb_sram[IVA2_L1DSRAM_SIZE]; /* all of it */


#define IPC_REG(off)  (*(volatile sU32*)(&mlb_sram[(off)]))

/*
 * it is important to access the L1DSRAM via the compiler "mlb_sram" variable.
 *
 * Access via (*(volatile sU32*)(L1DSRAMADDR)) freezes the DSP (wth!?!)
 */

#include "../include/syscalls.c"


/* --------------------------------------------------------------------------- vars */
static TSK_Attrs mlb_task_attrs;
//static TSK_Handle mlb_task_handle;

//static SEM_Handle mlb_sem_handle;

static volatile sUI num_irqs;
static volatile sUI num_msg_recvd;

static volatile sU32 mlb_inbox[4];

static dsp_component_t components[MLB_MAX_COMPONENTS];
static sUI num_core_components;

#include "../../linker_scripts/overlay_sections.c"


/* (note) log lines are separated by a single ASCIIZ
 * (note) a log line that starts with ASCIIZ marks the end of the buffer
 */
char *mlb_logbuf;
volatile sU32 *mlb_logbuf_offset_writep;


/* --------------------------------------------------------------------------- loc_component_find_by_name() */
dsp_component_t *loc_component_find_by_name(const char *_name, sBool _bIncRef) {
   sUI i;
   dsp_component_t *ret = components;

   for(i=0; i< MLB_MAX_COMPONENTS; i++)
   {
      if( (0 == i) || (0 != ret->component_id) )
      {
         if(0 == strncmp(ret->name, _name, MLB_MAX_COMPONENT_NAME_LENGTH))
         {
            if(_bIncRef)
            {
               ret->ref_count++;

               BCACHE_wbInvAll();
            }

            /* Found it */
            break;
         }
      }
      
      /* Try next component */
      ret++;
   }

   if(MLB_MAX_COMPONENTS == i)
   {
      ret = NULL;
   }

   return ret;
}


/* --------------------------------------------------------------------------- loc_component_find_by_id() */
dsp_component_t *loc_component_find_by_id(dsp_component_id_t _id) {
   dsp_component_t *ret = components;

   if(_id < MLB_MAX_COMPONENTS)
   {
      ret += _id;

      if(NULL == ret->fxns.exec)
      {
         /* Unused component */
         ret = NULL;
      }
      else
      {
         ////mlb_debug_usr(3, 0x900d);
      }
   }
   else
   {
      /* Failed: component not found */

      ret = NULL;
   }

   return ret;
}


/* --------------------------------------------------------------------------- loc_mlb_overlay_unregister() */
static sU32 loc_mlb_overlay_unregister(sU32 _sectionIdx) {
   sU32 ret;

   if(_sectionIdx < MLB_COM_NUM_SECTION_INDICES)
   {
      dsp_component_t *comp = components + MLB_MAX_CORE_COMPONENTS;
      sUI i;
      
      /* (note) could use a 1:1 mapping between sectionIdx..*/
      for(i=0; i< (MLB_MAX_OVERLAY_COMPONENTS); i++)
      {
         if(0 != comp->component_id)
         {
            if(_sectionIdx == comp->overlay_section_idx)
            {
               comp->ref_count = comp->ref_count - 1;

               if(0 == comp->ref_count)
               {
                  if(NULL != comp->fxns.exit)
                  {
                     comp->fxns.exit();
                  }
                  
                  /* Mark as unused */
                  comp->component_id = 0;
                  comp->fxns.exec    = NULL;

               }

               BCACHE_wbInvAll();
            }
         }
         
         /* Next component */
         comp++;
      }

      ret = CORE_ERR_OK;
   }
   else
   {
      /* Failed: section index out of bounds */
      ret = CORE_ERR_ILLSECIDX;
   }

   return ret;
}


/* --------------------------------------------------------------------------- loc_mlb_overlay_register() */
static sU32 loc_mlb_overlay_register(sU32 _sectionIdx, sU32 *_retMainComponentId) {
   sU32 ret;

   if(_sectionIdx < MLB_COM_NUM_SECTION_INDICES)
   {
      dsp_component_t *comp = components + MLB_MAX_CORE_COMPONENTS;
      sUI i;

      BCACHE_invL1pAll();
      
      for(i=0; i< (MLB_MAX_OVERLAY_COMPONENTS); i++)
      {
         if(0 == comp->component_id)
         {
            /* Found unused component */
            break;
         }

         /* Try next component */
         comp++;
      }

      if(i != MLB_MAX_OVERLAY_COMPONENTS)
      {
         dsp_component_t *srcComp = (dsp_component_t *) overlay_sections[_sectionIdx].phys_addr;
         sBool bInit = S_TRUE;

         *comp = *srcComp;

         comp->component_id        = (dsp_component_id_t) (MLB_MAX_CORE_COMPONENTS + i);
         comp->overlay_section_idx = _sectionIdx;
         comp->ref_count           = 1;

         /////mlb_debug_usr(1, 0x900d);

         if(NULL != comp->fxns.init)
         {
            /* (note) the init() function may register additional components */
            bInit = comp->fxns.init();
         }

         if(bInit)
         {
            *_retMainComponentId = comp->component_id;

            /* Succeeded */
            ret = CORE_ERR_OK;

            ////mlb_debug_usr(2, 0x900d);
         }
         else
         {
            /* Put back into the pool of free components */
            comp->component_id = 0;
            comp->fxns.exec    = NULL;

            /* Unregister any components the init() function may have added */
            (void)loc_mlb_overlay_unregister(_sectionIdx);

            /* Failed to init. component */
            ret = CORE_ERR_COM_INIT;
         }

         BCACHE_wbInvAll();
      }
      else
      {
         /* Failed: all components in use */
         ret = CORE_ERR_COM_ALLUSED;
      }

   }
   else
   {
      /* Failed: section index out of bounds */
      ret = CORE_ERR_ILLSECIDX;
   }

   return ret;
}


/* --------------------------------------------------------------------------- mlb_syscall_component_register() */
sBool mlb_syscall_component_register(dsp_component_t *_component, sU16 _sectionIdx) {
   sU32 ret;

   if(_sectionIdx < MLB_COM_NUM_SECTION_INDICES)
   {
      if(NULL != _component)
      {
         if(NULL != _component->fxns.exec)
         {
            dsp_component_t *comp = components + MLB_MAX_CORE_COMPONENTS;
            sUI i;
            
            for(i=0; i< (MLB_MAX_OVERLAY_COMPONENTS); i++)
            {
               if(0 == comp->component_id)
               {
                  /* Found unused component */
                  break;
               }
               
               /* Try next component */
               comp++;
            }
            
            if(i != MLB_MAX_OVERLAY_COMPONENTS)
            {
               sBool bInit = S_TRUE;
               
               memcpy(comp, _component, sizeof(dsp_component_t));
               
               comp->component_id        = (dsp_component_id_t) (MLB_MAX_CORE_COMPONENTS + i);
               comp->overlay_section_idx = _sectionIdx;
               comp->ref_count           = 1;
               
               if(NULL != comp->fxns.init)
               {
                  /* (note) the init() function MUST NOT register additional components */
                  bInit = comp->fxns.init();
               }
               
               if(bInit)
               {
                  /* Succeeded */
                  ret = CORE_ERR_OK;
               }
               else
               {
                  comp->component_id = 0;
                  
                  /* Failed to init. component */
                  ret = CORE_ERR_COM_INIT;
               }

               BCACHE_wbInvAll();
            }
            else
            {
               /* Failed: all components in use */
               ret = CORE_ERR_COM_ALLUSED;
            }
         }
         else
         {
            /* Failed: no exec() function */
            ret = CORE_ERR_NOEXEC;
         }
      }
      else
      {
         /* Failed: _component is NULL */
         ret = CORE_ERR_NULLPTR;
      }
   }
   else
   {
      /* Failed: section index out of bounds */
      ret = CORE_ERR_ILLSECIDX;
   }

   return ret;
}


/* --------------------------------------------------------------------------- mlb_version() */
sU32 mlb_version(void) {
   return MLB_VERSION;
}


/* --------------------------------------------------------------------------- loc_mlb_exec() */
static sU32 loc_mlb_exec(dsp_component_cmd_t _cmd,
                         sU32  _arg1, sU32  _arg2,
                         sU32 *_ret1, sU32 *_ret2
                         ) {
  sU32 ret;

  switch(_cmd)
  {
     default:
        /* Failed: illegal command id */
        ret = CORE_ERR_ILLCMD;
        break;
        
     case CORE_CMD_COM_OVERLAY_INITIATE:
        mlb_fastcall_initiate();

        BCACHE_wbInvAll();
        
        /* Succeeded */
        ret = 0;
        break;

     case CORE_CMD_COM_FIND:
     {
        /* **deprecated** */
        dsp_component_t *comp = loc_component_find_by_name((const char*)_arg1, (sBool)_arg2 /* bIncRef */);

        if(NULL != comp)
        {
           /* Succeeded */
           *_ret1 = CORE_ERR_OK;
           *_ret2 = (comp->component_id) | ( ((sU32)comp->overlay_section_idx) << 16);
        }
        else
        {
           /* Failed: component not found */
           *_ret1 = ret = CORE_ERR_COM_NOT_FOUND;
           *_ret2 = 0;
        }

        /* Succeeded */
        ret = 0;
        break;
     }

     case CORE_CMD_COM_EMERGENCY_UNLOAD:
        *_ret1 = loc_mlb_overlay_unregister(_arg1);

        /* Succeeded */
        ret = 0;
        break;
  }
  
  return ret;
}


/* --------------------------------------------------------------------------- loc_mlb_exec_fc() */
static sU32 loc_mlb_exec_fc(dsp_component_cmd_t _cmd,
                            sU32  _arg1, sU32  _arg2,
                            sU32 *_ret1, sU32 *_ret2
                            ) {
   sU32 ret = 0;

   switch(_cmd)
   {
      default:
         /* Failed: illegal command id */
         ret = CORE_ERR_ILLFCCMD;
         break;

      case DSP_COMPONENT_FC_CMD_DISPATCH(CORE_FC_CMD_COM_OVERLAY_CACHE_INV):

         /* Called after GPP side has updated component code section */
         BCACHE_wbInvAll();

         /* Succeeded */
         *_ret1 = CORE_ERR_OK;
         break;

      case DSP_COMPONENT_FC_CMD_DISPATCH(CORE_FC_CMD_COM_OVERLAY_REGISTER):

         /* Called after GPP side has updated component code section, and caches
          * have been invalidated on the DSP side.
          */
         *_ret1 = loc_mlb_overlay_register(_arg1, _ret2);
         break;

      case DSP_COMPONENT_FC_CMD_DISPATCH(CORE_FC_CMD_COM_OVERLAY_UNREGISTER):

         /* Called when an overlay and its component(s) need(s) to be unloaded.
          */
         *_ret1 = loc_mlb_overlay_unregister(_arg1);
         break;

      case DSP_COMPONENT_FC_CMD_DISPATCH(CORE_FC_CMD_COM_OVERLAY_FIND):
      {
         /* Find overlay/component by name */
         
         dsp_component_t *comp = loc_component_find_by_name((const char*)_arg1, (sBool)_arg2 /* bIncRef */);
         
         if(NULL != comp)
         {
            /* Succeeded */
            *_ret1 = CORE_ERR_OK;
            *_ret2 = (comp->component_id) | ( ((sU32)comp->overlay_section_idx) << 16);
         }
         else
         {
            /* Failed: component not found */
            *_ret1 = ret = CORE_ERR_COM_NOT_FOUND;
            *_ret2 = 0;
         }

         /* Succeeded */
         ret = 0;
         break;
      }
      
   }

   return ret;
}


/* --------------------------------------------------------------------------- mlb component */
static dsp_component_t component_mlb = {

   /* fxns: */
   {
      NULL, /* init */
      &loc_mlb_exec,
      &loc_mlb_exec_fc,
      NULL  /* exit */
   },

   COMPONENT_NAME_CORE,

   0, /* component_id */

};


/* --------------------------------------------------------------------------- loc_debug() */
static void loc_debug(sU32 _val) {

   IPC_REG(DSP_IPC_DEBUG_INTERNAL_OFF) = _val;
}


/* --------------------------------------------------------------------------- loc_bzero() */
static void loc_bzero(void *_addr, size_t _numBytes) {
   /* (todo) optimize */
   memset(_addr, 0, _numBytes);
}


/* --------------------------------------------------------------------------- loc_logbuf_addline() */
static int loc_logbuf_addline(const char *_str, sUI _numCharsIncASCIIZ) {
   int ret;

   if(_numCharsIncASCIIZ < DSP_LOGBUF_SIZE)
   {
      if( (*mlb_logbuf_offset_writep + _numCharsIncASCIIZ) > DSP_LOGBUF_SIZE )
      {
         /* Copy until end of ringbuf */
         memcpy(mlb_logbuf + *mlb_logbuf_offset_writep,
                _str,
                (DSP_LOGBUF_SIZE - *mlb_logbuf_offset_writep)
                );

         /* Copy remaining chars to start of ringbuf */
         memcpy(mlb_logbuf,
                _str + (DSP_LOGBUF_SIZE - *mlb_logbuf_offset_writep),
                _numCharsIncASCIIZ - (DSP_LOGBUF_SIZE - *mlb_logbuf_offset_writep)
                );
      }
      else
      {
         /* No buffer wrap-around */
         memcpy(mlb_logbuf + *mlb_logbuf_offset_writep,
                _str,
                _numCharsIncASCIIZ
                );
      }

      (*mlb_logbuf_offset_writep) += _numCharsIncASCIIZ;

      if(*mlb_logbuf_offset_writep >= DSP_LOGBUF_SIZE)
      {
         (*mlb_logbuf_offset_writep) -= DSP_LOGBUF_SIZE;
      }

      /* Set "end of ringbuf" flag */
      mlb_logbuf[*mlb_logbuf_offset_writep] = 0;

      /* Succeeded */
      ret = 0;
   }
   else
   {
      /* Failed: string exceeds log buffer size */
      ret = -1;
   }

   BCACHE_wbInv((Ptr)mlb_logbuf, DSP_LOGBUF_SIZE + 4, 1 /* wait */);

   return ret;
}


/* --------------------------------------------------------------------------- loc_vprintf() */
static int loc_puts(const char *_str) {
   int ret;
   int len = strlen(_str);

   if(len > 0)
   {
      ret = loc_logbuf_addline(_str, len + 1);
   }
   else
   {
      /* Warning: trying to print empty string (ignore) */
      ret = 0;
   }

   return ret;
}


/* --------------------------------------------------------------------------- loc_printf() */
static int loc_printf(const char *_fmt, ...) {
   char buf[DSP_LOGBUF_LINE_SIZE];
   int ret;

   va_list l;

   va_start(l, _fmt);

   ret = vsnprintf(buf, DSP_LOGBUF_LINE_SIZE, _fmt, l);

   if(ret > 0)
   {
      ret = loc_puts(buf);
   }

   va_end(l);

   return ret;
}


/* --------------------------------------------------------------------------- loc_vprintf() */
static int loc_vprintf(const char *_fmt, va_list _l) {
   int ret;
   char buf[DSP_LOGBUF_LINE_SIZE];

   ret = vsnprintf(buf, DSP_LOGBUF_LINE_SIZE, _fmt, _l);

   if(ret > 0)
   {
      ret = loc_puts(buf);
   }

   return ret;
}


/* --------------------------------------------------------------------------- loc_syscalls_init() */
static void loc_syscalls_init(void) {

   memset(&syscalls, 0, sizeof(mlb_syscalls_t));

   /* c64_tools / mailbox */
   syscalls.mlb_panic                  = &mlb_panic;
   syscalls.mlb_debug_usr              = &mlb_debug_usr;
   syscalls.mlb_fastcall_initiate      = &mlb_fastcall_initiate;
   syscalls.mlb_component_register     = &mlb_syscall_component_register;
   syscalls.mlb_component_find_by_name = &loc_component_find_by_name;
   syscalls.mlb_version                = &mlb_version;

   /* cache utility functions */
   syscalls.cache_inv       = (void (*)(void*, sUI, sBool)) &BCACHE_inv;
   syscalls.cache_wb        = (void (*)(void*, sUI, sBool)) &BCACHE_wb;
   syscalls.cache_wbInv     = (void (*)(void*, sUI, sBool)) &BCACHE_wbInv;
   syscalls.cache_invL1pAll = &BCACHE_invL1pAll;
   syscalls.cache_wbAll     = &BCACHE_wbAll;
   syscalls.cache_wbInvAll  = &BCACHE_wbInvAll;
   syscalls.cache_wait      = &BCACHE_wait;

   /* mem utility functions */
   syscalls.bzero   = &loc_bzero;
   syscalls.memset  = &memset;
   syscalls.memcpy  = &memcpy;

   /* string utility functions */
   syscalls.strncmp   = &strncmp;
   syscalls.strncpy   = &strncpy;
   syscalls.strstr    = &strstr;
   syscalls.strchr    = &strchr;
   syscalls.strrchr   = &strrchr;
   syscalls.snprintf  = &snprintf;
   syscalls.vsnprintf = &vsnprintf;
   syscalls.puts      = &loc_puts;
   syscalls.printf    = &loc_printf;
   syscalls.vprintf   = &loc_vprintf;

   /* QDMA utility functions */
   syscalls.qdma_init   = &qdma_init;
   syscalls.qdma_wait   = &qdma_wait;
   syscalls.qdma_copy1d = &qdma_copy1d;
   syscalls.qdma_copy2d = &qdma_copy2d;
   syscalls.qdma_link1d = &qdma_link1d;
   syscalls.qdma_link2d = &qdma_link2d;
}


/* --------------------------------------------------------------------------- loc_components_reset() */
static void loc_components_reset(void) {

   memset(components, 0, sizeof(components));
   
   /* Register the CORE component */
   components[0] = component_mlb;
   components[0].component_id = 0;
   
   num_core_components = 1;
}


/* --------------------------------------------------------------------------- loc_core_components_init() */
static void loc_core_components_init(void) {
   /* Initialize resident components */
   sUI i;

   for(i=0; i<num_core_components; i++)
   {
      if(NULL != components[i].fxns.init)
      {
         if(!components[i].fxns.init())
         {
            /* Failed: component failed to initialize */
            mlb_panic();
         }
      }
      
      /* Next component */
   }
}


/* --------------------------------------------------------------------------- loc_its_alive() */
void loc_its_alive(void) {
   /* Write some identity values so that GPP side can verify that DSP is actually running
    *
    *  (note) the first two cachelines (64 bytes) are reserved for GPP<>DSP communication
    *
    */
   sU32 i;
   volatile sU32 *resetBuf = (sU32*) (dsp_config.reset_vector.phys_addr + DSP_RESET_VECTOR_CODESIZE);

   for(i=0; i<16; i++)
   {
      resetBuf[i] = 0;
   }

   for(i=16; i<((DSP_RESET_VECTOR_MINSIZE - DSP_RESET_VECTOR_CODESIZE)/4); i++)
   {
      resetBuf[i] = i;
   }

   /* Signal GPP side that DSP startup is complete */
   IPC_REG(DSP_IPC_BOOTMODE_OFF) = DSP_BOOTFLAG_RUNNING;

   BCACHE_wbAll();
}


/* --------------------------------------------------------------------------- mlb_irq__handler() */
void mlb_irq_handler(void *_arg) {
   (void)_arg;
   sU32 numMsg = 0;

#ifdef DSP_MAILBOX_RESET
   /* Disable mailbox interrupt to prevent interrupt spam */
   DSP_REG(MLB_IRQENABLE_1) = 0;

   /* Acknowledge interrupt */
   DSP_REG(MLB_IRQSTATUS_1) = (1u << 0); /* clear NEWMSGSTATUSUUMB0 */
#endif

   {
      numMsg = DSP_REG(MLB_MSGSTATUS_0) & 7u; /* always 1 because of HW workaround */

      if(numMsg > 0)
      {
         sUI fifoIdx = 0;

         while(fifoIdx < numMsg)
         {
            mlb_inbox[fifoIdx] = DSP_REG(MLB_MESSAGE_0);

            /* Next message */
            fifoIdx++;
         }
      }
   }

#ifndef DSP_MAILBOX_RESET
   if(numMsg > 0)
#endif
   {
#ifndef DSP_MAILBOX_RESET
      /* Acknowledge interrupt */
      DSP_REG(MLB_IRQSTATUS_1) = (1u << 0); /* clear NEWMSGSTATUSUUMB0 */
#endif

      int_irq_clear(MLB_INT_VEC_ID);
      
      int_event_clear(IVA2_EVT_MAIL_U1_IRQ);
      
      C64_clearIFR(1 << MLB_INT_VEC_ID);
      
      //SEM_post(mlb_sem_handle);
      
      /* now this is ugly but w/o it, the ARM will not receive interrupts or the entire system will freeze (why??) */
      BCACHE_wbInvAll();
      
      num_irqs++;
   }
}


/* --------------------------------------------------------------------------- mlb_send_to_gpp() */
void mlb_send_to_gpp(sU32 _msg) {

   DSP_REG(MLB_MESSAGE_1) = ~mlb_inbox[0];
}


/* --------------------------------------------------------------------------- mlb_task__entry() */
static void mlb_task__entry(void) {

   num_irqs      = 0;
   num_msg_recvd = 0;

   {
#ifdef ENABLE_IPC_DEBUG
      sU32 iterCount = 0x10000000u;
#endif


      num_irqs = 0x20000000u;

#ifdef ENABLE_IPC_DEBUG
      loc_debug(0x07100001u);
#endif


#if 1
      mlb_logbuf               = (char*) DSP_LOGBUF_BASE_GPP; /* same as DSP */
      mlb_logbuf_offset_writep = (sU32*) (DSP_LOGBUF_BASE_GPP + DSP_LOGBUF_SIZE);
#endif

#if 1
      /* Initialize core / resident components */
      if(!mlb_resume())
      {
         loc_syscalls_init();

#ifdef ENABLE_IPC_DEBUG
         loc_debug(0x07110001u);
#endif

         loc_components_reset();

#ifdef ENABLE_IPC_DEBUG
         loc_debug(0x07110002u);
#endif

         if(!core_components_register())
         {
#ifdef ENABLE_IPC_DEBUG
            loc_debug(0x07110FFFFu);
#endif

            mlb_panic();
         }

#ifdef ENABLE_IPC_DEBUG
         loc_debug(0x07110003u);
#endif

         loc_core_components_init();
      }
#endif // 0

#ifdef ENABLE_IPC_DEBUG
      loc_debug(0x07000001u);
#endif


#if 1
      mlb_debug_usr(0, 0xCCddCCddu);
      mlb_debug_usr(1, 0xCCddCCddu);
      mlb_debug_usr(2, 0xCCddCCddu);
      mlb_debug_usr(3, 0xCCddCCddu);
#endif

#if 1
      IPC_REG(DSP_FSHM_MSG_TODSP_CTL_OFF) = DSP_FSHM_FASTCALL_CTL_DONE;
      IPC_REG(DSP_FSHM_MSG_TOGPP_CTL_OFF) = DSP_FSHM_FASTCALL_CTL_DONE;
#endif

      /* Fill buffer so ARM-side can see that the DSP is doing something */
      loc_its_alive();

#ifdef ENABLE_IPC_DEBUG
      loc_debug(0x07000002u);
#endif

#if 1
      {
         sUI oldNumIrqs = num_irqs;
         sUI oldMsgSerial = 0;

         IPC_REG(DSP_IPC_DEBUG_DSPIRQCOUNT_OFF) = 0;

         for(;;)
         {
            /* Sleep until message interrupt arrives */
            asm(" NOP\n");
            asm(" NOP\n");
            asm(" NOP\n");
            asm(" NOP\n");
            asm(" NOP\n");
            asm(" NOP\n");
            asm(" NOP\n");
            asm(" NOP\n");

            asm(" IDLE\n");

            asm(" NOP\n");
            asm(" NOP\n");
            asm(" NOP\n");
            asm(" NOP\n");
            asm(" NOP\n");
            asm(" NOP\n");
            asm(" NOP\n");
            asm(" NOP\n");

            if(num_irqs != oldNumIrqs)
            {
               sUI newMsgSerial;

#ifdef ENABLE_IPC_DEBUG
               loc_debug(0x07000003u);
#endif

               oldNumIrqs = num_irqs;

               newMsgSerial = IPC_REG(DSP_IPC_MSG_SERIAL_OFF);

#ifdef ENABLE_IPC_DEBUG
               IPC_REG(DSP_IPC_DEBUG_DSPIRQCOUNT_OFF) = num_irqs;
#endif

#ifdef ENABLE_IPC_DEBUG
               loc_debug(0x07000004u);
#endif

               if(oldMsgSerial != newMsgSerial)
               {
#ifdef ENABLE_IPC_DEBUG
                  sU32 rpcDebug = 0;
#endif

                  oldMsgSerial = newMsgSerial;

#ifdef ENABLE_IPC_DEBUG
                  loc_debug(0x07000005u);
#endif

                  /* Try to execute requested command */
#ifdef ENABLE_COMPONENT_DISPATCHER
                  {
                     sU32 compAndCmd = IPC_REG(DSP_IPC_MSG_COMP_OFF);

                     const dsp_component_t *comp = loc_component_find_by_id(
                           (dsp_component_id_t)(compAndCmd & 0xFFFFu)
                           );

                     if(NULL != comp)
                     {
                        sU32 err;

                        /* Execute component command
                         *
                         *  (note) the first +4u skips the "client" field of the dsp_msg_t structure
                         *  (note) +2u then addresses the cmd_id (sU16)
                         *  (note) the remaining +4u resp. +8u address the arg1 resp. arg2 fields
                         *
                         *  (note) also see "msg.h"
                         */

                        sU32 r1;
                        sU32 r2;

                        err = comp->fxns.exec(
                              (dsp_component_cmd_t)(compAndCmd >> 16u),
                              IPC_REG(DSP_IPC_MSG_ARG1_OFF),
                              IPC_REG(DSP_IPC_MSG_ARG2_OFF),
                              &r1,
                              &r2
                              );

                        IPC_REG(DSP_IPC_MSG_RET1_OFF) = r1;
                        IPC_REG(DSP_IPC_MSG_RET2_OFF) = r2;

                        IPC_REG(DSP_IPC_ERRNO_OFF) = err;

                        if(0 == err)
                        {
                           /* Ok, command completed successfully */
#ifdef ENABLE_IPC_DEBUG
                           rpcDebug = 0x70000000u;

                           rpcDebug |= (r1 & 255) << 8u;
#endif

                           if(DSP_FSHM_FASTCALL_CTL_IDLE == IPC_REG(DSP_FSHM_MSG_TOGPP_CTL_OFF))
                           {
                              /* RPC initiated fastcall sequence */
                              sU32 errFC;
                              sU32 timeout;

                              for(;;)
                              {
                                 sU32 ctlFC;

                                 timeout = 0;

                                 do
                                 {
                                    /* Read control 'register' */
                                    ctlFC = IPC_REG(DSP_FSHM_MSG_TODSP_CTL_OFF);

                                    timeout++;

                                    asm(" NOP\n");
                                    asm(" NOP\n");
                                    asm(" NOP\n");
                                    asm(" NOP\n");
                                    asm(" NOP\n");
                                    asm(" NOP\n");
                                    asm(" NOP\n");
                                    asm(" NOP\n");
                                    
                                    /* Wait for GPP to request RPC */
                                 }
                                 while
                                    (
                                       (ctlFC == DSP_FSHM_FASTCALL_CTL_IDLE) &&
                                       (timeout < FASTCALL_TIMEOUT)
                                     );

                                 if(timeout < FASTCALL_TIMEOUT)
                                 {
                                    if(DSP_FSHM_FASTCALL_CTL_REQ == IPC_REG(DSP_FSHM_MSG_TODSP_CTL_OFF))
                                    {
                                       if(NULL != comp->fxns.exec_fc)
                                       {
                                          sU32 r1FC;
                                          sU32 r2FC;

                                          /* (note) when the following code lines are compiled with opt. level 2,
                                           *         the resulting DSP image will not load (!)
                                           */
                                          dsp_component_cmd_t cmdFC =
                                                (dsp_component_cmd_t)
                                                DSP_COMPONENT_FC_CMD_DISPATCH(IPC_REG(DSP_FSHM_MSG_TODSP_CMD_OFF))
                                                ;

                                          errFC = comp->fxns.exec_fc(
                                                     cmdFC,
                                                     IPC_REG(DSP_FSHM_MSG_TODSP_ARG1_OFF),
                                                     IPC_REG(DSP_FSHM_MSG_TODSP_ARG2_OFF),
                                                     &r1FC,
                                                     &r2FC
                                                  );

                                          IPC_REG(DSP_FSHM_MSG_TOGPP_RET1_OFF) = r1FC;
                                          IPC_REG(DSP_FSHM_MSG_TOGPP_RET2_OFF) = r2FC;

                                          IPC_REG(DSP_IPC_ERRNO_OFF) = errFC;

                                          if(1)
                                          {
                                             /* Notify GPP that call results are available */
                                             IPC_REG(DSP_FSHM_MSG_TOGPP_CTL_OFF) = DSP_FSHM_FASTCALL_CTL_ACK;
                                             
                                             timeout = 0;
                                             
                                             do
                                             {
                                                /* Read control 'register' */
                                                ctlFC = IPC_REG(DSP_FSHM_MSG_TODSP_CTL_OFF);
                                                
                                                timeout++;

                                                asm(" NOP\n");
                                                asm(" NOP\n");
                                                asm(" NOP\n");
                                                asm(" NOP\n");
                                                asm(" NOP\n");
                                                asm(" NOP\n");
                                                asm(" NOP\n");
                                                asm(" NOP\n");
                                                
                                                /* Wait for GPP to ack RPC return */
                                             }
                                             while
                                                (
                                                   (ctlFC != DSP_FSHM_FASTCALL_CTL_IDLE)
                                                   && (timeout < FASTCALL_TIMEOUT)
                                                 );

                                             IPC_REG(DSP_FSHM_MSG_TOGPP_CTL_OFF) = DSP_FSHM_FASTCALL_CTL_IDLE;

                                             if(timeout == FASTCALL_TIMEOUT)
                                             {
                                                /* GPP failed to acknowledge return */
                                                break;
                                             }

                                          }
                                          else
                                          {
                                             /* Failed: RPC did not succeed */
                                          }
                                       }
                                       else
                                       {
                                          /* Failed: exec_fc() function not implemented */
                                          IPC_REG(DSP_FSHM_MSG_TOGPP_CTL_OFF) = DSP_FSHM_FASTCALL_CTL_IDLE;
                                       }
                                    }
                                    else
                                    {
                                       /* CTL_DONE (or ERR) */
                                       break;
                                    }
                                 }
                                 else
                                 {
                                    /* Timeout occured while waiting for GPP request */
                                    IPC_REG(DSP_FSHM_MSG_TOGPP_CTL_OFF) = DSP_FSHM_FASTCALL_CTL_IDLE;
                                    break;
                                 }

                              } /* Process fastcall RPCs */

                              IPC_REG(DSP_FSHM_MSG_TOGPP_CTL_OFF) = DSP_FSHM_FASTCALL_CTL_DONE;

                           } /* if fastcall sequence initiated */
                        }
                        else
                        {
                           /* Propagate error code to GPP */
                           IPC_REG(DSP_IPC_ERRNO_OFF) = err;
#ifdef ENABLE_IPC_DEBUG

                           rpcDebug = 0x30000000u;

                           rpcDebug |=
                                 ((err & 255u) << 16u) |
                                 ((*(dsp_component_cmd_t*)(IPC_REG(DSP_IPC_MSG_COMP_OFF))) << 8u)
                                 ;
#endif
                        }

                     }
                     else
                     {
                        /* Failed: component not found */
#ifdef ENABLE_IPC_DEBUG
                        rpcDebug = 0x10000000u;
#endif
                     }
                  }
#endif /* ENABLE_COMPONENT_DISPATCHER */


#ifdef ENABLE_IPC_DEBUG
                  loc_debug(0x07000006u | rpcDebug);
#endif

                  /* Send reply to trigger IRQ on GPP-side.
                   *
                   *  (note) the content of the message register does not matter,
                   *          it's only written to in order to trigger an interrupt.
                   *
                   *  (note) even if the command failed or the requested component does not exist,
                   *          a reply message must be sent.
                   */
                  mlb_send_to_gpp(~mlb_inbox[0]);

#ifdef ENABLE_IPC_DEBUG
                  loc_debug(0x07000007u | rpcDebug);

                  iterCount = 0;
#endif

               } // if(oldMsgSerial != newMsgSerial)

            } // if(num_irqs != oldNumIrqs)

#ifdef ENABLE_IPC_DEBUG
            iterCount++;

            IPC_REG(DSP_IPC_DEBUG_DSPITERCOUNT_OFF) = iterCount;
#endif

         } // for(;;);
      }
#else
      for(;;); // xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
#endif // 0
   }
}


/* --------------------------------------------------------------------------- mlb_start() */
sBool mlb_component_register (dsp_component_id_t     _compId,
                              const dsp_component_t *_comp
                              ) {
   sBool ret;

   if(_compId > 0)
   {
      if(NULL != _comp)
      {
         if(num_core_components < MLB_MAX_CORE_COMPONENTS)
         {
            if(NULL != _comp->fxns.exec)
            {
               components[num_core_components] = *_comp;

               components[num_core_components].component_id = _compId;

               num_core_components++;

               /* Succeeded */
               ret = S_TRUE;
            }
            else
            {
               /* Failed: no exec function */
               ret = S_FALSE;
            }
         }
         else
         {
            /* Failed: maximum number of components exceeded */
            ret = S_FALSE;
         }
      }
      else
      {
         /* Failed: component reference is NULL */
         ret = S_FALSE;
      }
   }
   else
   {
      /* Failed: illegal component id */
      ret = S_FALSE;
   }

   return ret;
}


/* --------------------------------------------------------------------------- mlb_debug_usr() */
void mlb_debug_usr(sU32 _idx, sU32 _val) {

   IPC_REG(DSP_IPC_DEBUG_USR0_OFF + (4u * (_idx & 3u))) = _val;
}


/* --------------------------------------------------------------------------- mlb_fastcall_initiate() */
void mlb_fastcall_initiate(void) {

   /* Initiate fastcall sequence */

   IPC_REG(DSP_FSHM_MSG_TOGPP_CTL_OFF) = DSP_FSHM_FASTCALL_CTL_IDLE;

   /* here be dragons */
}


/* --------------------------------------------------------------------------- mlb_panic() */
void mlb_panic(void) {

   /* Signal GPP-side that it needs to reboot the DSP */
   IPC_REG(DSP_IPC_ERRNO_OFF) = 0xbaadf00d;

   for(;;)
   {
      /* Loop a while.. loop..forever! */

      asm(" IDLE\n");
      asm(" NOP\n");
      asm(" NOP\n");
      asm(" NOP\n");
      asm(" NOP\n");
      asm(" NOP\n");
      asm(" NOP\n");
      asm(" NOP\n");
   }
}


/* --------------------------------------------------------------------------- mlb_resume() */
sBool mlb_resume(void) {
   return (DSP_BOOTFLAG_RESUME == IPC_REG(DSP_IPC_BOOTMODE_OFF));
}


/* --------------------------------------------------------------------------- mlb_start() */
void mlb_start(void) {

   /* Create ARM<>DSP mailbox task
    *
    *  (todo) use tconf to create the task statically (size opt.)
    */
   mlb_task_attrs = TSK_ATTRS;
   mlb_task_attrs.name     = "mlb";
   mlb_task_attrs.priority = TSK_MINPRI;

   /*mlb_task_handle = */(void)TSK_create((Fxn)mlb_task__entry, &mlb_task_attrs);
}
