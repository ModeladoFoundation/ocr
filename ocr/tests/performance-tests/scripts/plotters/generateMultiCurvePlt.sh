#!/bin/bash

DATA_FILE=$1
CURVETITLE_FILE=$2
XLABELS_FILE=$3
OUTPUT_IMG_NAME=$4
OUTPUT_PLOT_NAME=$5
TITLE=$6
XLABEL=$7
YLABEL=$8
COL_IDX=$9

#
# Extract target image format from name
#

OUTPUT_IMG_FORMAT=${OUTPUT_IMG_NAME#*.}

TMPL=${SCRIPT_ROOT}/plotters/multicurves_template.plt

#
# We need to generate 'N' plotting command based on the number of datasets titles defined.
#

CURVES_DEFS=""
NB_CURVES=`cat ${CURVETITLE_FILE} |wc -l`
while read -r line
do
    INPUT="\'$DATA_FILE\' using ${COL_IDX}:xticlabels(1) title \"$line\" with lines"
    if [[ -z "${CURVES_DEFS}" ]]; then
        CURVES_DEFS="$INPUT"
    else
        CURVES_DEFS="$CURVES_DEFS, ${INPUT}"
    fi
    let COL_IDX=${COL_IDX}+1
done < "${CURVETITLE_FILE}"

# Convert XLABELS_FILE data to a comma separated list
XTICKS=`cat $XLABELS_FILE | tr '\n' ',' | sed  s/,$//g`

sed -e "s|TITLE_VAR|${TITLE}|g"  \
    -e "s|XLABEL_VAR|${XLABEL}|g" \
    -e "s|YLABEL_VAR|${YLABEL}|g" \
    -e "s|XTICKS_VAR|${XTICKS}|g" \
    -e "s|CURVES_VAR|${CURVES_DEFS}|g" \
    -e s/OUTPUT_IMG_FORMAT_VAR/"${OUTPUT_IMG_FORMAT}"/g \
    -e s/OUTPUT_NAME_VAR/"${OUTPUT_IMG_NAME}"/g ${TMPL} > ${OUTPUT_PLOT_NAME}
