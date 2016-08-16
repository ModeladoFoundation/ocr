#
# Disperse EDT Scaling Experiment driver
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

# Inherited by runProg
if [[ -z "${LOGDIR}" ]]; then
    export LOGDIR=`mktemp -d logs_scalingDisperse.XXXXX`
else
    mkdir -p ${LOGDIR}
fi

function runAll() {
    export NAME=evtToEdtAddDepSticky0RR
    runProg

    export NAME=evtToEdtAddDepSticky0LL
    runProg

    export NAME=evtToEdtAddDepSticky0LR
    runProg

    export NAME=evtToEdtAddDepSticky0RL
    runProg

    echo "${SCRIPT_ROOT}/plotCoreScalingMultiRun.sh ${LOGDIR}/report*${EXT}"
    ${SCRIPT_ROOT}/plotCoreScalingMultiRun.sh ${LOGDIR}/report*${EXT}
    mv comparison-graph.svg ${LOGDIR}/comparison-${NAME_EXP}${EXT}.svg
}

#
# Common Driver Arguments
#
export CFGARG_BINDING=${CFGARG_BINDING-"seq"}
export NB_RUN=${NB_RUN-3}
export NODE_SCALING=${NODE_SCALING-"1"}

if [[ ${OCR_TYPE} == "x86" ]]; then
    export CORE_SCALING=${CORE_SCALING-"1 2 4 8 16"}
else
    # For distributed MPI or GASNET need at least two workers
    export CORE_SCALING=${CORE_SCALING-"2 4 8 16"}
fi

#
# Common OCR Build arguments
#
export CFLAGS_USER="${CFLAGS_USER}"


#
# Common Benchmark Arguments
#

# export DEPTH=21
# export NB_INSTANCES=2097152


#
# Run section
#
export NAME_EXP="compAddDep"

# Same with assertions off
export NO_DEBUG=yes
buildOcr
export EXT="-${OCR_TYPE}.std"
runAll

export NO_DEBUG=yes
export CFLAGS_USER="-DNEWREG"
buildOcr
export EXT="-${OCR_TYPE}.newreg"
runAll

export NO_DEBUG=yes
export CFLAGS_USER="-DNEWREG_MODE"
buildOcr
export EXT="-${OCR_TYPE}.newreg_mode"
runAll

${SCRIPT_ROOT}/plotCoreScalingMultiRun.sh ${LOGDIR}/report-*
mv comparison-graph.svg comparison-${OCR_TYPE}-${NAME_EXP}-all.svg
