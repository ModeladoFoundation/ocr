#include "perfs.h"
#include "ocr.h"

// DESC: One worker creates all the tasks. Sink EDT depends on
//       all tasks through the output-event of a finish EDT.
// TIME: Completion of all tasks
// FREQ: Create 'NB_INSTANCES' EDTs once
//
// VARIABLES:
// - NB_INSTANCES
// - PARAMC_SIZE: the size of paramc for the created EDTs

// TODO DEFAULT VALUE HERE
#include "edtExecuteFinishSync0.ctpl"
