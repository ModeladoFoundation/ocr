#!/bin/bash

SCRIPT_NAME=${0##*/}

#
# Environment check
#
if [[ -z "${SCRIPT_ROOT}" ]]; then
    echo "error: ${SCRIPT_NAME} environment SCRIPT_ROOT is not defined"
    exit 1
fi

if [[ -z "${OCR_INSTALL}" ]]; then
    # Check if this an OCR repo
    export OCR_INSTALL="${SCRIPT_ROOT}/../../../install"
    if [[ ! -d ${OCR_INSTALL} ]]; then
        echo "OCR_INSTALL environment variable is not defined and cannot be deduced"
        exit 1
    fi
fi

#
# OCR setup and run configuration
#

export CORE_SCALING=${CORE_SCALING-"1 2 4 8 16"}

export NODE_SCALING=${NODE_SCALING-"1"}

#
# OCR test setup
#

PROG_ARG=""
LOGDIR_OPT="no"
LOGDIR_ARG=""
NBRUN_OPT="no"
NBRUN_ARG=""
RUNLOG_OPT="no"
RUNLOG_ARG=""
REPORT_OPT="no"
REPORT_ARG=""
SWEEPFILE_OPT="no"
SWEEPFILE_ARG=""
TARGET_ARG="x86"
TMPDIR_ARG=""
NOCLEAN_OPT="no"
EXPORTVARS_OPT="no"

# Default options are for micro-benchmarks
RUNNER_TYPE=${RUNNER_TYPE-"MicroBenchmark"}
OCR_MAKEFILE=${OCR_MAKEFILE-"Makefile"}

#
# Option Parsing and Checking
#

while [[ $# -gt 0 ]]; do
    # for arg processing debug
    #echo "Processing argument $1"
    if [[ "$1" = "-sweepfile" && $# -ge 2 ]]; then
        shift
        SWEEPFILE_OPT="yes"
        SWEEPFILE_ARG=("$@")
        shift
        if [[ ! -f ${SWEEPFILE_ARG} ]]; then
            echo "error: ${SCRIPT_NAME} cannot find sweepfile ${SWEEPFILE_ARG}"
            exit 1
        fi
    elif [[ "$1" = "-logdir" && $# -ge 2 ]]; then
        shift
        LOGDIR_OPT="yes"
        LOGDIR_ARG=("$@")
        echo "LOGDIR_ARG=$LOGDIR_ARG"
        shift
    elif [[ "$1" = "-nbrun" && $# -ge 2 ]]; then
        shift
        NBRUN_OPT="yes"
        NBRUN_ARG=("$@")
        shift
    elif [[ "$1" = "-runlog" && $# -ge 2 ]]; then
        shift
        RUNLOG_OPT="yes"
        RUNLOG_ARG=("$@")
        shift
    elif [[ "$1" = "-report" && $# -ge 2 ]]; then
        shift
        REPORT_OPT="yes"
        REPORT_ARG=("$@")
        shift
    elif [[ "$1" = "-target" && $# -ge 2 ]]; then
        shift
        TARGET_ARG=("$@")
        shift
    elif [[ "$1" = "-noclean" ]]; then
        shift
        NOCLEAN_OPT="yes"
    elif [[ "$1" = "-exportvars" ]]; then
        shift
        EXPORTVARS_OPT="yes"
    elif [[ "$1" = "-help" ]]; then
        echo "usage: ${SCRIPT_NAME} [-sweepfile file] program"
        echo "       -sweepfile file    : Use the specified sweep file for the program"
        echo "       -logdir path       : Log file output folder"
        echo "       -nbrun  integer    : Number of runs per program"
        echo "       -runlog name       : Naming for run logs"
        echo "       -report name       : Naming for run reports"
        echo "       -target name       : Target to run on (x86|x86-mpi|x86-gasnet)"
        echo "       -noclean           : Do not cleanup temporary files"
        echo "Environment variables:"
        echo "       - CUSTOM_BOUNDS: defines to use when compiling the program"
        echo "Defines resolution order:"
        echo "       - sweepfile, CUSTOM_BOUNDS, defaults.mk"
        exit 0
    else
        # Remaining program argument
        PROG_ARG=$1
        shift
    fi
done

if [[ -z "$PROG_ARG" ]]; then
    echo "error: ${SCRIPT_NAME} is missing program argument"
    exit 1
fi

if [[ "${LOGDIR_OPT}" != "yes" ]]; then
    LOGDIR=`mktemp -d rundir.XXXXXX`
else
    LOGDIR=${LOGDIR_ARG}
fi

# If we need to export what we ran, clear the variables
if [[ "${EXPORTVARS_OPT}" = "yes" ]]; then
    export EXPORTED_RUNNER_CFGARG="" EXPORTED_RUNNER_CFGHASH="" EXPORTED_RUNNER_DEF="" EXPORTED_RUNNER_CMD=""
fi

echo "Results will be located under ${LOGDIR}"

if [[ ! -e ${LOGDIR_ARG} ]]; then
    echo "${SCRIPT_NAME} creating log dir ${LOGDIR_ARG}"
    mkdir -p ${LOGDIR}
fi

if [[ -z "$RUNLOG_ARG" ]]; then
    RUNLOG_ARG="${LOGDIR}/${RUNLOG_FILENAME_BASE}-${PROG_ARG}"
else
    RUNLOG_ARG="${LOGDIR}/${RUNLOG_ARG}"
fi

if [[ -z "$REPORT_ARG" ]]; then
    REPORT_ARG="${LOGDIR}/${REPORT_FILENAME_BASE}-${PROG_ARG}"
else
    REPORT_ARG="${LOGDIR}/${REPORT_ARG}"
fi

# Utility to delete a file only when NOCLEAN is set to "no"
function deleteFiles() {
    local files=$@
    if [[ "$NOCLEAN_OPT" = "no" ]]; then
        rm -Rf $files
    fi
}

# Setup default target-dependent env variable for the config
# file generator unless they are already defined in the environment
#
function defaultConfigTarget() {
    local target=$1
    echo "defaultConfigTarget $target"
    case "$target" in
        x86)
            export CFGARG_GUID=${CFGARG_GUID-"PTR"}
            export CFGARG_PLATFORM=${CFGARG_PLATFORM-"X86"}
            export CFGARG_TARGET=${CFGARG_TARGET-"x86"}
            export CFGARG_BINDING=${CFGARG_BINDING-"seq"}
            export CFGARG_ALLOC=${CFGARG_ALLOC-"32"}
            export CFGARG_ALLOCTYPE=${CFGARG_ALLOCTYPE-"mallocproxy"}
        ;;
        x86-mpi)
            export CFGARG_GUID=${CFGARG_GUID-"COUNTED_MAP"}
            export CFGARG_PLATFORM=${CFGARG_PLATFORM-"X86"}
            export CFGARG_TARGET=${CFGARG_TARGET-"mpi"}
            export CFGARG_BINDING=${CFGARG_BINDING-"seq"}
            export CFGARG_ALLOC=${CFGARG_ALLOC-"32"}
            export CFGARG_ALLOCTYPE=${CFGARG_ALLOCTYPE-"mallocproxy"}
        ;;
        x86-gasnet)
            export CFGARG_GUID=${CFGARG_GUID-"COUNTED_MAP"}
            export CFGARG_PLATFORM=${CFGARG_PLATFORM-"X86"}
            export CFGARG_TARGET=${CFGARG_TARGET-"gasnet"}
            export CFGARG_BINDING=${CFGARG_BINDING-"seq"}
            export CFGARG_ALLOC=${CFGARG_ALLOC-"32"}
            export CFGARG_ALLOCTYPE=${CFGARG_ALLOCTYPE-"mallocproxy"}
        ;;
        *)
            echo $"Unknown target $target"
            exit 1
    esac
}

function generateMachineFile {
    local  __resultvar=$1

    VALUE="$PWD/mf${nodes}"

    if [[ ${OCRRUN_OPT_ENVKIND} == "CLE" ]]; then
        cat $PBS_NODEFILE | sort | uniq | head -n ${nodes} > ${VALUE}
        echo "Generated node file: ${OCR_NODEFILE} for ${OCR_NUM_NODES} number of nodes"
    elif [[ ${OCRRUN_OPT_ENVKIND} == "SLURM" ]]; then
        #Just rely on srun to do the mapping
        echo "No node file has been generated - Slurm's srun does the mapping"
        VALUE=""
    else
        # Check if user provided a node file template
        # It should contain at least the number of nodes requested. If there are more, the list is truncated.
        if [[ -n "${OCRRUN_OPT_TPL_NODEFILE}" ]]; then
            if [[ ! -f "${OCRRUN_OPT_TPL_NODEFILE}" ]]; then
                echo "Error: USER envkind requires OCRRUN_OPT_TPL_NODEFILE does not point to a file"
                VALUE=""
            fi
            head -n ${nodes} "${OCRRUN_OPT_TPL_NODEFILE}" > $VALUE
            local nb=`cat $VALUE | wc -l`;
            if [[ "$nb" != "${nodes}" ]]; then
                nb=`cat ${OCRRUN_OPT_TPL_NODEFILE} | wc -l`
                echo "Error: not enough nodes (${nodes}/${nb}) declared in ${OCRRUN_OPT_TPL_NODEFILE}"
                VALUE=""
            else
                echo "Generated node file: ${VALUE} for ${OCR_NUM_NODES} number of nodes from ${OCRRUN_OPT_TPL_NODEFILE}"
            fi
        else
            VALUE=""
            echo "No node file has been generated - default when OCRRUN_OPT_ENVKIND is not specified"
        fi
    fi

    eval $__resultvar="'$VALUE'"
}

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

# Generates an OCR configuration file
#
# Any CFGARG_* env variables set are transformed into program
# arguments to the configuration generator, else the generator's
# default values are used.
function generateCfgFile {
    # Read all CFGARG_ environment variables and transform
    # them into config generator's arguments
    for cfgarg in `env | grep ^CFGARG_`; do
        #Extract argument name and value
        parsed=(${cfgarg//=/ })
        argNameU=${parsed[0]#CFGARG_}
        toLower ${argNameU} argNameL
        argValue=${parsed[1]}
        #Append to generator argument list
        arg="--${argNameL} ${argValue}"
        CFG_ARGS+="$arg "
    done
    if [[ -z "${CFGARG_OUTPUT}" ]]; then
        echo "error: ${SCRIPT_NAME} The environment variable CFGARG_OUTPUT is not set"
        exit 1
    fi

    # Invoke the cfg file generator
    ${OCR_INSTALL}/share/ocr/scripts/Configs/config-generator.py --remove-destination ${CFG_ARGS}
}

#
# Compile and Run Function
#

function runMicroBenchmark {
    local  __resultvar=$1

    # Compile the program with provided defines
    echo "Compiling for OCR ${prog} ${runInfo}"
    echo "${progDefines} ${runInfo} make -f ${OCR_MAKEFILE} benchmark build/${prog} PROG=ocr/${prog}.c"
    eval ${progDefines} ${runInfo} make -f ${OCR_MAKEFILE} benchmark build/${prog} PROG=ocr/${prog}.c

    # Run the program with the appropriate OCR cfg file
    if [[ -z "${RUN_HPCTOOLKIT}" ]]; then
        echo "Run with OCR ${prog} ${runInfo}"
        make -f ${OCR_MAKEFILE} OCR_CONFIG=${PWD}/${CFGARG_OUTPUT} run build/${prog}
        RES=$?
    else
        echo "Run HPCToolkit with OCR ${prog} ${runInfo}"
        make -f ${OCR_MAKEFILE} OCR_CONFIG=${PWD}/${CFGARG_OUTPUT} hpcrun build/${prog}
        RES=$?
    fi
    eval $__resultvar="'$RES'"
}

function runApplication {
    local  __resultvar=$1

    # Compile the program with provided defines
    echo "Compiling OCR Application ${prog} ${runInfo}"
    echo "${progDefines} ${runInfo} make -f ${OCR_MAKEFILE}"
    eval ${runInfo} make -f ${OCR_MAKEFILE}

    # Run the program with the appropriate OCR cfg file
    echo "Run OCR Application ${prog} ${runInfo}"
    eval ${progDefines} make -f ${OCR_MAKEFILE} OCR_CONFIG=${PWD}/${CFGARG_OUTPUT} run
    RES=$?
    eval $__resultvar="'$RES'"
}


function invokeRunnerWorkloadArguments {
    local  __resultvar=$1
    type runnerWorkloadArguments 2>/dev/null | grep "is a function"
    RES=$?
    if [[ $RES -eq 0 ]]; then
        echo "Use user-defined runnerWorkloadArguments function to generate WORKLOAD_ARGS"
        runnerWorkloadArguments
    else
        echo "Use WORKLOAD_ARGS=${WORKLOAD_ARGS}"
    fi
    eval $__resultvar="'$WORKLOAD_ARGS'"
}

function scalingTest {
    prog=$1
    progDefines="$2"
    export HAS_NODEFILE="${OCR_NODEFILE}"

    for nodes in `echo "${NODE_SCALING}"`; do
        for cores in `echo "${CORE_SCALING}"`; do
            export nodes_all=${nodes}
            export pes_per_node=${cores}
            export pes_all=`echo "${cores}*${nodes}" | bc`
            if [[ "${OCR_TYPE}" == "x86-mpi" ]]; then
                export pes_comp=`echo "(${cores}-1)*${nodes}" | bc`
            else
                export pes_comp=${pes_all}
            fi
            runInfo="NB_WORKERS=${cores} NB_NODES=${nodes} nodes_all=${nodes_all} pes_per_node=${pes_per_node} pes_all=${pes_all}"
            echo "======== $runInfo ======== "

            export OCR_NUM_NODES=${nodes}

            if [[ -z "${HAS_NODEFILE}" ]]; then
                export OCR_NODEFILE=
                # Generate the machine file list, can be none
                generateMachineFile OCR_NODEFILE
                if [[ "${OCR_NODEFILE}" == "" ]]; then
                    unset OCR_NODEFILE
                fi
            fi

            # Generate the OCR CFG file
            export CFGARG_THREADS=${cores}
            export CFGARG_OUTPUT="${LOGDIR}/${prog}-${cores}c.cfg"
            generateCfgFile

            # Generate workload arguments if there is a user-provided function
            invokeRunnerWorkloadArguments WORKLOAD_ARGS

            if [[ "${RUNNER_TYPE}" == "Application" ]]; then
                runApplication RES
            else
                # Default is micro-benchmark
                runMicroBenchmark RES
            fi

            if [[ ! -f ${CFGARG_OUTPUT} ]]; then
                echo "error: ${SCRIPT_NAME} Cannot find generated OCR config file ${CFGARG_OUTPUT}"
                exit 1
            fi

            if [[ $RES -ne 0 ]]; then
                if [[ "${TARGET_ARG}" != "gasnet" ]]; then
                    echo "error: run failed !"
                else
                    echo "Warning, gasnet returns an error"
                fi
                exit 1
            fi
            # Everything went fine, delete the generated cfg file
            # unless instructed otherwise by NOCLEAN_OPT
            deleteFiles ${CFGARG_OUTPUT}

            unset OCR_NUM_NODES
            unset OCR_NODEFILE
        done
    done
    unset HAS_NODEFILE
}

function runTest() {
    local prog=$1
    local nbRun=$2
    local runlog=$3
    local report=$4
    local defines=$5
    let i=0;
    #TODO if this is part of a sweep we should mangle the name
    # A 'run' here is the full sweep across requested nodes/cores

    # System information
    env > ${LOGDIR}/info_env_all
    lscpu > ${LOGDIR}/info_lscpu
    w > ${LOGDIR}/info_machine_load

    # OCR specific information
    cat ${LOGDIR}/info_env_all | grep -e "OCR" -e "CFGARG_" -e "SCALING" > ${LOGDIR}/info_ocr_env
    FULLLOGDIR=$PWD/${LOGDIR}
    cd ${OCR_INSTALL}
    # test if we have an actual GIT repo checkout
    git log -n 1 > /dev/null
    RES=$?
    if [[ $RES -eq 0 ]]; then
        echo "### Repo HEAD ###" > ${FULLLOGDIR}/info_ocr_repo
        git log -n 1 2>/dev/null >> ${FULLLOGDIR}/info_ocr_repo
        echo "### Repot branch ###" >> ${FULLLOGDIR}/info_ocr_repo
        git branch 2>/dev/null >> ${FULLLOGDIR}/info_ocr_repo
        echo "### Repo status ###" >> ${FULLLOGDIR}/info_ocr_repo
        git status 2>/dev/null >> ${FULLLOGDIR}/info_ocr_repo
        git diff 2>/dev/null > ${FULLLOGDIR}/info_ocr_repo_diff
    else
        echo "error: OCR_INSTALL does not point to an OCR checkout from a GIT repository" > ${FULLLOGDIR}/info_ocr_repo
        echo "error: OCR_INSTALL=${OCR_INSTALL}" >> ${FULLLOGDIR}/info_ocr_repo
    fi
    cd -
    START_DATE=`date`
    if [[ "${EXPORTVARS_OPT}" == "yes" ]]; then
        # Here we are going to run a test so we output its configuration
        # We pretend to generate a configuration file for only one core
        # (to standardize it)
        export OCR_NUM_NODES=1
        export CFGARG_THREADS=1
        export CFGARG_OUTPUT=`mktemp ${LOGDIR}/tempconfig.XXX`
        generateCfgFile

        # At this point, we can extract the CFGARG_ variables as well
        # as hash the configuration file we just generated
        local temp
        temp=`env | grep ^CFGARG_ | grep -v ^CFGARG_OUTPUT | tr '\n' ' '`
        if [[ -z ${EXPORTED_RUNNER_CFGARG} ]]; then
            if [[ -z $temp ]]; then
                temp='#'
            fi
            export EXPORTED_RUNNER_CFGARG="$temp"
        else
            export EXPORTED_RUNNER_CFGARG= "${EXPORTED_RUNNER_CFGARG}#$temp"
        fi

        temp=`sha1sum ${CFGARG_OUTPUT} | cut -f1 -d' '`
        if [[ -z ${EXPORTED_RUNNER_CFGHASH} ]]; then
            export EXPORTED_RUNNER_CFGHASH="$temp"
        else
            export EXPORTED_RUNNER_CFGHASH= "${EXPORTED_RUNNER_CFGHASH}#$temp"
        fi
        if [[ -z ${EXPORTED_RUNNER_DEF} ]]; then
            export EXPORTED_RUNNER_DEF="#"
        else
            export EXPORTED_RUNNER_DEF= "${EXPORTED_RUNNER_CFGHASH}#"
        fi
        if [[ -z ${EXPORTED_RUNNER_CMD} ]]; then
            export EXPORTED_RUNNER_CMD="#"
        else
            export EXPORTED_RUNNER_CMD= "${EXPORTED_RUNNER_CFGHASH}#"
        fi
        rm -rf ${CFGARG_OUTPUT}
    fi

    while (( $i < ${nbRun} )); do
        scalingTest ${prog} "${defines}" | tee ${runlog}-$i
        let i=$i+1;
    done

    # WARNING WARNING WARNING
    # Whenever an additional porint line is added here, the post-processing
    # scripts must be updated to correctly strip out those additional lines.
    echo "start_date: ${START_DATE}" >> ${report}
    if [[ $defines = "" ]]; then
        echo "defines_set: defaults.mk" >> ${report}
    else
        echo "defines: ${defines}" >> ${report}
    fi
    echo "== Scaling Report ==" >> ${report}

    # Generate a report based on any filename matching ${RUNLOG_FILE}*
    reportGenOpt=""
    if [[ "$NOCLEAN_OPT" = "yes" ]]; then
        reportGenOpt="-noclean"
    fi
    echo "${SCRIPT_ROOT}/reportGenerator.sh ${reportGenOpt} ${runlog} >> ${report}"
    ${SCRIPT_ROOT}/reportGenerator.sh ${reportGenOpt} ${runlog} >> ${report}
}

echo "Running core scaling for ${PROG_ARG}"

# Setting up env variables for the cfg file generator
defaultConfigTarget ${TARGET_ARG}

if [[ "$SWEEPFILE_OPT" = "yes" ]]; then
    # use sweep file
    echo "${SCRIPT_NAME} Loading sweep configuration: $SWEEPFILE_ARG"
    let count=0
    # Used the following approach to read the sweepfile but it
    # seems that invoking mpirun somehow breaks the loop and only
    # the first line is read.
    # while read -r defines
    # done < "${SWEEPFILE_ARG}"
    readarray defineArray < "${SWEEPFILE_ARG}"
    for defines in "${defineArray[@]}"
    do
        tmp="$defines"
        defines=`echo $tmp | tr '\n' ' '`
        echo "${0%%.c} Executing sweep configuration: $defines"
        runlogFilename=${RUNLOG_ARG}-sweep${count}
        reportFilename=${REPORT_ARG}-sweep${count}
        runTest $PROG_ARG $NBRUN_ARG $runlogFilename $reportFilename "${defines}"
        let count=${count}+1
    done
elif [[ -n "$CUSTOM_BOUNDS" ]]; then
    # use provided defines
    echo "${SCRIPT_NAME} Use CUSTOM_BOUNDS: ${CUSTOM_BOUNDS}"
    runTest $PROG_ARG $NBRUN_ARG $RUNLOG_ARG $REPORT_ARG "${CUSTOM_BOUNDS}"
else
    # rely on defaults.mk
    runTest $PROG_ARG $NBRUN_ARG $RUNLOG_ARG $REPORT_ARG ""
fi

deleteFiles ${RUNLOG_ARG}*
deleteFiles ${SCRIPT_ROOT}/tmp/*

