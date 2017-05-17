BEGIN {
    RECV="RECV"
    SEND="SEND"
}

# Match a line from commstats.bash telling us which block we are in
/^block [0-9][0-9]$/ {
    block = strtonum($2)
    blocks += 1
}

# Match a line for Sending/Receiving a msg from an XE
/COMM-PLAT\(MSG\).*XE/ {
    xe = substr($10,0,1)
    msg=$16
    direction=$6=="Sending" ? SEND : RECV
    XE[block][xe][msg][direction] += 1
    msgs[msg] = msg
    summary_xe[msg][direction] += 1

    if (xe+1 > xe_count)
        xe_count = xe+1
}

# Match a line for Sending a msg to a CE
/COMM-PLAT\(MSG\).*CE->CE message/ {
    msg=$23
    ce_addr=(($6=="Sent") ? $15 : $13)
    # CE number is (ce_addr << 23) & 0xf
    ce=and(rshift(strtonum(ce_addr),23),0xf)
    direction=(($6=="Sent") ? SEND : RECV)
    CE[block][ce][msg][direction] += 1
    msgs[msg] = msg
    summary_ce[msg][direction] += 1

    if (ce+1 > ce_count)
        ce_count = ce+1
}

function print_msgs(direction) {
    for (i=1; i <= msg_count; i++) {
        msg = msgs[i]

        # Print the msg name and summary for all CEs and XEs
        printf ",%s,%d,%d,", msg,summary_ce[msg][direction],summary_xe[msg][direction]

        for (block=0; block < blocks; block++) {
            for (ce=0; ce < ce_count; ce++) if (ce != block) printf ",%d",CE[block][ce][msg][direction]
            for (xe=0; xe < xe_count; xe++) printf ",%d",XE[block][xe][msg][direction]
            printf "," # Next block
        }

        printf "\n"
    }
}

END {
    msg_count = asort(msgs)

    # Print header of block numbers
    printf ",,summary,,,"
    for (block=0; block < blocks; block++) {
        printf "block %d", block
        for (i=0; i < ce_count + xe_count - 1; i++) printf ","
        printf "," # Next block
    }
    printf "\n"

    # Print header of agent numbers
    printf ",,CE,XE,,"
    for (block=0; block < blocks; block++) {
        for (ce=0; ce < ce_count; ce++) if (ce != block) printf "CE%d,", ce
        for (xe=0; xe < xe_count; xe++) printf "XE%d,", xe
        printf "," # Next block
    }
    printf "\n"

    # Print msg counts
    printf "Sent"
    print_msgs(SEND)
    printf "Received"
    print_msgs(RECV)

}
