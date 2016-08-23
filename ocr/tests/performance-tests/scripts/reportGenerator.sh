#!/bin/bash

#
# Environment check
#
if [[ -z "${SCRIPT_ROOT}" ]]; then
    echo "error: ${0} environment SCRIPT_ROOT is not defined"
    exit 1
fi

if [[ -z "$CORE_SCALING" ]]; then
    echo "error $0: Cannot find environment variable CORE_SCALING";
    exit
fi

SCRIPT_NAME=`basename $0`

#
# Option setup
#

NOCLEAN_OPT="no"
TMPDIR_OPT="no"

#
# Option Parsing and Checking
#
RUNLOG_FILE=""

if [[ -z "$RUNLOG_FILE" ]]; then
    RUNLOG_FILE="${RUNLOG_FILENAME_BASE}"
    #echo "${SCRIPT_NAME}: Use default RUNLOG_FILE: $RUNLOG_FILE";
fi

while [[ $# -gt 0 ]]; do
    # for arg processing debug
    #echo "Processing argument $1"
    if [[ "$1" = "-noclean" ]]; then
        shift
        NOCLEAN_OPT="yes"
    elif [[ "$1" = "-tmpdir" && $# -ge 2 ]]; then
        shift
        TMPDIR_OPT="yes"
        TMPDIR_ARG=("$@")
        shift
    else
        # Remaining is runlog file arguments
        RUNLOG_FILE=$1
        shift
    fi
done

if [[ "${TMPDIR_OPT}" = "no" ]]; then
    TMPDIR=`mktemp -d -p ${SCRIPT_ROOT} tmpdir-report.XXXXXX`
else
    TMPDIR=${TMPDIR_ARG}
fi


function toLower() {
    local  input=$1
    local  __resultvar=$2
    BASH_VER=`echo "$BASH_VERSION" | cut -b1-1`
    if [[ ${BASH_VER} -eq 4 ]]; then
        input=${input,,}
    else
        input=`echo ${input} | tr '[:upper:]' '[:lower:]'`
    fi
    eval $__resultvar="'$input'"
}


function extractMetric() {
    local  METRIC_NAME=$1
    local  CUT_FIELD=$2
    local  ANALYSIS=$3

    local metric_name_low
    toLower ${METRIC_NAME} metric_name_low

    TMP_ALL_METRIC_FILES=""
    for logFile in `ls ${RUNLOG_FILE}*`; do
        logFilename=${logFile##*/}
        TMP_METRIC_FILE=${TMPDIR}/${logFilename}.tmp
        TMP_ALL_METRIC_FILES="${TMP_ALL_METRIC_FILES} ${TMP_METRIC_FILE}"
        grep ${METRIC_NAME} $logFile | cut -d' ' -f${CUT_FIELD}-${CUT_FIELD} > ${TMP_METRIC_FILE}
    done

    # Assemble each run results
    paste ${TMP_ALL_METRIC_FILES} | tr '\t' ' ' > ${TMPDIR}/tmp-all-runlog-metric

    if [[ "$ANALYSIS" == "stddev" ]]; then
        # Aggregate results and compute stddev
        while read line
        do
            ${SCRIPT_ROOT}/utils/stddev.sh "$line"
        done < ${TMPDIR}/tmp-all-runlog-metric > ${TMPDIR}/tmp-agg-results-${metric_name_low}
    elif [[ "$ANALYSIS" == "avg" ]]; then
        # Aggregate results and compute avg
        while read line
        do
            ${SCRIPT_ROOT}/utils/avg.sh "$line"
        done < ${TMPDIR}/tmp-all-runlog-metric > ${TMPDIR}/tmp-agg-results-${metric_name_low}
    else
        cp ${TMPDIR}/tmp-all-runlog-metric ${TMPDIR}/tmp-agg-results-${metric_name_low}
    fi
}


# Utility to delete a file only when NOCLEAN is set to "no"
function deleteFiles() {
    local files=$@
    if [[ "$NOCLEAN_OPT" = "no" ]]; then
        rm -Rf $files
    fi
}

#
# Grep for a metric in each runlog file
#

extractMetric "Throughput" 4 "stddev"
extractMetric "Duration"   9 "stddev"

# Compute speed-up on the average column (first one)
AVG=`cat ${TMPDIR}/tmp-agg-results-throughput | cut -d' ' -f 1-1`
# set that on a single line
AVG=`echo ${AVG} | sed -e "s/\n/ /g"`
# Count how many core configurations
w=`echo "${CORE_SCALING}" | wc -w | sed -e 's/ //g'`

# This is going to put all of the average values into an array
# and iterate over this array in chunk of 'core configurations'
# to invoke the 'speedup' script on each chunk. The result is
# the speed-up computed for each node configuration with the
# baseline being the first core scaling configuration.
SPUP=""
IFS=' ' read -r -a array <<< "$AVG"
let l=0
let u=0

for nodes in `echo "${NODE_SCALING}"`; do
    let u=${l}+${w};
    AVG=`echo "${array[@]:${l}:${w}}"`
    SPUP+=`${SCRIPT_ROOT}/utils/speedup.sh "${AVG}"`
    SPUP+=" "
    let l=${u}
done

# Format speed-up information
echo "$SPUP" | tr ' ' '\n' > ${TMPDIR}/tmp-agg-results-spup

for nodes in `echo "${NODE_SCALING}"`; do
    # Format core-scaling information
    echo "${CORE_SCALING}" | tr ' ' '\n' >> ${TMPDIR}/tmp-core-scaling
done

# Pasting all results and analysis together. Can be used in the future to do
# global processing without having to parse out comments and information.
# final formatting: core-scaling | avg | stddev | count | speed-up | duration | stddev | count
paste ${TMPDIR}/tmp-core-scaling ${TMPDIR}/tmp-agg-results-throughput ${TMPDIR}/tmp-agg-results-spup ${TMPDIR}/tmp-agg-results-duration | column -t > ${TMPDIR}/tmp-results

# This is going over the perf dump and chunk it up to inject text
let l=0

IFS=' ' read -r -a arrayNodes <<< "${NODE_SCALING}"
IFS=$'\n'
let i=0
for lines in `cat "${TMPDIR}/tmp-results"`; do
    echo "#N=${arrayNodes[$i]} Nodes Scaling Results"
    echo "$lines"
    let i=$i+1
done

# delete left-over temporary file
deleteFiles ${TMPDIR}
deleteFiles ${TMP_ALL_METRIC_FILES}
