#
# Micro-benchmark driver
#
# TARGET: Single node runs on x86 and x86 distributed
# PLATFORM: Calibrated for a foobar cluster node
#

#
# Environment check
#
if [[ -z "$SCRIPT_ROOT" ]]; then
    echo "SCRIPT_ROOT environment variable is not defined"
    exit 1
fi

unset OCR_CONFIG

. ${SCRIPT_ROOT}/drivers/utils.sh

if [[ ${OCR_TYPE} = "x86" ]]; then
    export CORE_SCALING=${CORE_SCALING-1}
    export OCR_NUM_NODES=${OCR_NUM_NODES-1}
    export NB_RUN=${NB_RUN-3}
else
    # Baseline in distributed is at least two worker threads
    export CORE_SCALING=${CORE_SCALING-2}
    export OCR_NUM_NODES=${OCR_NUM_NODES-1}
    export NB_RUN=${NB_RUN-3}
fi


#
# Build OCR
#
export CFLAGS_USER="${CFLAGS_USER} -DINIT_DEQUE_CAPACITY=2500000 -DGUID_PROVIDER_NB_BUCKETS=2500000"
#buildOcr


#
# Common Definitions
#

# - Generates reports under LOGDIR
# - Generates digest report in current folder
LOGDIR=`mktemp -d logs_foobar.XXXXXX`

# Fan-out for event related tests
VAL_FAN_OUT=1


#
# EDT benchmarking
#
# Need a deque as big as NB_INSTANCES if CORE_SCALING is 1
export CUSTOM_BOUNDS="NB_INSTANCES=2500000"
# Timings: 'crash #674' 1.3 0.7
# SCRIPT_ROOT=./scripts ./scripts/perfDriver.sh -logdir ${LOGDIR} -target ${OCR_TYPE} edtCreateStickySync

SCRIPT_ROOT=./scripts ./scripts/compDriver.sh -logdir ${LOGDIR} -target ${OCR_TYPE} edtCreateFinishSync #edtCreateLatchSync
