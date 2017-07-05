#!/usr/bin/env python

import argparse
import multiprocessing
import os.path
import sys
import ConfigParser
import io
import itertools
import tempfile

# README First
# This script modifies a few fields in the OCR config file:
# 1. Modifies L1 SPAD start address based on how much of it is already being used by ELF sections
# 2. Modifies DRAM start address based no how much of it is being used by ELF section

parser = argparse.ArgumentParser(description='Generate a modified OCR config \
        file for XE from original config file & binary file.')
parser.add_argument('--binstart', dest='binstart', default='0x0',
                   help='Binary file start address (default: 0x0)')
parser.add_argument('--binend', dest='binend', default='0x0',
                   help='Binary file end address (default: 0x0)')
parser.add_argument('--fsimcfg', dest='fsimcfg', default='config.cfg',
                   help='FSim config file to use (default: config.cfg)')
parser.add_argument('--ocrcfg', dest='ocrcfg', default='default.cfg',
                   help='OCR config file to use (will be overwritten)')

args = parser.parse_args()
binstart = args.binstart
binend = args.binend
binsize = long(binend, 16) - long(binstart, 16)
ocrcfg = args.ocrcfg
fsimcfg = args.fsimcfg
print "Size is 0x%lx" % (binsize,)

def ExtractValues(infilename):
    config = ConfigParser.SafeConfigParser(allow_no_value=True)
    config.readfp(infilename)
    global platstart, platsize, tgtsize, allocsize
    # FIXME: Currently only does for L1 scratchpad - the below ini strings are hardcoded
    platstart = config.get('MemPlatformInst0', 'start').strip(' ').split(' ')[0]
    platstart = ''.join(itertools.takewhile(lambda s: s.isalnum(), platstart))
    platsize = config.get('MemPlatformInst0', 'size').strip(' ').split(' ')[0]
    platsize = ''.join(itertools.takewhile(lambda s: s.isalnum(), platsize))
    tgtsize = config.get('MemTargetInst0', 'size').strip(' ').split(' ')[0]
    tgtsize = ''.join(itertools.takewhile(lambda s: s.isalnum(), tgtsize))
    allocsize = config.get('AllocatorInst0', 'size').strip(' ').split(' ')[0]
    allocsize = ''.join(itertools.takewhile(lambda s: s.isalnum(), allocsize))

def ExtractValuesFsim(infilename):
    config = ConfigParser.SafeConfigParser(allow_no_value=True)
    config.readfp(infilename)
    global neighborcount, stackadd
    global m2

    cc = config.get('SocketGlobal', 'cluster_count').strip(' ').split(' ')[0]
    cc = int(''.join(itertools.takewhile(lambda s: s.isdigit(), cc)))
    bc = config.get('ClusterGlobal', 'block_count').strip(' ').split(' ')[0]
    bc = int(''.join(itertools.takewhile(lambda s: s.isdigit(), bc)))
    neighborcount = bc*cc
    stackadd = 0

    m2 = config.get('BlockGlobal', 'sl2_size').strip(' ').split(' ')[0]
    m2 = int(''.join(itertools.takewhile(lambda s: s.isdigit(), m2)))
    m2 = m2 * 1024

    # If stack is in IPM, we can reclaim that space (0x7000) in L1
    if config.has_option('TricksGlobal', 'xe_ipm_stack'):
        xe_ipm_stack = config.get('TricksGlobal', 'xe_ipm_stack').strip(' ').split(' ')[0]
        xe_ipm_stack = int(''.join(itertools.takewhile(lambda s: s.isdigit(), xe_ipm_stack)))
        if xe_ipm_stack > 0:
            stackadd = 0x7000

def RewriteConfig(cfg):
    global platsize, tgtsize, allocsize, neighborcount, stackadd
    global m2

    with open(cfg, 'r+') as fp:
        lines = fp.readlines()
        fp.seek(0)
        fp.truncate()
        section = 0    # Keeps track of section being parsed
        for line in lines:
            if 'MemPlatformInst0' in line:
                section = 1
            if 'MemPlatformInstForL2' in line:
                section = 2
            if 'MemTargetInst0' in line:
                section = 10
            if 'MemTargetInstForL2' in line:
                section = 20
            if 'AllocatorInst0' in line:
                section = 100
            if 'AllocatorInstForL2' in line:
                section = 200
            if 'PolicydomainInst0' in line:
                section = 1000
            if section == 1 and 'start' in line:
                line = '   start = \t' + hex(long(platstart,16)+binsize) + '\n'
            if section == 1 and 'size' in line:
                line = '   size =\t' + hex(long(platsize,16)-binsize+stackadd-0x100) + '\n'
                section = 0
            if section == 2 and 'size' in line:
                line = '   size\t=\t' + hex(m2) + '\n'
                m2 = m2 - (m2 >> 5)  # issue #902
                section = 0

            if section == 10 and 'size' in line:
                line = '   size =\t' + hex(long(tgtsize,16)-binsize+stackadd-0x100) + '\n'
                section = 0
            if section == 20 and 'size' in line:
                line = '   size\t=\t' + hex(m2) + '\n'
                m2 = m2 - (m2 >> 5)
                section = 0

            if section == 100 and 'size' in line:
                line = '   size =\t' + hex(long(allocsize,16)-binsize+stackadd-0x100) + '\n'
                section = 0
            if section == 200 and 'size' in line:
                line = '   size\t=\t' + hex(m2) + '\n'
                section = 0

            if section == 1000 and 'neighborcount' in line:
                line = '   neighborcount = \t' + str(neighborcount-1) + '\n'
                section = 0

            fp.write(line)


def StripLeadingWhitespace(infile, outfile):
    with open(infile, "r") as inhandle:
        for line in inhandle:
            outfile.write(line.lstrip())

if os.path.isfile(ocrcfg):
    # Because python config parsing can't handle leading tabs
    with tempfile.TemporaryFile() as temphandle:
        StripLeadingWhitespace(ocrcfg, temphandle)
        temphandle.seek(0)
        ExtractValues(temphandle)
    if os.path.isfile(fsimcfg):
        with tempfile.TemporaryFile() as temphandle:
            StripLeadingWhitespace(fsimcfg, temphandle)
            temphandle.seek(0)
            ExtractValuesFsim(temphandle)
    else:
        print 'Unable to find FSim config file ', fsimcfg
    RewriteConfig(ocrcfg)
else:
    print 'Unable to find OCR config file ', ocrcfg
    sys.exit(0)
