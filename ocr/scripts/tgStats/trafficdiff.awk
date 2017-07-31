#!/usr/bin/awk -f
BEGIN {
    #perc=1
    #sidebyside=1
    FS=","
    if (ARGC != 3) {
        print "trafficdiff.awk takes two net_traffic.csv filenames as arguments" > "/dev/stderr"
        print "" > "/dev/stderr"
        print "You can also add arguments to turn on (or force off) perc and sidebyside output:" > "/dev/stderr"
        print "-v perc=1         turn on percentage output" > "/dev/stderr"
        print "-v perc=0         turn off percentage output" > "/dev/stderr"
        print "-v sidebyside=1   turn on side-by-side output" > "/dev/stderr"
        print "-v sidebyside=0   turn off side-by-side output" > "/dev/stderr"
        exit 1
    }
}
FNR==1 { # We are at the start of a file
    file++;
}
!header { # If we don't have a header, then we get it from the first input line
    header = $0
    if (sidebyside) {
        gsub(/,/, ",,,", header)
        header = substr(header, 3) "\n" # Remove 3 initial commas and add a \n
        for (i=1;i<=NF-2;i++) header = header ",orig,new,diff"
    }
    next
}
function getval(orig, new) {
    # Given an original and new value, this returns the entry for the output CSV
    # Note that if sidebyside is set, the output will be three CSV values
    if (perc) {
        if (orig == 0) {
            if (new == 0) diff = 0
            else diff = "+inf"
        } else {
            diff = sprintf ("%.2f%", (new - orig) / orig * 100)
        }
    } else {
        diff = new - orig
    }
    if (sidebyside) return sprintf ("%d, %d, %s", orig, new, diff)
    else return diff
}
/^[^,]/ { # For every line which is not a header
    for (i=2; i<=NF; i++) {
        if ($i=="") break # Ignore empty value at the end of the line
        if (file == 1) {
            # Get the orig value (this is file 1)
            data[$1][i-1] = $i
        } else {
            # Get the value to print (this is file 2)
            # Overwrite the orig value with it
            data[$1][i-1] = getval(data[$1][i-1], $i)
        }
    }
    # Keep track of all row headers
    if (file == 1) rows[++row] = $1
}
END {
    # Output everything
    print header
    for (x=1; x<=length(rows); x++) {
        row = rows[x]
        printf "%s", row
        for (y=1; y<=length(data[row]); y++) {
            printf ",%s", data[row][y]
        }
        printf "\n"
    }
}
