/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr.h"
#include "extensions/ocr-affinity.h"

/**
 * DESC: Create a lazy datablock that's shared with other policy-domains in a ring fashion.
 */

#define USE_LAZY_DB 0

#include "dbLazyRing.ctpl"
