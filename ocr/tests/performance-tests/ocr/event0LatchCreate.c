#include "perfs.h"
#include "ocr.h"

// DESC: Creates NB_INSTANCES events, then destroy them.
// TIME: Creation of NB_INSTANCES events, measured 'NB_ITERS' times
// FREQ: Done 'NB_ITERS' times.

#define TIME_CREATION 1
#define TIME_DESTRUCTION 0
#define CLEAN_UP_ITERATION 1

#define EVENT_TYPE OCR_EVENT_LATCH_T

#include "event0.ctpl"
