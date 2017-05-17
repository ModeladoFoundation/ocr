#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $# < 1 || $1 == "-h" || $1 == "--help" ]]; then
  cat <<EOF

Usage: $(basename $0) log_directory [out]

  log_directory - the directory containing the log files.
  out           - the filename to output to.
		  Default: out.csv

If out is -, then output to stdout"

EOF
  exit 1
fi

out_filename=${2-out.csv}
[[ $out_filename == "-" ]] && out_filename=/dev/stdout

for log in $1/*blk*.CE.[0-9][0-9]; do
  block=$(echo $log | sed 's/.*blk//' | cut -c-2)
  echo
  echo "block $block"
  sed 's/^CONSOLE: XE[0-9] >>> .*\(CONSOLE: COMM-PLAT(MSG)\)/\1/' $log
done | gawk -f $SCRIPT_DIR/msgstats.awk > $out_filename
