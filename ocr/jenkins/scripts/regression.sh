#!/bin/bash

if [ $1 == "_params" ]; then
    if [ $2 == "output" ]; then
        echo "${JJOB_START_HOME}/ocr/tests/tests-log/TESTS-TestSuites.xml"
        exit 0
    fi
else
    export OCR_INSTALL=${JJOB_START_HOME}/ocr/install/$1/
    export LD_LIBRARY_PATH=${OCR_INSTALL}/lib/
    cd ${JJOB_START_HOME}/ocr/tests/
    ./ocrTests
    exit $?
fi
