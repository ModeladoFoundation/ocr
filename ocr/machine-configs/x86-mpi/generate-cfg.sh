#!/bin/bash

export CFG_SCRIPT=../../scripts/Configs/config-generator.py

echo "$PWD/../../scripts/Configs/config-generator.py"

PLATFORM=mpi

# Placement Affinity
ARGS="--guid COUNTED_MAP --target ${PLATFORM} --scheduler PLACEMENT_AFFINITY  --remove-destination"
for c in `echo "2 4 8"`; do
    $CFG_SCRIPT ${ARGS} --threads ${c} --output mach-x86-${PLATFORM}-affinity-${c}w-lockableDB.cfg
done

# ST
ARGS="--guid COUNTED_MAP --target ${PLATFORM} --scheduler ST  --remove-destination"
for c in `echo "2 4 8"`; do
    $CFG_SCRIPT ${ARGS} --threads ${c} --output mach-x86-${PLATFORM}-st-${c}w-lockableDB.cfg
done

# STATIC
ARGS="--guid COUNTED_MAP --target ${PLATFORM} --scheduler STATIC  --remove-destination"
for c in `echo "2 4 8"`; do
    $CFG_SCRIPT ${ARGS} --threads ${c} --output mach-x86-${PLATFORM}-static-${c}w-lockableDB.cfg
done

# Legacy
ARGS="--guid COUNTED_MAP --target ${PLATFORM} --scheduler PLACEMENT_AFFINITY  --remove-destination"
for c in `echo "8"`; do
    $CFG_SCRIPT ${ARGS} --threads ${c} --output mach-x86-${PLATFORM}-legacy-${c}w-lockableDB.cfg
done

# Jenkins config
ARGS="--guid LABELED --target ${PLATFORM} --scheduler PLACEMENT_AFFINITY --threads 8 --remove-destination"
$CFG_SCRIPT ${ARGS} --output jenkins-x86-${PLATFORM}.cfg

# Jenkins ST config
ARGS="--guid COUNTED_MAP --target ${PLATFORM} --scheduler ST --threads 8 --remove-destination"
$CFG_SCRIPT ${ARGS} --output jenkins-x86-${PLATFORM}-st.cfg

unset CFG_SCRIPT
