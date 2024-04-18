/******************************************************************************

  Copyright (c) 2001-2016, Intel Corporation
  All rights reserved.
  
  Redistribution and use in source and binary forms, with or without 
  modification, are permitted provided that the following conditions are met:
  
   1. Redistributions of source code must retain the above copyright notice, 
      this list of conditions and the following disclaimer.
  
   2. Redistributions in binary form must reproduce the above copyright 
      notice, this list of conditions and the following disclaimer in the 
      documentation and/or other materials provided with the distribution.
  
   3. Neither the name of the Intel Corporation nor the names of its 
      contributors may be used to endorse or promote products derived from 
      this software without specific prior written permission.
  
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE 
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.

******************************************************************************/
/*$FreeBSD$*/


#ifndef _FREEBSD_OS_H_
#define _FREEBSD_OS_H_

#include <wdm.h>

#define usec_delay(x) KeStallExecutionProcessor(x)
#define usec_delay_irq(x) KeStallExecutionProcessor(x)
#define msec_delay(x) KeStallExecutionProcessor(1000*(x))
#define msec_delay_irq(x) KeStallExecutionProcessor(1000*(x))

#if DBG
#define DEBUGOUT(...) \
    do { DbgPrint(__VA_ARGS__); } while (0)
#else
#define DEBUGOUT(...)
#endif
#define DEBUGOUT1(...)			DEBUGOUT(__VA_ARGS__)
#define DEBUGOUT2(...)			DEBUGOUT(__VA_ARGS__)
#define DEBUGOUT3(...)			DEBUGOUT(__VA_ARGS__)
#define DEBUGOUT7(...)			DEBUGOUT(__VA_ARGS__)
#define DEBUGFUNC(F)			DEBUGOUT(F "\n")

/* Mutex used in the shared code */
#define E1000_MUTEX			struct mtx
#define E1000_MUTEX_INIT(mutex)		mtx_init((mutex), #mutex, \
                                            MTX_NETWORK_LOCK, \
                                            MTX_DEF | MTX_DUPOK)
#define E1000_MUTEX_DESTROY(mutex)	mtx_destroy(mutex)
#define E1000_MUTEX_LOCK(mutex)		mtx_lock(mutex)
#define E1000_MUTEX_TRYLOCK(mutex)	mtx_trylock(mutex)
#define E1000_MUTEX_UNLOCK(mutex)	mtx_unlock(mutex)

typedef ULONG64 u64;
typedef ULONG u32;
typedef USHORT u16;
typedef UCHAR u8;
typedef LONG64 s64;
typedef LONG s32;
typedef SHORT s16;
typedef CHAR s8;
#ifndef __cplusplus
typedef BOOLEAN bool;
#endif

#define E1000_WRITE_FLUSH(a) E1000_READ_REG(a, E1000_STATUS)

/* Register READ/WRITE macros */

#define E1000_READ_REG(hw, reg) \
    READ_REGISTER_ULONG((PULONG)((PUCHAR)((hw)->back) + (reg)))

#define E1000_WRITE_REG(hw, reg, value) \
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)((hw)->back) + (reg)), (value))

#define E1000_READ_REG_ARRAY(hw, reg, index) \
    E1000_READ_REG((hw), (reg) + ((index) << 2))

#define E1000_WRITE_REG_ARRAY(hw, reg, index, value) \
    E1000_WRITE_REG((hw), (reg) + ((index) << 2), (value))

#define E1000_READ_REG_ARRAY_DWORD E1000_READ_REG_ARRAY
#define E1000_WRITE_REG_ARRAY_DWORD E1000_WRITE_REG_ARRAY

#define E1000_READ_REG_ARRAY_BYTE(hw, reg, index) \
    READ_REGISTER_UCHAR((PUCHAR)((PUCHAR)((hw)->back) + ((reg) + (index)))

#define E1000_WRITE_REG_ARRAY_BYTE(hw, reg, index, value) \
    WRITE_REGISTER_UCHAR((PUCHAR)((PUCHAR)((hw)->back) + ((reg) + (index)), (value))

#define E1000_WRITE_REG_ARRAY_WORD(hw, reg, index, value) \
    WRITE_REGISTER_USHORT((PUSHORT)((PUCHAR)((hw)->back) + ((reg) + (index << 1)), (value))

#endif  /* _FREEBSD_OS_H_ */

