#!/bin/bash

#
# Main driver to compile, run and compare OCR performance tests
#
# For help invoke: ./compDriver.sh -help
# or check the -help option below
#

SCRIPT_NAME=${0##*/}

if [[ -z "${SCRIPT_ROOT}" ]]; then
    echo "error: ${SCRIPT_NAME} environment SCRIPT_ROOT is not defined"
    exit 1
fi

#
# Environment variables default values
#

# Number of runs to perform
export NB_RUN=${NB_RUN-"3"}

# Core scaling sweep
if [[ "${OCR_TYPE}" = "x86" ]]; then
    export CORE_SCALING=${CORE_SCALING-"1 2 4 8 16"}
else
    export CORE_SCALING=${CORE_SCALING-"2 4 8 16"}
fi

# Number of nodes is controlled by the caller

# Default runlog and report naming
export RUNLOG_FILENAME_BASE=${RUNLOG_FILENAME_BASE-"runlog"}
export REPORT_FILENAME_BASE=${REPORT_FILENAME_BASE-"report"}

#
# Option Parsing and Checking
#

NOCLEAN_OPT="no"
SWEEP_OPT="no"
SWEEPFILE_OPT="no"
SWEEPFILE_ARG=""
DEFSFILE_OPT="no"
DEFSFILE_ARG=""
TARGET_OPT="no"
TARGET_ARG="x86"
PROGRAMS=""

SWEEP_FOLDER="${SCRIPT_ROOT}/../configSweep"
TEST_FOLDER="${SCRIPT_ROOT}/../ocr"

if [[ ! -d ${SCRIPT_ROOT} ]]; then
    echo "error: ${SCRIPT_NAME} cannot find sweep config folder ${SWEEP_FOLDER}"
    exit 1
fi

if [[ ! -d ${TEST_FOLDER} ]]; then
    echo "error: ${SCRIPT_NAME} cannot find test folder ${TEST_FOLDER}"
    exit 1
fi

# Environment override
if [[ -n "${LOGDIR}" ]]; then
    LOGDIR_OPT="yes"
    LOGDIR_ARG="${LOGDIR}"
fi

# Environment is superceded by command line
while [[ $# -gt 0 ]]; do
    if [[ "$1" = "-noclean" ]]; then
        shift
        NOCLEAN_OPT="yes"
    elif [[ "$1" = "-sweep" ]]; then
        shift
        SWEEP_OPT="yes"
    elif [[ "$1" = "-target" && $# -ge 2 ]]; then
        shift
        TARGET_OPT="yes"
        TARGET_ARG=("$@")
        shift
    elif [[ "$1" = "-sweepfile" && $# -ge 2 ]]; then
        shift
        SWEEPFILE_OPT="yes"
        SWEEPFILE_ARG=("$@")
        if [[ ! -f ${SWEEPFILE_ARG} ]]; then
            echo "error: ${SCRIPT_NAME} cannot find sweepfile ${SWEEPFILE_ARG}"
            exit 1
        fi
        shift
    elif [[ "$1" = "-logdir" && $# -ge 2 ]]; then
        shift
        LOGDIR_OPT="yes"
        LOGDIR_ARG=("$@")
        shift
    elif [[ "$1" = "-defaultfile" && $# -ge 2 ]]; then
        shift
        DEFSFILE_OPT="yes"
        DEFSFILE_ARG=("$@")
        if [[ ! -f ${DEFSFILE_ARG} ]]; then
            echo "error: ${SCRIPT_NAME} cannot find defaultfile ${DEFSFILE_ARG}"
            exit 1
        fi
        shift
    elif [[ "$1" = "-help" ]]; then
        echo "usage: ${SCRIPT_NAME} -db sqlitedb -conf configuration [-defaultfile file] [-sweep] [-sweepfile file] [programs...]"
        echo "       -db sqlitedb       : database to use to store/read results from. If this file does not exist,"
        echo "                            an empty database will be created and populated with results"
        echo "       -conf configuration: configuration regarding the checkouts and runtime configurations to compare"
        echo "                            each line of that file should be of one of the following formats:"
        echo "                             - # comment; lines starting with # are ignored"
        echo "                             - <commit hash/ID> <set of CFLAGS where + denotes the addition of the flag and - denotes the removal of a default flag>"
        echo "                            For the second case, an example of a set of CFLAGS would be +DOCR_TICKETLOCK -DCACHE_LINE_SZB to"
        echo "                            enable ticket-locks and remove the CACHE_LINE_SZB define"
        echo "       -sweep             : lookup for a sweep file matching the progname under configSweep/"
        echo "       -sweepfile file    : Use the specified sweep file for all the programs"
        echo "       -defaultfile file  : parse file to find the test's define defaults"
        echo ""
        echo "Defines resolution order:"
        echo "       sweep, sweepfile, defaultfile, defaults.mk"
        exit 0
    else
        # stacking remaining program arguments
        PROGRAMS="${PROGRAMS} $1"
        shift
    fi
done

if [[ -z "$PROGRAMS" ]]; then
    #No programs specified, scan the ocr/ folder
    PROGRAMS=`find ${TEST_FOLDER} -name "*.c"`
    ACC=""
    for prog in `echo $PROGRAMS`; do
        CUR=${prog##ocr/}
        CUR=${CUR%%.c}
        ACC="${ACC} ${CUR}"
    done
    PROGRAMS="${ACC}"
fi

# Extracts the CFLAGS from the OCR compilation process
function extractCFlagsString() {
    local __resultvar=$1
    # This long command line:
    #  - looks for EXPORTED_OCR_CFLAGS in the environment
    #  - extracts that value (cutting the first = and printing the rest of the line)
    #  - strips out the whitespace at the beginning and end of the string
    #  - splits the string into newlines for each option separated by a space
    #  - removes all non -D options (includes, etc.) leaving only the defines
    #  - sorts them
    #  - reassembles in one string
    local result=`env | grep ^EXPORTED_OCR_CFLAGS | cut -d= -f2- | sed 's/^\s\+//; s/\s\+$//; s/ \+/\n/g' | sed '/^-D/ !d' | sort | tr '\n' ' '`
    unset EXPORTED_OCR_CFLAGS
    eval $__resultvar ="$result"
}

# Extracts an array of definitions for
# runs from the EXPORTED_RUNNER_CFGARG, EXPORTED_RUNNER_CFGHASH, EXPORTED_RUNNER_DEF and EXPORTED_RUNNER_CMD
# environment variables
function extractRunString() {
    local __resultvar=$1
    local cfgArgStr=`env | grep ^EXPORTED_RUNNER_CFGARG | cut -d= -f2-`
    local cfgHashStr=`env | grep ^EXPORTED_RUNNER_CFGHASH | cut -d= -f2-`
    local defStr=`env | grep ^EXPORTED_RUNNER_DEF | cut -d= -f2-`
    local cmdStr=`env | grep ^EXPORTED_RUNNER_CMD | cut -d= -f2-`
    IFS='#' read -a cfgArgArray <<< "$cfgArgStr"
    IFS='#' read -a cfgHashArray <<< "$cfgHashStr"
    IFS='#' read -a defArray <<< "$defStr"
    IFS='#' read -a cmdArray <<< "$cmdStr"

    # At this point, we have arrays of all the runs. We just need to
    # match them up and massage them.
    local resultArray=()
    local resultStr=""
    for idx in "${!cfgArgArray[@]}" ; do
        # We can use idx to index into all arrays. They should
        # all be the same size
        resultStr=`echo "${cfgArgArray[$idx]}" | sed 's/^\s\+//; s/\s\+$//; s/ \+/\n/g' | sort | tr '\n' ' '`
        resultStr+='#'
        resultStr+="${cfgHashArray[$idx]}"
        resultStr+='#'
        resultStr+=`echo "${defArray[$idx]}" | sed 's/^\s\+//; s/\s\+$//; s/ \+/\n/g' | sed '/^-D/ !d' | sort | tr '\n' ' '`
        resultStr+='#'
        resultStr+=`echo "${cmdArray[$idx]}" | sed 's/^\s\+//; s/\s\+$//; s/ \+/\n/g' | sort | tr '\n' ' '`
        echo "Got resultStr ${resultStr}"
        resultArray[$idx]="$resultStr"
    done
    unset EXPORTED_RUNNER_CFGARG EXPORTED_RUNNER_CFGHASH EXPORTED_RUNNER_DEF EXPORTED_RUNNER_CMD
    eval $__resultvar=$resultArray
}

# Hash used in the data-base for everything from CFLAGS to
# benchmark configuration
function hashString() {
    echo `sha1sum $1`
}



function matchDefaultSweepFile() {
    local __resultvar=$1
    local prog=$2
    local isFound=""
    local sweepfile=${SWEEP_FOLDER}/${prog}.sweep
    if [[ -f ${sweepfile} ]]; then
        echo ">>> ${prog}: Use defines from sweep file ${sweepfile}"
        isFound=${sweepfile}
    fi
    eval $__resultvar="$isFound"
}

function matchSweepFile() {
    local __resultvar=$1
    local prog=$2
    isFound=""
    if [[ -f ${SWEEPFILE_ARG} ]]; then
        echo ">>> ${prog}: Use defines from sweep file ${SWEEPFILE_ARG}"
        isFound="${SWEEPFILE_ARG}"
    fi
    eval $__resultvar="$isFound"
}

function matchDefaultFile() {
    local __resultvar=$1
    local prog=$2
    # Switch to a map when bash 4.x
    isFound=""
    while read line; do
        array=($line)
        testFile=${array[0]}
        defines=${array[@]:1}
        if [ "${prog}.c" = "$testFile" ]; then
            echo ">>> ${prog}: Use defines from default file ${DEFSFILE_ARG}"
            isFound="$defines"
            break
        fi
    done < ${DEFSFILE_ARG}
    eval $__resultvar="'$isFound'"
}

function run() {
    ARGS=
    if [[ -n "${LOGDIR_ARG}" ]]; then
        ARGS+=" -logdir ${LOGDIR_ARG}"
    fi
    ARGS+=" -exportvars"
    for prog in `echo "$PROGRAMS"`; do
        local found=""
        local runnerArgs=""
        # Try to match a sweep file for the program
        if [ "${NOCLEAN_OPT}" = "yes" ]; then
            runnerArgs="-noclean"
        fi
        if [ "${SWEEP_OPT}" = "yes" ]; then
            matchDefaultSweepFile found $prog
            if [ -n "$found" ]; then
                runnerArgs="-sweepfile $found"
            fi
        fi
        # Try to match a general sweep file
        if [ "$found" = "" -a "${SWEEPFILE_OPT}" = "yes" ]; then
            matchSweepFile found $prog
            if [ -n "$found" ]; then
                runnerArgs="-sweepfile $found"
            fi
        fi
        # Try to match an entry for program in a default define file
        if [ "$found" = "" -a "${DEFSFILE_OPT}" = "yes" ]; then
            matchDefaultFile found $prog
            # to be picked up by the runner script
            if [ -n "$found" ]; then
                export CUSTOM_BOUNDS="$found"
            fi
        fi
        # else rely on default values from defaults.mk
        if [ -z "$found" ]; then
            echo ">>> ${prog}: Use defines from defaults.mk"
        fi
        runlogFilename=${RUNLOG_FILENAME_BASE}-${prog}
        reportFilename=${REPORT_FILENAME_BASE}-${prog}${REPORT_FILENAME_EXT}
        echo "RUNNER_ARGS=${SCRIPT_ROOT}/runner.sh ${ARGS} -nbrun ${NB_RUN} -target ${TARGET_ARG} -runlog ${runlogFilename} -report ${reportFilename} ${runnerArgs} ${prog}"
        . ${SCRIPT_ROOT}/runner.sh ${ARGS} -nbrun ${NB_RUN} -target ${TARGET_ARG} -runlog ${runlogFilename} -report ${reportFilename} ${runnerArgs} ${prog}

        local runResults=()
        extractRunString runResults
        for idx in "${!runResults[$@]}"; do
            echo "Ran test ${runResults[$idx]}"
        done
    done
}

run
