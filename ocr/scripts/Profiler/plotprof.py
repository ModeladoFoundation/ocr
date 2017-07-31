#!/usr/bin/env python2

# Cleaned up
# Modified to work for analyzeProfile.py without -s

import argparse
import sys
import os
import re
from itertools import chain, product, dropwhile, islice, takewhile
import pipes

colorscheme="""
set linetype 1 lc rgb "#1B8E67" lw 1
set linetype 2 lc rgb "#D95F02" lw 1
set linetype 3 lc rgb "#7570B3" lw 1
set linetype 4 lc rgb "#E7298A" lw 1
set linetype 5 lc rgb "#66A61E" lw 1
set linetype 6 lc rgb "#E6AB22" lw 1
set linetype 7 lc rgb "#A6761D" lw 1
set linetype 8 lc rgb "#666666" lw 1
set linetype 9 lc rgb "#000000" lw 1
set linetype cycle 9
"""

# Indexes of records from get_profiler_summary
CUM_T, SELF_T, CALLS, AVG_T, STD_DEV_T, NAME = xrange(6)

def read_prof(prof_file):
    ''' Takes a profile output file path and turns it into a list of tuples:
       (cum, self, calls, avg, std_dev, name)
       All entries are strings'''
    ret = []

    with open(prof_file, 'r') as f:
        prof = f.read()

    lines = prof.splitlines()
    # Remove everything before the summary
    lines = dropwhile(lambda x: x != "#### TOTAL ####", lines)
    # Remove the header lines
    lines = islice(lines, 4, None)
    # Remove everything after the flat profile
    lines = takewhile(lambda x: x != "", lines)
    # Get lines that don't begin with '\'
    lines = (l for l in lines if l[0] != '\\')

    for line in lines:
        if line[0] == '/':
            ret.append(tuple(line.split()[2:8]))
        else:
            ret.append(tuple(line.split()[1:7]))
        # Check for malphormed lines
        if len(ret[-1]) < NAME+1:
            sys.stderr.write("\nLine in %s doesn't have enough fields\n" % prof_file)
            sys.stderr.write(line + '\n')
            sys.exit(1)

    return ret

def get_profs(args, count):
    profs = {}

    if count:
        col = CALLS
    else:
        col = CUM_T if args.cum else SELF_T

    for prof_name, prof_files in zip(args.prof_name, args.prof):

        raw_prof_data = map(read_prof, prof_files)

        profs[prof_name] = {subroutine: [0.] * len(prof_files) for subroutine in args.field}
        profs[prof_name]['mpi'] = [0. for i in prof_files]

        for i, dataset in enumerate(raw_prof_data):
            for d in dataset:
                if d[NAME] in args.field:
                    profs[prof_name][d[NAME]][i] = float(d[col])

                # Add all subroutines whos name contains *mpi* to the mpi subroutine
                if d[NAME].lower().find("mpi") != -1:
                    profs[prof_name]['mpi'][i] += float(d[col])

        for field in args.field:
            if field not in profs[prof_name]:
                sys.stderr.write("Warning: Unable to find field `%s' in %s.\n" % (field, prof_name))
                sys.stderr.write("    Assuming all zero.\n")
                profs[prof_name][field] = [0. for i in prof_files]

    return profs


def median(l):
    l=sorted(l)
    if (len(l) % 2):
        return l[len(l) // 2]
    else:
        return (l[len(l) // 2 - 1] + l[len(l) // 2]) / 2


def output_plot(profs, args, counts=None):

    outfile = None
    try:
        if args.outfile == '-':
            outfile = sys.stdout
        else:
            outfile = open(args.outfile, 'w+')

        outfile.write("#!/usr/bin/env gnuplot\n")

        outfile.write("\n# Created by %s\n" % " ".join(map(pipes.quote, sys.argv)))
        outfile.write("# in directory %s\n" % os.getcwd())

        outfile.write(colorscheme)

        outfile.write("set style data boxplot\n")
        if args.no_outliers:
            outfile.write("set style boxplot nooutliers\n")
        outfile.write("unset key\n")
        outfile.write("set grid xtics\n")
        xtics = []

        outfile.write("set term pdf enhanced size 8.5,11\n")
        outfile.write("set output '%s'\n" % args.pdf)

        outfile.write("set border 2\n")
        outfile.write("set ytics nomirror\n")

        if args.label_count:
            outfile.write('set ylabel "time (ms)\\n[median call count in brackets]"\n')
        else:
            outfile.write('set ylabel "time (ms)"\n')

        if args.log:
            outfile.write("set logscale y %d\n" % args.log)

        cmds=[]
        data=[]

        if args.sort:
            name_field = product(args.prof_name, args.field)
            name_field = sorted(name_field,key=lambda (n,f): -median(profs[n][f]))
        else:
            name_field = []
            for f in sorted(args.field,key=lambda f:-max(median(profs[n][f]) for n in profs.keys())):
                for n in args.prof_name:
                    name_field.append((n,f))

        i = 0
        for n, f in name_field:

            f_escape = f.replace('_', '\_')

            i += 1

            cmds.append("'-' using (%d):1 title '{/=10 %s} %s'" % (i,n,f_escape))

            if args.sort:
                line_color = args.prof_name.index(n)
            else:
                line_color = args.field.index(f)
            line_color = (line_color % 10) + 1
            cmds[-1] += " lc %d" % line_color

            data.append("\n".join(str(y) for y in profs[n][f]))
            xtics.append("'{/=10%s} %s' %d" % (n,f_escape,i))
            y_min = min(profs[n][f])

            med = median(profs[n][f])
            if args.label_count:
                label = median(counts[n][f])
            else:
                label = med

            if args.label_count:
                exp=len(str(abs(int(label))))-1
                label="%0.2fx10^%d" % (float(label)/10**exp, exp)

            if args.label_count:
                outfile.write("set label %f '[%s]' at %.2f, %f\n" % (i, label, i+.33, med))
            else:
                outfile.write("set label %f '%0.3f' at %.2f, %f\n" % (i, label, i+.33, med))

        outfile.write("set xtics (%s) scale 1.0 rotate by 60 right" % ",\\\n".join(xtics))
        outfile.write("\n")

        outfile.write("plot \\\n")
        outfile.write(", \\\n".join(cmds))
        outfile.write("\n")
        outfile.write("\ne\n".join(data))
        outfile.write("\ne\n")

    finally:
        if args.outfile != '-' and outfile:
            outfile.close()

def main(argv):
    parser = argparse.ArgumentParser(
            description='Plot the difference in profile results',
            usage='%(prog)s [options] (--prof NAME PROF [PROF ...] ) ... --field ...'\
    )
    parser.add_argument("--prof", type=str, nargs="+", metavar=("NAME", "PROF"),
                        action="append", required=True,
                        help="NAME is the human readable name of the profile. "
                             "PROFs are files containing the ouput of analyzeProfile.py. "
                             "Must have at least 2")
    parser.add_argument("--field", type=str, nargs="+", metavar="F", action="append",
                        help="F is a field to graph.")
    parser.add_argument("-c", "--cum", action="store_true",
                        help="Use the cumulative results in calculations")
    parser.add_argument("--label-count",  action="store_true",
                        help="Use the average count as the label instead of the median.")
    parser.add_argument("--no-outliers", action="store_true", help="Do not show outliers")
    parser.add_argument("--sort", action="store_true",
                        help="Sort by mean value instead grouping fields together")
    parser.add_argument("-o", metavar="OUTFILE", type=str, nargs=1, dest="outfile", default='out.gpi',
                        help="gnuplot file to output to (if - then stdout). Default out.gpi")
    parser.add_argument("--log", metavar="SCALE", type=int, nargs='?', default=0,
                        help="Use a log scale for the y access. Default scale is 2")

    args=parser.parse_args(argv)

    args.log = 2 if (args.log is None) else args.log

    if args.outfile != 'out.gpi':
        args.outfile = args.outfile[0] #De-listify
    args.pdf = str(args.outfile) + '.pdf'

    if not args.field:
        parser.error("Requires --field")

    args.prof_name = [l.pop(0) for l in args.prof]

    for l in args.prof:
        if not l:
            parser.error("Every --prof argument requires one or more PROFs")

    args.field = list(set(chain(*args.field)))

    profs = get_profs(args, count=False)
    if args.label_count:
        counts = get_profs(args, count=True)

    if args.label_count:
        output_plot(profs, args, counts)
    else:
        output_plot(profs, args)

    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

