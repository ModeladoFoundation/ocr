OCR_SRC_WORKPILE=workpile

noinst_LTLIBRARIES += libocr_workpile.la
libocr_la_LIBADD += libocr_workpile.la

libocr_workpile_la_SOURCES = \
$(OCR_SRC_WORKPILE)/ocr-workpile.c

libocr_workpile_la_CFLAGS = $(AM_CFLAGS)


#
# OCR-HC workpile implementation
#
noinst_LTLIBRARIES += libocr_workpile_hc.la
libocr_la_LIBADD += libocr_workpile_hc.la

libocr_workpile_hc_la_SOURCES = \
$(OCR_SRC_WORKPILE)/ocr-workpile-hc/ocr-workpile-hc.c \
$(OCR_SRC_WORKPILE)/ocr-workpile-hc/deque.c \
$(OCR_SRC_WORKPILE)/ocr-workpile-hc/heap.c

libocr_workpile_hc_la_CFLAGS = $(AM_CFLAGS)#

# OCR-FSIM workpile implementation
#
noinst_LTLIBRARIES += libocr_workpile_fsim.la
libocr_la_LIBADD += libocr_workpile_fsim.la

libocr_workpile_fsim_la_SOURCES = \
$(OCR_SRC_WORKPILE)/ocr-workpile-fsim/ocr-workpile-fsim.c

libocr_workpile_fsim_la_CFLAGS = $(AM_CFLAGS)