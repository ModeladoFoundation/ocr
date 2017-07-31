#!/usr/bin/awk -f

BEGIN {
    FS=","
    sending=1
    is_large=1
}
/^Sent/ {
    sending=1
}
/^Received/ {
    sending=0
}
NR!=FNR {
    is_large=0
}
/summary|XE0/ {
    next
}
{
    data[is_large][sending][$2]["ce"] = $3
    data[is_large][sending][$2]["xe"] = $4

    if (! ($2 in all_msgs))
        all_msgs[$2]=i++

}

function print_col(msg, ce_large, ce_small, xe_large, xe_small) {
    printf ",%s", msg
    printf ",%d,%d,%d,%d", ce_large, xe_large, ce_small, xe_small
    printf ",%d", ce_large - ce_small
    printf ",%d", xe_large - xe_small
    if (ce_small)
        printf ",%d%", (ce_large - ce_small) * 100.0 / ce_small
    else if (ce_large)
        printf ",inf"
    else
        printf ",0%"
    if (xe_small)
        printf ",%d%\n", (xe_large - xe_small) * 100.0 / xe_small
    else if (xe_large)
        printf ",inf\n"
    else
        printf ",0%\n"
}

END {
    printf ",,orig,,,        ,diff,        ,%% inc,        \n"
    printf ",,1_CE,1_XE,2_CE,2_XE,CE,XE,CE,XE\n"

    c = asorti(all_msgs, msgs)

    printf "Sent"
    for (i=1;i<=c;i++) {
        msg = msgs[i]
        print_col( msg, data[1][1][msg]["ce"], data[0][1][msg]["ce"], data[1][1][msg]["xe"], data[0][1][msg]["xe"])
    }

    printf "Received"
    for (i=1;i<=c;i++) {
        msg = msgs[i]
        print_col( msg, data[1][0][msg]["ce"], data[0][0][msg]["ce"], data[1][0][msg]["xe"], data[0][0][msg]["xe"])
    }
}
