/* Copyright (c) 2012, Rice University

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

1.  Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
2.  Redistributions in binary form must reproduce the above
     copyright notice, this list of conditions and the following
     disclaimer in the documentation and/or other materials provided
     with the distribution.
3.  Neither the name of Intel Corporation
     nor the names of its contributors may be used to endorse or
     promote products derived from this software without specific
     prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef HC_SYSDEP_H_
#define HC_SYSDEP_H_

//
// I386
//

#ifdef __i386__

#define HC_CACHE_LINE 32

static __inline__ void hc_mfence(void) {
	int x = 0, y;
	__asm__ volatile ("xchgl %0,%1" : "=r" (x) : "m" (y), "0" (x) : "memory");
}

/* 
 * if (*ptr == ag) { *ptr = x, return 1 }
 * else return 0;
 */
static __inline__ int hc_cas(volatile int *ptr, int ag, int x) {
	int tmp;
	__asm__ __volatile__("lock;\n"
			"cmpxchg %1,%3"
			: "=a" (tmp) /* %0 EAX, return value */
			  : "r"(x), /* %1 reg, new value */
			    "0" (ag), /* %2 EAX, compare value */
			    "m" (*(ptr)) /* %3 mem, destination operand */
			    : "memory", "cc" /* content changed, memory and cond register */
	);
	return tmp == ag;
}

#endif /* __i386__ */

//
// amd_64
//

#ifdef __x86_64

#define HC_CACHE_LINE 64

static __inline__ void hc_mfence() {
	__asm__ __volatile__("mfence":: : "memory");
}

/* 
 * if (*ptr == ag) { *ptr = x, return 1 }
 * else return 0;
 */
static __inline__ int hc_cas(volatile int *ptr, int ag, int x) {
	int tmp;
	__asm__ __volatile__("lock;\n"
			"cmpxchgl %1,%3"
			: "=a" (tmp) /* %0 EAX, return value */
			  : "r"(x), /* %1 reg, new value */
			    "0" (ag), /* %2 EAX, compare value */
			    "m" (*(ptr)) /* %3 mem, destination operand */
			    : "memory" /*, "cc" content changed, memory and cond register */
	);
	return tmp == ag;
}

#endif /* __x86_64 */

//
// Sparc
//

#ifdef __sparc__
#define HC_CACHE_LINE 64
#include <atomic.h>

static __inline__ void hc_mfence() {
	membar_consumer();
}

static __inline__ int hc_cas(volatile unsigned int *ptr, int ag, int x) {
	int old = atomic_cas_uint(ptr, ag, x);
	return old == ag;
}

#endif /* sparc */

//
// PowerPC 64
//

#ifdef __powerpc64__

#define HC_CACHE_LINE 128

static __inline__ void hc_mfence(void) {
        __asm__ __volatile__ ("sync"
	: : : "memory");
}

static __inline__ int hc_cas(volatile int *ptr, int ag, int x) {
        int prev;

        __asm__ __volatile__ (
	"lwsync \n\
1:      lwarx   %0,0,%2         # __cmpxchg_u32\n\
        cmpw    0,%0,%3\n\
        bne-    2f\n\
        stwcx.  %4,0,%2\n\
        bne-    1b \n\
2:	isync"
        : "=&r" (prev), "+m" (*ptr)
        : "r" (ptr), "r" (ag), "r" (x)
        : "cc", "memory");

        return prev==ag;
}

#endif /* __powerpc64__ */

#endif /* HC_SYSDEP_H_ */
