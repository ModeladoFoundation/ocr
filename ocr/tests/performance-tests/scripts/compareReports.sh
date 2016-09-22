
#Takes two reports as input and compare their throughouts

REPORT1=$1
REPORT2=$2

COL1=`cat $REPORT1 | grep "^[0-9]" | sed -e "s/ \+/ /g" | cut -d' ' -f 2-2 | tr '\n' ' '`
COL2=`cat $REPORT2 | grep "^[0-9]" | sed -e "s/ \+/ /g" | cut -d' ' -f 2-2 | tr '\n' ' '`

IFS=' ' read -r -a ARRAY1 <<< "$COL1"
IFS=' ' read -r -a ARRAY2 <<< "$COL2"

let lg=${#ARRAY1[@]}
let i=0;
while (( i < lg )); do
    echo "scale=3; (${ARRAY1[$i]} / ${ARRAY2[$i]})" | bc
    let i=$i+1
done
