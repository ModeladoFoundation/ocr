#!/bin/bash

##########################################################################################
#
#  This script is used to run test suite for Scheduler and/or PD modification to ocr.
#  It is current programmed to generate the 3 test cases.
#
#  Follow these steps to use it:
#  1. copy the script to the application directory
#  2. change the following if needed
#     - block number: BLOCKCOUNT=(n)            ; default to 4
#     - add msgstats run: MSGSTATS_CFG=[1|0]    ; default to 1
#     - add energy run: ENERGYSTATS_CFG=[1|0]   ; default to 1
#     - enable correct workload by uncomment the line or add your own line if needed.
#       multigen has default workload.
#     - make sure the correct configuration template is used. The config-template is used for
#       for multigen, fib, and fft.  RSbench and CoMD uses the config-newlib-template which
#       has the xe stacks in ipm.
#  3. run the script, it will generates
#     - a run script to run the entire test suite: jobScript_[n]d.sh
#     - configuration files for msgStats and/or energyStats: config_msgStats.cfg & config_energy.cfg
#     - post processing scripts for msgStats and/or energyStats: jobScript_4d_postMsgStats.sh & jobScript_4d_postEnergyStats.sh
#     - all the install directories
#     - run_commands that contains one line command to run the tests in the backgroud
#  4. "cat run_commands" will show something like:
#               ./jobScript_4d.sh &> jobScript_4d.out &
#  5. run this line will direct all console output to jobScript_4d.out
#
##########################################################################################
source $APPS_ROOT/tools/execution_tools/aux_bash_functions

BLOCKCOUNTS=(4)
MSGSTATS_CFG=1
ENERGYSTATS_CFG=1

# workload for RSBench if BLOCKCOUNTS = 1
#WORKLOAD='-d -s small -t 8 -l 12801'

# workload for RSBench if BLOCKCOUNTS = 4
#WORKLOAD='-d -s small -t 32 -l 12801'

# workload for CoMD
WORKLOAD="-x 8 -y 8 -z 8 -i 2 -j 2 -k 2 -N 2 -n 1"

# make sure the configuration template exists before run this script
CFGFILE="~/apps/apps/tg-configs/config-newlib-template.cfg"
#CFGFILE="~/apps/apps/tg-configs/config-template.cfg"

# for CoMD and RSBench uncomment the next line
NEWLIB_CONFIG=1

function enableNewlibforXE()
{
    if [ ! -z "$NEWLIB_CONFIG" ]; then
        MOD_CMD="sed -i '/#define ENABLE_NEWLIB_SCAFFOLD_TG/c\#define ENABLE_NEWLIB_SCAFFOLD_TG' ~/ocr/ocr/build/tg-xe/ocr-config.h"
    else
        MOD_CMD="sed -i '/#define ENABLE_NEWLIB_SCAFFOLD_TG/c\\/\/#define ENABLE_NEWLIB_SCAFFOLD_TG' ~/ocr/ocr/build/tg-xe/ocr-config.h"
    fi
    echo $MOD_CMD >> ${jfile}.sh
}

function enableSharedPolicyDomain()
{
    local x=$1

    if [ $x -eq 0 ];	then
	MOD_CMD="sed -i '/CFLAGS += -DOCR_SHARED_XE_POLICY_DOMAIN/c\# CFLAGS += -DOCR_SHARED_XE_POLICY_DOMAIN' ~/ocr/ocr/build/common.mk"
    else
        MOD_CMD="sed -i '/CFLAGS += -DOCR_SHARED_XE_POLICY_DOMAIN/c\CFLAGS += -DOCR_SHARED_XE_POLICY_DOMAIN' ~/ocr/ocr/build/common.mk"
    fi
    echo $MOD_CMD >> ${jfile}.sh
}

function disableL1UserAlloc()
{
    local x=$1

    if [ $x -eq 0 ];	then
	MOD_CMD="sed -i '/CFLAGS += -DOCR_DISABLE_USER_L1_ALLOC/c\# CFLAGS += -DOCR_DISABLE_USER_L1_ALLOC' ~/ocr/ocr/build/common.mk"
    else
        MOD_CMD="sed -i '/CFLAGS += -DOCR_DISABLE_USER_L1_ALLOC/c\CFLAGS += -DOCR_DISABLE_USER_L1_ALLOC' ~/ocr/ocr/build/common.mk"
    fi
    echo $MOD_CMD >> ${jfile}.sh
}

function disableL1RuntimeAlloc()
{
   local x=$1

   if [ $x -eq 0 ];	then
        MOD_CMD="sed -i '/CFLAGS += -DOCR_DISABLE_RUNTIME_L1_ALLOC/c\# CFLAGS += -DOCR_DISABLE_RUNTIME_L1_ALLOC' ~/ocr/ocr/build/common.mk"
    else
        MOD_CMD="sed -i '/CFLAGS += -DOCR_DISABLE_RUNTIME_L1_ALLOC/c\CFLAGS += -DOCR_DISABLE_RUNTIME_L1_ALLOC' ~/ocr/ocr/build/common.mk"
    fi
    echo $MOD_CMD >> ${jfile}.sh
}

function enableL2Alloc()
{
    local x=$1

    if [ $x -eq 0 ];	then
	MOD_CMD="sed -i '/CFLAGS += -DOCR_ENABLE_XE_L2_ALLOC/c\# CFLAGS += -DOCR_ENABLE_XE_L2_ALLOC' ~/ocr/ocr/build/common.mk"
    else
        MOD_CMD="sed -i '/CFLAGS += -DOCR_ENABLE_XE_L2_ALLOC/c\CFLAGS += -DOCR_ENABLE_XE_L2_ALLOC' ~/ocr/ocr/build/common.mk"
    fi
    echo $MOD_CMD >> ${jfile}.sh
}

function enableXeMultiGet()
{
    local x=$1

    if [ $x -eq 0 ];	then
	MOD_CMD="sed -i '/CFLAGS += -DOCR_ENABLE_XE_GET_MULTI_WORK/c\# CFLAGS += -DOCR_ENABLE_XE_GET_MULTI_WORK' ~/ocr/ocr/build/common.mk"
    else
        MOD_CMD="sed -i '/CFLAGS += -DOCR_ENABLE_XE_GET_MULTI_WORK/c\CFLAGS += -DOCR_ENABLE_XE_GET_MULTI_WORK' ~/ocr/ocr/build/common.mk"
    fi
    echo $MOD_CMD >> ${jfile}.sh
}

function enableCeMultiGet()
{
    local x=$1

    if [ $x -eq 0 ];	then
	MOD_CMD="sed -i '/CFLAGS += -DOCR_ENABLE_CE_GET_MULTI_WORK/c\# CFLAGS += -DOCR_ENABLE_CE_GET_MULTI_WORK' ~/ocr/ocr/build/common.mk"
    else
        MOD_CMD="sed -i '/CFLAGS += -DOCR_ENABLE_CE_GET_MULTI_WORK/c\CFLAGS += -DOCR_ENABLE_CE_GET_MULTI_WORK' ~/ocr/ocr/build/common.mk"
    fi
    echo $MOD_CMD >> ${jfile}.sh
}

function enableCeSpawningQueue()
{
    local x=$1

    if [ $x -eq 0 ];	then
	MOD_CMD="sed -i '/CFLAGS += -DOCR_ENABLE_SCHEDULER_SPAWN_QUEUE/c\# CFLAGS += -DOCR_ENABLE_SCHEDULER_SPAWN_QUEUE' ~/ocr/ocr/build/common.mk"
    else
        MOD_CMD="sed -i '/CFLAGS += -DOCR_ENABLE_SCHEDULER_SPAWN_QUEUE/c\CFLAGS += -DOCR_ENABLE_SCHEDULER_SPAWN_QUEUE' ~/ocr/ocr/build/common.mk"
    fi
    echo $MOD_CMD >> ${jfile}.sh
    if [ $x -eq 0 ];	then
	MOD_CMD="sed -i '/CFLAGS += -DENABLE_SPAWNING_HINT/c\#CFLAGS += -DENABLE_SPAWNING_HINT' Makefile.tg"
    else
        MOD_CMD="sed -i '/CFLAGS += -DENABLE_SPAWNING_HINT/c\CFLAGS += -DENABLE_SPAWNING_HINT' Makefile.tg"
    fi
    echo $MOD_CMD >> ${jfile}.sh
}

function setLocalCacheMax()
{
    local l1max=$1
    local l2max=$2

    echo "echo '==========================================================' " >> ${jfile}.sh
    echo "echo ' Setting max L1 and L2 cache                          ' " >> ${jfile}.sh
    echo "echo ' - L1 Max: ${l1max}K                                  ' " >> ${jfile}.sh
    echo "echo ' - L2 Max: ${l2max}K                                  ' " >> ${jfile}.sh
    echo "echo '==========================================================' " >> ${jfile}.sh

    MOD_CMD="sed -i '/u64 AllocXeL1MaxSize/c\u64 AllocXeL1MaxSize = ${l1max}*1024;' ~/ocr/ocr/src/policy-domain/xe/xe-policy.c"
    echo $MOD_CMD >> ${jfile}.sh

    MOD_CMD="sed -i '/u64 AllocXeL2MaxSize/c\u64 AllocXeL2MaxSize = ${l2max}*1024;' ~/ocr/ocr/src/policy-domain/xe/xe-policy.c"
    echo $MOD_CMD >> ${jfile}.sh
}


function addScript()
{
    local case=$1
    local pd=$2
    local l2=$3
    local xe=$4
    local ce=$5
    local sq=$6

    echo "echo '==========================================================' " >> ${jfile}.sh
    echo "echo ' Setting configuration: Case ${case} in ${jfile}.sh       ' " >> ${jfile}.sh
    echo "echo ' -OCR_SHARED_XE_POLICY_DOMMAIN: ${pd}                 ' " >> ${jfile}.sh
    echo "echo ' -OCR_ENABLE_XE_L2_ALLOC: ${l2}                       ' " >> ${jfile}.sh
    echo "echo ' -OCR_ENABLE_XE_GET_MULTI_WORK: ${xe}                 ' " >> ${jfile}.sh
    echo "echo ' -OCR_ENABLE_CE_GET_MULTI_WORK: ${ce}                 ' " >> ${jfile}.sh
    echo "echo ' -OCR_ENABLE_SCHEDULER_SPAWN_QUEUE: ${sq}             ' " >> ${jfile}.sh
    echo "echo '==========================================================' " >> ${jfile}.sh

    enableNewlibforXE
    enableSharedPolicyDomain $pd
    enableL2Alloc $l2
    enableXeMultiGet $xe
    enableCeMultiGet $ce
    enableCeSpawningQueue $sq

    (
        echo date >> ${jfile}.sh
	if [ $MSGSTATS_CFG -eq 1 ]; then

	    echo "echo '===============================================' " >> ${jfile}.sh
	    echo "echo ' Running MsgStats test	                       ' " >> ${jfile}.sh
	    echo "echo '===============================================' " >> ${jfile}.sh
            jobHeader="c${case}_msgstats_pd_${pd}_xe_${xe}_ce_${ce}_sq_${sq}_${BLOCKCOUNT}b"
            winstall="install_$jobHeader"

	    mkdir -p ./${winstall}/tg/logs

            CLEAN_CMD="WORKLOAD_INSTALL_ROOT=./${winstall} make -f Makefile.tg clean uninstall "
            echo $CLEAN_CMD >> ${jfile}.sh

            CMD_OPT="WORKLOAD_INSTALL_ROOT=./${winstall}"
	    if [ ! -z "$WORKLOAD" ]; then
		CMD_OPT="${CMD_OPT} WORKLOAD_ARGS='${WORKLOAD}'"
	    fi
	    if [ ! -z "$CFGFILE" ]; then
		CMD_OPT="${CMD_OPT} TG_CONFIG_TEMPLATE_CFG=config_msgStats.cfg"
	    fi

            RUN_CMD="${CMD_OPT} make -f Makefile.tg run"
            echo $RUN_CMD >> ${jfile}.sh

            echo "cp ~/ocr/ocr/build/tg-xe/ocr-config.h ./${winstall}/tg/ocr-config.h.sav" >> ${jfile}.sh
            echo "cp ~/ocr/ocr/build/common.mk ./${winstall}/tg/common.mk.sav" >> ${jfile}.sh
            echo "cp Makefile.tg ./${winstall}/tg/Makefile.tg.sav" >> ${jfile}.sh

	    POST_CMD="~/ocr/ocr/scripts/MsgStats/msgstats.sh ./${winstall}/tg/logs msgstats.csv"
            echo $POST_CMD >> ${jfile}_postMsgStats.sh
	    echo "mv msgstats.csv ./${winstall}/tg/" >> ${jfile}_postMsgStats.sh
	fi
        echo date >> ${jfile}.sh

        if [ $ENERGYSTATS_CFG -eq 1 ]; then

	    echo "echo '===============================================' " >> ${jfile}.sh
	    echo "echo ' Running Energy test	                       ' " >> ${jfile}.sh
	    echo "echo '===============================================' " >> ${jfile}.sh
	    jobHeader="c${case}_energy_pd_${pd}_xe_${xe}_ce_${ce}_sq_${sq}_${BLOCKCOUNT}b"
            winstall="install_$jobHeader"

	    mkdir -p ./${winstall}/tg/logs

            CLEAN_CMD="WORKLOAD_INSTALL_ROOT=./${winstall} make -f Makefile.tg clean uninstall "
            echo $CLEAN_CMD >> ${jfile}.sh

	    CMD_OPT="ENERGY=yes WORKLOAD_INSTALL_ROOT=./${winstall}"
	    if [ ! -z "$WORKLOAD" ]; then
		CMD_OPT="${CMD_OPT} WORKLOAD_ARGS='${WORKLOAD}'"
	    fi
	    if [ ! -z "$CFGFILE" ]; then
		CMD_OPT="${CMD_OPT} TG_CONFIG_TEMPLATE_CFG=config_energy.cfg"
	    fi

            RUN_CMD="${CMD_OPT} make -f Makefile.tg run"
            echo $RUN_CMD >> ${jfile}.sh

            echo "cp ~/ocr/ocr/build/tg-xe/ocr-config.h ./${winstall}/tg/ocr-config.h.sav" >> ${jfile}.sh
            echo "cp ~/ocr/ocr/build/common.mk ./${winstall}/tg/common.mk.sav" >> ${jfile}.sh
            echo "cp Makefile.tg ./${winstall}/tg/Makefile.tg.sav" >> ${jfile}.sh

	    POST_CMD="python ~/ocr/ocr/scripts/tgStats/tgStats.py ./${winstall}/tg/logs"
            echo $POST_CMD >> ${jfile}_postEnergyStats.sh
	    echo "mv results/ ./${winstall}/tg/" >> ${jfile}_postEnergyStats.sh
	fi
    )
}

rm run_commands

for BLOCKCOUNT in ${BLOCKCOUNTS[@]}; do

    jfile=jobScript_${BLOCKCOUNT}d
    rm ${jfile}.sh
    rm ${jfile}_postMsgStats.sh
    rm ${jfile}_postEnergyStats.sh

    echo "#!/bin/bash" >> ${jfile}.sh
    echo >> ${jfile}.sh
    echo "#!/bin/bash" >> ${jfile}_postMsgStats.sh
    echo >> ${jfile}_postMsgStats.sh
    echo "#!/bin/bash" >> ${jfile}_postEnergyStats.sh
    echo >> ${jfile}_postEnergyStats.sh

#    if [ -f ${CFGFILE} ]; then
    if [[ -f "${CFGFILE/\~/$HOME}" ]]; then
	if [ $MSGSTATS_CFG -eq 1 ]; then
            BLK_CMD="sed '/block_count/c\block_count   = ${BLOCKCOUNT}' ${CFGFILE}> config_msgStats.cfg"
	    eval ${BLK_CMD}
            sed -i '/krnl_args/c\  krnl_args    = -mbboot -mbsafepayload -ocrdbgmask 0x8' config_msgStats.cfg
	fi
	if [ $ENERGYSTATS_CFG -eq 1 ]; then
	    #gawk -F'=|#' '/^\s*trace\s*=/{$2=sprintf("= 0x%x #",or($2, 0x210000000))} //' ${CFGFILE} > config_energy.cfg
	    gawk -F'=|#' '/^\s*trace\s*=/{$2=sprintf("= 0x%x #",or($2, 0x210000000))} //' "${CFGFILE/\~/$HOME}" > config_energy.cfg
            BLK_CMD="sed -i '/block_count/c\  block_count   = ${BLOCKCOUNT}' config_energy.cfg"
	    eval ${BLK_CMD}
	fi
    else
        echo ERROR: Failed to find configuration file: ${CFGFILE}. Exiting...
        exit 1
    fi

##########################################################
# this block can be replaced with multiple addScript calls
#
#    for pd in ${PDSHARE[@]}; do
#    for user in ${L1_USER[@]}; do
#    for runtime in ${L1_RUNTIME[@]}; do
#	addScript $pd $user $runtime
#    done
#    done
#    done
#
##########################################################

    # 1. PD: 8XE, L2, XE multiget disabled, CE multiget diabled, spawn queue disabled
    addScript 1 1 1 0 0 0
    # 2. PD: 8XE, L2, XE multiget enabled, CE multiget enabled, spawn queue disabled
    addScript 2 1 1 1 1 0
    # 3. PD: 8XE, L2, XE multiget disabled, CE multiget disabled, spawn queue enabled
    addScript 3 1 1 0 0 1

    chmod +x ${jfile}.sh
    chmod +x ${jfile}_postMsgStats.sh
    chmod +x ${jfile}_postEnergyStats.sh

    echo "./${jfile}.sh &> ${jfile}.out &" >> run_commands

done
