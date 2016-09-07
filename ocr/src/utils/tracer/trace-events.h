 /*
  * This file is subject to the license agreement located in the file LICENSE
  * and cannot be distributed without it. This notice cannot be
  * removed or modified.
  */

#ifndef __TRACE_EVENTS_H__
#define __TRACE_EVENTS_H__

//Strings to identify user/runtime created objects
const char *evt_type[] = {
    "RUNTIME",
    "USER"
};

//Strings for traced OCR objects
const char *obj_type[] = {
    "EDT",
    "EVENT",
    "MESSAGE",
    "DATABLOCK"
    "DATABLOCK",
    "WORKER",
    "SCHEDULER",
    "DEQUE"
};

//Strings for traced OCR events
const char *action_type[] = {
    "CREATE",
    "DESTROY",
    "RUNNABLE",
    "SCHEDULED",
    "ADD_DEP",
    "SATISFY",
    "EXECUTE",
    "FINISH",
    "SHIFT",
    "DATA_ACQUIRE",
    "DATA_RELEASE",
    "END_TO_END",
    "WORK_REQUEST",
    "BEGIN_WORK_STEAL",
    "WORK_TAKEN",
    "WORK_AVAIL",
    "WORK_SPIN",
    "WORK_DEPLETED",
    "SCHED_MSG_SEND",
    "SCHED_MSG_RCV",
    "SCHED_INVOKE",
    "SCHED_HEUR_INVOKE"
    "ACTION_MAX"
};

#endif /* __TRACE_EVENTS_H__ */
