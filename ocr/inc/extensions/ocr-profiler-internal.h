/* Modified in 2014-16 by Romain Cledat (now at Intel). The original
 * license (BSD) is below. This file is also subject to the license
 * aggrement located in the file LICENSE and cannot be distributed
 * without it. This notice cannot be removed or modified
 */

/* Copyright (c) 2011, Romain Cledat
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Georgia Institute of Technology nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL ROMAIN CLEDAT BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __OCR_PROFILER_INTERNAL_H__
#define __OCR_PROFILER_INTERNAL_H__

#ifdef OCR_RUNTIME_PROFILER
#include "ocr-types.h"

struct __profilerData;

typedef struct __profilerFlags {
    u8 active    :1;
    u8 isPaused  :1;
    u8 isRecurse :1;
    u8 hasAddLevel :1;
    u8 isRtCall  :1;
    u8 _pad      :3;
} _profilerFlags;

typedef struct __profiler {
    u64 startTicks, endTicks;
    u64 recurseAccumulate; // Accumulates ticks to be substracted from parents for recursion
    u64 currentRecurseAccumulate; // Counts for each child (will accumulate in recurseAccumulate when child is destroyed
    s64 accumulatorTicks; // Hopefully no overflow
    u64 accumulatedChildrenTicks; // Ticks from children
    struct __profilerData *myData;
    u32 countResume;
    u32 onStackCount;
    u32 myEvent;
    u32 previousLastLevel; // Contains the level that was in stackPosition before
                           // we started. If isRecurse is true, this is also the
                           // level from which we should remove our time from
    _profilerFlags flags;
} _profiler;

#define _gettime(ticks)                         \
    do {                                        \
        u64 d;                                  \
        __asm__ __volatile__ (                  \
            "rdtscp \n\t"                       \
            : "=a" (ticks), "=d" (d)            \
            : /* no input */                    \
            : "ecx"                             \
            );                                  \
        ticks |= (d << 32);                     \
    } while(0);


/* Non-inline profiler functions */
// maxLevels is -1 if we don't care about starting/stopping levels. Otherwise, track
// from this level to this level + maxLevel
void _profilerInit(_profiler *self, u32 ev, u64 lastTick);
_profiler* _profilerDestroy(_profiler *self, u64 tick);
void _profilerPauseInternal(_profiler *self);
void _profilerResumeInternal(_profiler *self);

/* _profiler inline functions */
/* If realEndTicks is not zero, it means that we are not really "pausing" but rather
   stopping to count to eliminate overhead
*/
static inline void _profilerPause(_profiler *self, u64 realEndTicks) __attribute__((always_inline));
static inline void _profilerPause(_profiler *self, u64 realEndTicks) {
    // Code looks weird to try to get _gettime as early as possible
    if(realEndTicks == 0) {
        _gettime(self->endTicks);
        if(self->flags.active && !self->flags.isPaused) {
            _profilerPauseInternal(self);
        }
    } else {
        self->endTicks = realEndTicks;
        if(self->flags.active && !self->flags.isPaused) {
            // Here: self->endTicks > self->startTicks
            self->accumulatorTicks += self->endTicks - self->startTicks;
            self->flags.isPaused = 1;
        }
    }
}

/* If "isFakePause", this corresponds to the symmetric call of _profilerPause with realEndTicks != 0 */
static inline void _profilerResume(_profiler *self, u8 isFakePause) __attribute__((always_inline));
static inline void _profilerResume(_profiler *self, u8 isFakePause) {
    if(self->flags.active && self->flags.isPaused) {
        if(!isFakePause) {
            _profilerResumeInternal(self);
        }
        self->flags.isPaused = 0;
        ++self->countResume;
        _gettime(self->startTicks);
    }
}

/* the overhead we are not accounting for is:
  *    - overhead of _gettime
 */
#define START_PROFILE(eventId)                                          \
    u64 _tempTicks;                                                     \
    _gettime(_tempTicks)                                                \
    /*_sync_synchronize();*/                                            \
    _profiler _flightweight;                                            \
    _profilerInit(&_flightweight, eventId, _tempTicks);                 \
    _gettime(_flightweight.startTicks);

#define PAUSE_PROFILE                           \
    do { _profilerPause(&_flightweight, 0); } while(0);

#define RESUME_PROFILE                          \
    do { _profilerResume(&_flightweight, 0); } while(0);

#define RETURN_PROFILE(val)                                             \
    do {                                                                \
        _gettime(_tempTicks);                                           \
        /*_sync_synchronize(); */                                       \
        _profiler *_tres = _profilerDestroy(&_flightweight, _tempTicks); \
        if(_tres) _profilerResume(_tres, 1);                            \
        return val;                                                     \
    } while(0);

#define EXIT_PROFILE                                                    \
    do {                                                                \
        _gettime(_tempTicks);                                           \
        /*_sync_synchronize();*/                                        \
        _profiler *_tres = _profilerDestroy(&_flightweight, _tempTicks); \
        if(_tres) _profilerResume(_tres, 1);                            \
    } while(0);

#else

#ifdef OCR_RUNTIME_PROFILER_TAU
#include <Profile/Profiler.h>
#define START_PROFILE(name)                                             \
    TAU_PROFILE_TIMER(_tau_profile, #name, "", TAU_USER);               \
    TAU_PROFILE_START(_tau_profile);

/* TAU doesn't have PAUSE or RESUME functionality. */
#define PAUSE_PROFILE
#define RESUME_PROFILE

#define RETURN_PROFILE(val)                                             \
    do {                                                                \
        TAU_PROFILE_STOP(_tau_profile);                                 \
        return val;                                                     \
    } while(0);

#define EXIT_PROFILE                                                    \
    do {                                                                \
        TAU_PROFILE_STOP(_tau_profile);                                 \
    } while(0);

#else

#define START_PROFILE(name)
#define PAUSE_PROFILE
#define RESUME_PROFILE

#define RETURN_PROFILE(val) return val;
#define EXIT_PROFILE
#endif /* OCR_RUNTIME_PROFILER_TAU */
#endif /* OCR_RUNTIME_PROFILER */

#endif /* __OCR_PROFILER_INTERNAL_H__ */

