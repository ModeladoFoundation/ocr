/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __OCR_SAL_FSIM_XE_H__
#define __OCR_SAL_FSIM_XE_H__

#include "ocr-hal.h"

#define sal_abort() hal_abort()

#define sal_exit(x) hal_exit(x)

#define sal_assert(x, f, l) while(0) /*if(x) hal_abort() */

#define sal_print(msg) while(0) /* FIXME: TBD */

#endif /* __OCR_SAL_FSIM_XE_H__ */
