/**
 * @brief OCR versioning macros
 **/

/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __OCR_VERSION_H__
#define __OCR_VERSION_H__

/****************************************************************************/
/* The below macros define the pieces of OCR version and the version string */
/****************************************************************************/

#define OCR_MAJOR_VERSION 1
#define OCR_MINOR_VERSION 0
#define OCR_PATCH_VERSION 1

/* OCR version string */
#define OCR_VERSION tostr(OCR_MAJOR_VERSION) DOT tostr(OCR_MINOR_VERSION) \
                    DOT tostr(OCR_PATCH_VERSION)

/****************************************************************************/
/* The below macros can be used on any version string to extract its pieces */
/****************************************************************************/

#define OCR_VERSION_GET_MAJOR(verstr) ocrVersionExtractMajor(verstr)
#define OCR_VERSION_GET_MINOR(verstr) ocrVersionExtractMinor(verstr)
#define OCR_VERSION_GET_PATCH(verstr) ocrVersionExtractPatch(verstr)

/****************************************************************************/
/* The below macros can be used to test for extensions built with runtime   */
/****************************************************************************/

/* This macro returns the extension support represented as a bitmap */
#define OCR_VERSION_EXTENSION_BITMAP ocrVersionExtensionBitmap()

/* Use the following macros to get bitmask for various extensions */

// Legacy feature support - ocrLegacyInit(), ocrLegacyFinalize(), etc.
#define OCR_VERSION_LEGACY_BIT            (1<<EXTENSION_LEGACY_BITPOS)
// Features to affinitize task/data to particular domains
#define OCR_VERSION_AFFINITY_BIT          (1<<EXTENSION_AFFINITY_BITPOS)
// Allow initial counter value to be specified for latch events
#define OCR_VERSION_PARAMS_EVT_BIT        (1<<EXTENSION_PARAMS_EVT_BITPOS)
// Implementation of counted events: events that are automatically destroyed
// after a pre-defined number of satisfactions
#define OCR_VERSION_COUNTED_EVT_BIT       (1<<EXTENSION_COUNTED_EVT_BITPOS)
// Experimental features such as EDT Local Storage management,
// worker information, etc.
#define OCR_VERSION_RTITF_BIT             (1<<EXTENSION_RTITF_BITPOS)
// Allows use of runtime/pause features using ocrPause() and ocrResume()
#define OCR_VERSION_PAUSE_BIT             (1<<EXTENSION_PAUSE_BITPOS)
// Implementation of labeled GUIDs
#define OCR_VERSION_LABELING_BIT          (1<<EXTENSION_LABELING_BITPOS)

/***************** Versioning internals below **************************/

#define str(s) #s
#define tostr(s) str(s)
#define DOT "."

extern u64 ocrVersionExtensionBitmap(void);
extern u32 ocrVersionExtractMajor(u8 *verstr);
extern u32 ocrVersionExtractMinor(u8 *verstr);
extern u32 ocrVersionExtractPatch(u8 *verstr);

#define EXTENSION_LEGACY_BITPOS        1
#define EXTENSION_AFFINITY_BITPOS      2
#define EXTENSION_PARAMS_EVT_BITPOS    3
#define EXTENSION_COUNTED_EVT_BITPOS   4
#define EXTENSION_RTITF_BITPOS         5
#define EXTENSION_PAUSE_BITPOS         6
#define EXTENSION_LABELING_BITPOS      7

#endif /* __OCR_VERSION_H__ */