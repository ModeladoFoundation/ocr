#!/bin/bash

# We start out in JJOB_PRIVATE_HOME
# Nothing is copied over so we can do some fine-tuning; the repos are getting
# way too big...
if [ $# > 0 ]; then
    TG=$1
else
    TG=""
fi

# Copy things from the initial directory
rsync -av ${JJOB_INITDIR_OCR} ${JJOB_PRIVATE_HOME} --exclude .git
rsync -av  ${JJOB_INITDIR_APPS} ${JJOB_PRIVATE_HOME} --exclude .git

if [ "x$TG" == "xtg" ]; then
    mkdir -p ${JJOB_PRIVATE_HOME}/tg/tg/
    cp -r ${JJOB_INITDIR_TG}/tg/tgkrnl ${JJOB_PRIVATE_HOME}/tg/tg
    cp -r ${JJOB_INITDIR_TG}/tg/build ${JJOB_PRIVATE_HOME}/tg/tg
    cp -r ${JJOB_INITDIR_TG}/tg/common ${JJOB_PRIVATE_HOME}/tg/tg
    cp -r ${JJOB_INITDIR_TG}/tg/jenkins ${JJOB_PRIVATE_HOME}/tg/tg
fi

# Make sure the Jenkins system is fully accessible in the shared home
mkdir -p ${JJOB_SHARED_HOME}/ocr/jenkins
mkdir -p ${JJOB_SHARED_HOME}/ocr/ocr/jenkins
mkdir -p ${JJOB_SHARED_HOME}/ocr/ocr/scripts

cp -r ${JJOB_PRIVATE_HOME}/ocr/jenkins/* ${JJOB_SHARED_HOME}/ocr/jenkins/
cp -r ${JJOB_PRIVATE_HOME}/ocr/ocr/jenkins/* ${JJOB_SHARED_HOME}/ocr/ocr/jenkins/
cp -r ${JJOB_PRIVATE_HOME}/ocr/ocr/scripts/* ${JJOB_SHARED_HOME}/ocr/ocr/scripts/

if [ "x$TG" == "xtg" ]; then
    mkdir -p ${JJOB_SHARED_HOME}/tg/tg/jenkins
    cp -r ${JJOB_PRIVATE_HOME}/tg/tg/jenkins/* ${JJOB_SHARED_HOME}/tg/tg/jenkins/
fi
