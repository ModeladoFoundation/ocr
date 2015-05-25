#include "perfs.h"
#include "ocr.h"

// DESC: Creates NB_INSTANCES events, then destroy them.
// TIME: Destruction of NB_INSTANCES events, measured 'NB_ITERS' times
// FREQ: Done 'NB_ITERS' times.
//
// VARIABLES:
// - NB_INSTANCES
// - NB_ITERS

#define TIME_CREATION 0
#define TIME_DESTRUCTION 1

// Can clean-up non-persistent event here because events
// are only created but not satisfied in this test.
#define CLEAN_UP_ITERATION 1

#define EVENT_TYPE OCR_EVENT_ONCE_T

#include "event0.ctpl"
