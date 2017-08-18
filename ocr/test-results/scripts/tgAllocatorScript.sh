#!/bin/bash

##########################################################################################
#
#  This script is used to run test suite for Allocator and/or PD modification to ocr.
#  It is current programmed to generate the 8 test cases for L2 Allocator.
#
#  Follow these steps to use it:
#  1. copy the script to the application directory
#  2. change the following if needed
#     - block number: BLOCKCOUNT=(n)            ; default to 4
#     - add msgstats run: MSGSTATS_CFG=[1|0]    ; default to 1
#     - add energy run: ENERGYSTATS_CFG=[1|0]   ; default to 1
#     - enable correct workload by uncomment the line or add your own line if needed.
#       fib and fft has default workload.
#     - make sure the correct configuration template is used. The default setting
#       was tested for fft, fib, and  RSbench. CoMD uses a local template that enables
#       large stack on ipm.
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
#WORKLOAD="-x 8 -y 8 -z 8 -i 2 -j 2 -k 2 -N 2 -n 1"

# generic configuration template
#CFGFILE="~/apps/apps/tg-configs/config-template.cfg"
# using local configuration template for CoMD, requiring xe_ipm_stack = 64
#CFGFILE="config.tpl"


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

function enableL1UserAlloc()
{
    local x=$1

    if [ $x -eq 0 ];	then
	MOD_CMD="sed -i '/CFLAGS += -DOCR_DISABLE_USER_L1_ALLOC/c\# CFLAGS += -DOCR_DISABLE_USER_L1_ALLOC' ~/ocr/ocr/build/common.mk"
    else
        MOD_CMD="sed -i '/CFLAGS += -DOCR_DISABLE_USER_L1_ALLOC/c\CFLAGS += -DOCR_DISABLE_USER_L1_ALLOC' ~/ocr/ocr/build/common.mk"
    fi
    echo $MOD_CMD >> ${jfile}.sh
}

function enableL1RuntimeAlloc()
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
    local user=$3
    local rt=$4
    local l2=$5
    local l1max=$6
    local l2max=$7

    echo "echo '==========================================================' " >> ${jfile}.sh
    echo "echo ' Setting configuration case ${case} in script ${jfile}.sh  ' " >> ${jfile}.sh
    echo "echo ' -OCR_SHARED_XE_POLICY_DOMMAIN: ${pd}                 ' " >> ${jfile}.sh
    echo "echo ' -OCR_DISABLE_USER_L1_ALLOC: ${user}                  ' " >> ${jfile}.sh
    echo "echo ' -OCR_DISABLE_RUNTIME_L1_ALLOC: ${rt}                 ' " >> ${jfile}.sh
    echo "echo ' -OCR_ENABLE_L2_ALLOC: ${l2}                          ' " >> ${jfile}.sh
    echo "echo '==========================================================' " >> ${jfile}.sh

    enableSharedPolicyDomain $pd
    enableL1UserAlloc $user
    enableL1RuntimeAlloc $rt
    enableL2Alloc $l2
    if [ $l2 -eq 1 ];	then
        setLocalCacheMax $l1max $l2max
    fi

    (
        echo date >> ${jfile}.sh
	if [ $MSGSTATS_CFG -eq 1 ]; then

	    echo "echo '===============================================' " >> ${jfile}.sh
	    echo "echo ' Running MsgStats test	                       ' " >> ${jfile}.sh
	    echo "echo '===============================================' " >> ${jfile}.sh
            jobHeader="c${case}_msgstats_pd_${pd}_user_${user}_rt_${rt}_l2_${l2}_${l1max}K_${BLOCKCOUNT}d"
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

	    POST_CMD="~/ocr/ocr/scripts/MsgStats/msgstats.sh ./${winstall}/tg/logs ${winstall}_msg.csv"
            echo $POST_CMD >> ${jfile}_postMsgStats.sh
	fi
        echo date >> ${jfile}.sh


        if [ $ENERGYSTATS_CFG -eq 1 ]; then

	    echo "echo '===============================================' " >> ${jfile}.sh
	    echo "echo ' Running Energy test	                       ' " >> ${jfile}.sh
	    echo "echo '===============================================' " >> ${jfile}.sh
	    jobHeader="c${case}_energy_pd_${pd}_user_${user}_rt_${rt}_l2_${l2}_${l1max}K_${BLOCKCOUNT}d"
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

    # 1. PD: singleXE, L1: user & runtime enabled, L2: diabled
    addScript 1 0 0 0 0 0 0
    # 2. PD: singleXE, L1: user & runtime disabled, L2: disabled
    addScript 2 0 1 1 0 0 0
    # 3. PD: 8XE, L1: user & runtime disabled, L2: disabled
    addScript 3 1 1 1 0 0 0
    # 4. PD: 8XE, L1: user & runtime enabled, L2: disabled
    addScript 4 1 0 0 0 0 0
    # 5. PD: 8XE, L1: user disabled & runtime enabled, L2:disabled
    addScript 5 1 1 0 0 0 0
    # 6. PD: 8XE, L1: user enabled & runtime disabled, L2: disabled
    addScript 6 1 0 1 0 0 0
    # 7. PD: 8XE, L1: user & runtime enabled, Local cache L1/L2: 64K/2048K
    addScript 7 1 0 0 1 64 2048
    # 8. PD: 8XE, L1: user & runtime enabled, Local cache L1/L2: 4K/32K
    addScript 8 1 0 0 1 4 32
    # 9. PD: 8XE, L1: user enabled  & runtime disabled, Local cache L1/L2: 64K/2048K (no limit)
    addScript 9 1 0 1 1 64 2048
    # 10. PD: 8XE, L1: user disable & runtime enabled, Local cache L1/L2: 64K/2048K (no limit)
    addScript 10 1 1 0 1 64 2048

    chmod +x ${jfile}.sh
    chmod +x ${jfile}_postMsgStats.sh
    chmod +x ${jfile}_postEnergyStats.sh

    echo "./${jfile}.sh &> ${jfile}.out &" >> run_commands

done
