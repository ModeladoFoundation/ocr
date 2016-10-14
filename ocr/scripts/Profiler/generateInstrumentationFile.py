#!/usr/bin/python

# Modified in 2014 by Romain Cledat (now at Intel). The original
# license (BSD) is below. This file is also subject to the license
# aggrement located in the OCR LICENSE file and cannot be distributed
# without it. This notice cannot be removed or modified

# Copyright (c) 2011, Romain Cledat
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#     * Neither the name of the Georgia Institute of Technology nor the
#       names of its contributors may be used to endorse or promote products
#       derived from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL ROMAIN CLEDAT BE LIABLE FOR ANY
# DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

MODE_UNK   = -1
MODE_RT    = 0x1
MODE_RTAPP = 0x3
MODE_APP   = 0x4

import getopt, glob, os, re, shutil, sys

class Usage:
    def __init__(self, msg):
        self.msg = msg

def main(argv=None):
    if argv is None:
        argv = sys.argv

    opMode = MODE_UNK # Mode of operation
    topDir = None     # Top directory to search
    excludeDir = []   # List of directories to exclude
    includeExt = ['c', 'h']   # List of extensions to process
    outFile = None    # Out file
    rtFile = None     # Runtime file
    rootDir = None    # Directory to scan
    quietMode = False # Set to true if you want no output
    createOtherBucket = False

    try:
        try:
            opts, args = getopt.getopt(argv[1:], "hm:o:", ["help", "mode=", "out=", "rtfile=", "ext=", "exclude=", "quiet",
                                                           "otherbucket"])
        except getopt.error, err:
            raise Usage(err)
        for o, a in opts:
            if o in ('-h', '--help'):
                raise Usage(\
"""
    Usage: %s -m MODE -o OUT [--ext EXTENSION] [--exclude DIR] [--rtfile FILE] ROOT

    This script will process all files with the proper extensions contained in ROOT
    except if in the excluded sub-directories and look for START_PROFILE calls to extract
    the name and ID of the profiling sites.

    -h,--help:      Prints this message
    -m,--mode:      Mode of generation. This must be one of:
                        - 'rt' to generate the profile files for the OCR runtime
                        - 'rtapp' to generate the profile files for the OCR runtime
                          so that it can be used with an application profile file
                        - 'app' to generate the profile files for an application
    -o,--out:       Base name of the output file to generate. This script will generate
                    both a '.h' and '.c' file that have the same name. This is only valid in
                    'rt' or 'rtapp' mode as the profile file for applications is generated in the
                    same file as for the runtime
    --ext:          Extensions to process. Defaults to 'c' and 'h'. This can be specified
                    multiple times
    --exclude:      Directories to exclude when looking for files. This can be specified
                    multiple times
    --rtfile:       Only when in 'app' mode: path to the base name of the generated file
                    for the runtime (generated in 'rtapp' mode). This file must already exist
                    and have properly processed the runtime source code
    --otherbucket   Create a "EVENT_OTHER" bucket that will be used to collect stuff that is not
                    the focus of the profiling
    --quiet:        Do not print anything
""" % (argv[0]))
            elif o in ('-m', '--mode'):
                if a == 'rt':
                    opMode = MODE_RT
                elif a == 'rtapp':
                    opMode = MODE_RTAPP
                elif a == 'app':
                    opMode = MODE_APP
                else:
                    raise Usage("Unknown mode: '%s'" % (a))
            elif o in ('-o', '--out'):
                if outFile is not None:
                    raise Usage("Outfile specified multiple times")
                outFile = a
            elif o in ('--ext'):
                includeExt.append(a)
            elif o in ('--exclude'):
                excludeDir.append(a)
            elif o in ('--rtfile'):
                if rtFile is not None:
                    raise Usage("Rtfile specified multiple times")
                rtFile = a
            elif o in ('--otherbucket'):
                createOtherBucket = True
            elif o in ('--quiet'):
                quietMode = True
            else:
                raise Usage("Unhandled option: %s" % o)
        # End of for loop on arguments
        if args is None or len(args) <> 1:
            raise Usage("Too many arguments or no root specified")
        rootDir = os.path.realpath(os.path.expanduser(os.path.expandvars(args[0])))

        # Start sanity checks
        if opMode == MODE_UNK:
            raise Usage("Missing mode")
        if (opMode & MODE_RT):
            if outFile is None:
                raise Usage("Output file required for RT or RTAPP mode")
            if rtFile is not None:
                rtFile = None
                print >> sys.stderr, "Ignoring rtfile in RT or RTAPP mode"
        else:
            if outFile is not None:
                outFile = None
                print >> sys.stderr, "Ignoring outfile in APP mode"
            if rtFile is None:
                raise Usage("Rtfile required for APP mode")
        if outFile is not None:
            outFile = os.path.realpath(os.path.expanduser(os.path.expandvars(outFile)))
        if rtFile is not None:
            rtFile = os.path.realpath(os.path.expanduser(os.path.expandvars(rtFile)))
            if (not os.path.isfile(rtFile + '.c.orig')) or (not os.path.isfile(rtFile + 'RT.h')):
                raise Usage("Rtfile does not point to a valid file base (looking for '.c.orig' and 'RT.h'): %s" % (rtFile))
        if not os.path.isdir(rootDir):
            raise Usage("Root directory (%s) is not valid" % (rootDir))
        # Done sanity checks
    except Usage, msg:
        print >> sys.stderr, msg.msg
        print >> sys.stderr, "For help, use -h"
        return 2


    allEvents = dict()
    allFunctions = set()
    allWarnings = set()

    profileLine_re = re.compile(r"\s*START_PROFILE\(\s*([0-9A-Za-z_]+)\s*\)");
    maxRtEvent_re = re.compile(r"\s*MAX_EVENTS_RT = ([0-9]+)")
    endCStruct_re = re.compile(r"^};$")
    eventOther_re = re.compile(r"^    \"EVENT_OTHER\"")
    eventType_re = re.compile(r"[0-9A-Za-z]+");

    for root, subDirs, files in os.walk(rootDir):
        if not quietMode:
            print "Going down '%s' ..." % (root)
        # Remove all the excluded directories
        toDelete = []
        for i, v in enumerate(subDirs):
            if v in excludeDir:
                toDelete.append(i)
        decrement = 0
        for i in toDelete:
            del subDirs[i-decrement]
            decrement += 1

        if not quietMode:
            print subDirs
        realRoot = os.path.abspath(root)
        # Process interesting files
        for extension in includeExt:
            files = glob.glob(realRoot + '/*.' + extension)
            for file in files:
                if not quietMode:
                    print "\tProcessing %s" % (os.path.basename(file))
                fileHandle = open(file, 'r')
                for line in fileHandle:
                    matchOb = profileLine_re.match(line)
                    if matchOb is not None:
                        # We have a match
                        # Get event type and value
                        evtValue = matchOb.group(1)
                        evtType = eventType_re.match(evtValue).group(0)
                        if evtType not in allEvents:
                            allEvents[evtType] = set()

                        if evtValue not in allEvents[evtType]:
                            allEvents[evtType].add(evtValue)
                        else:
                            allWarnings.add(evtValue)

                fileHandle.close()
    # We processed all the functions. We can start writing the output
    # In rtApp or rt mode, we just write a new file
    evtType_code = dict()
    evtValue_code = dict()
    if (opMode & MODE_RT):
        with open(outFile + 'RT.h', 'w') as fileHandle:
            fileHandle.write("""#ifndef __OCR_PROFILERAUTOGENRT_H__
#define __OCR_PROFILERAUTOGENRT_H__

#ifndef EXTRAE_RUNTIME_INSTRUMENTATION
#error "EXTRAE_RUNTIME_INSTRUMENTATION should be defined if including the auto-generated profile file"
#endif

#include <extrae.h>

#define BITMASK(n) \\
    ((unsigned long long)(1ULL<<(n-1)))

#define IS_EVENT_ENABLED( evtType ) \\
    ( (((unsigned long long) enabled_instrumentation_types) & BITMASK(evtType)) != 0 )

#define IS_EVENT_HWC_ENABLED( evtType ) \\
    ( (((unsigned long long) enabled_hw_counter_types) & BITMASK(evtType)) != 0 )

""")

            # Define the enum
            fileHandle.write('typedef enum {\n')
            keys = 1
            values = 1
            mask = 0x200
            for evtType in allEvents.iterkeys():
                evtType_code[evtType] = keys
                for evtValue in allEvents[evtType]:
                    evtValue_code[evtValue] = mask+values
                    fileHandle.write('    %s_type  = 0x%xULL,\n' % (evtValue, evtType_code[evtType]))
                    fileHandle.write('    %s_value = 0x%xULL,\n' % (evtValue, evtValue_code[evtValue]))
                    values += 1
                mask <<= 1
                keys += 1
            if opMode == MODE_RTAPP:
                fileHandle.write('    MAX_EVENTS_RT = %dU,\n} profilerEventRT_t;\n' % (values))
            else:
                if createOtherBucket:
                    fileHandle.write('EVENT_OTHER = %dU,\n    ' % (mask+values))
                    values += 1
                fileHandle.write('    MAX_EVENTS = %dU,\n} profilerEventRT_t;\n' % (values))

            # Define name mapping function
            fileHandle.write("""
static inline void Extrae_registerOcrEvents() {
"""         )

            # Define event values and descriptions
            for evtType in allEvents.iterkeys():
                fileHandle.write("""
    {{
        extrae_type_t evt_type = {0:d};
        unsigned size = {1:d};
        extrae_value_t values[] = {{
            0x0ULL""".format( evtType_code[evtType], len(allEvents[evtType])+1 ) )

                for evtValue in allEvents[evtType]:
                    fileHandle.write(', 0x%xLL' % ( evtValue_code[evtValue] ))
                fileHandle.write(""" };
        char* value_descr[] = {
            "END" """)
                for evtValue in allEvents[evtType]:
                    fileHandle.write(', "%s"' % ( evtValue ))
                fileHandle.write(""" }};
        Extrae_define_event_type( &evt_type, "OCR {0} event", &size, values, value_descr );
    }}
"""             .format(evtType) )

            # Define enabled event types mask
            fileHandle.write("""
}

// Enabled event mask as an anonymous enum
enum {
    enabled_instrumentation_types = 0
""")
            for evtType in allEvents.iterkeys():
                fileHandle.write("""
#ifdef ENABLE_EVENT_{0}
        | BITMASK({1})
#endif""".format( evtType.upper(), evtType_code[evtType] ) )

            fileHandle.write("""
};

// Enabled hardware counter event mask as an anonymous enum
enum {
    enabled_hw_counter_types = 0
""")
            for evtType in allEvents.iterkeys():
                fileHandle.write("""
#ifdef ENABLE_HWC_EVENT_{0}
        | BITMASK({1})
#endif""".format( evtType.upper(), evtType_code[evtType] ) )

            fileHandle.write("""
};

#endif
""")
        # End function

#
#    for warning in allWarnings:
#        fileHandle.write('#warning "%s is defined multiple times"\n' % (warning))

    return 0


if __name__ == "__main__":
    sys.exit(main())
