#include "perfs.h"
#include "ocr.h"

// DESC: Create FAN_OUT producer ONCE events and one consumer EDT depending on all of them
// TIME: Satisfy all producers event
// FREQ: Done 'NB_ITERS' times
// NOTE: The driver EDT is a finish EDT to collect created EDTs

#define PRODUCER_EVENT_TYPE  OCR_EVENT_STICKY_T

#define TIME_SATISFY 1
#define TIME_CONSUMER_CREATE 0
#define TIME_ADD_DEP 0
#define CLEAN_UP_ITERATION 1

#include "event1FanInEdt.ctpl"
