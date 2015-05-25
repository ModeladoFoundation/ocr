#include "perfs.h"
#include "ocr.h"

// DESC: Create FAN_OUT producer ONCE events and one consumer EDT depending on all of them
// TIME: Setting up the dependence between the producer event and consumer EDTs
// FREQ: Done 'NB_ITERS' times
// NOTE: The driver EDT is a finish EDT to collect created EDTs
//
// VARIABLES
// - NB_ITERS
// - FAN_OUT

#define PRODUCER_EVENT_TYPE  OCR_EVENT_ONCE_T

#define TIME_SATISFY 0
#define TIME_CONSUMER_CREATE 0
#define TIME_ADD_DEP 1
#define CLEAN_UP_ITERATION 0

#include "event1FanInEdt.ctpl"
