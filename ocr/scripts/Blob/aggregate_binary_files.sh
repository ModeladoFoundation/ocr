#!/bin/bash

if [ $# -le 3 ]
then
echo "Usage: ${0} <CE struct file> <XE struct file> <Args> <Optional file> <Output bin file>"
else

LEAF_CE_STRUCT=5120
XE_STRUCT=4096
SHARED_XE_STRUCT=16384
ARGS_STRUCT=4096

# This file aggregates multiple binary files resulting in the following layout:
# 1. CE Struct binary padded to 4K
# 2. XE Struct binary padded to 4K (or 16K for L2 shared binary)
# 3. Args padded to 4K
# 4. Input file(s)

# Copy the CE struct file
cat $1 > tmpfile

# Zero pad it
size=`stat -c%s $1`
let size=${LEAF_CE_STRUCT}-$size
if test $size -lt 0 ; then
    echo "CE size is greater than ${LEAF_CE_STRUCT} (overflow: ${size}) -- aborting"
    exit 1
fi
dd if=/dev/zero ibs=1 count=$size status=none >> tmpfile

# Do the same with XE struct file
cat $2 >> tmpfile
size=`stat -c%s $2`
if test $size -gt ${XE_STRUCT}; then
  let xe_struct_size=${SHARED_XE_STRUCT}
else
  let xe_struct_size=${XE_STRUCT}
fi
let size=${xe_struct_size}-$size
if test $size -lt 0 ; then
    echo "XE size is greater than ${xe_struct_size} (overflow: ${size}) -- aborting"
    exit 1
fi
dd if=/dev/zero ibs=1 count=$size status=none >> tmpfile

# Do the same with args file
cat $3 >> tmpfile
size=`stat -c%s $3`

# If there's a file, dump that too, otherwise bail out

shift 3

if [ $# -eq 2 ]
then
if [ -e $1 ]
then
let size=${ARGS_STRUCT}-$size
if test $size -lt 0 ; then
    echo "Argument size is greater than ${ARGS_STRUCT} (overflow: ${size}) -- aborting"
    exit 1
fi
dd if=/dev/zero ibs=1 count=$size status=none >> tmpfile
cat $1 >> tmpfile
fi
shift
fi

# Now rename tmpfile
mv tmpfile $1

fi
