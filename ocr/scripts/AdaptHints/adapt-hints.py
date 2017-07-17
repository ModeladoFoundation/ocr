#!/usr/bin/env python
import re
import sys
import argparse
import subprocess

parser = argparse.ArgumentParser(description='Generate a hints header file.')
parser.add_argument("-e","--exe", dest="executable", help="executable file to run", metavar="FILE")
parser.add_argument("-o","--out", dest="header_file",help="output header file", metavar="FILE")
parser.add_argument('args', nargs="*")

header_file = "priority.h"
args = parser.parse_args()
if not args.executable:
    print "must specify executable via -e!"
    exit()
if args.header_file:
    out_header = args.header_file


class PerfStats(object):
    fields="\t".join(["EDT(addr)",
                      "Count",
                      "L1_hits",
                      "L1_miss",
                      "Float_ops",
                      "EDT_Creates",
                      "DB_total",
                      "DB_creates",
                      "DB_destroys",
                      "EVT_Satisfies",
                      "mask" ])

    def __init__(self,edt_name,
                 address,
                 count,
                 hw_cycles,l1_hits,
                 l1_miss,
                 float_ops,
                 edt_creates,
                 db_total,
                 db_creates,
                 db_destroys,
                 evt_satisfies,
                 mask):
        self.edt_name       = edt_name
        self.address        = address
        self.count          = count
        self.l1_hits        = l1_hits
        self.l1_miss        = l1_miss
        self.float_ops      = float_ops
        self.edt_creates    = int(edt_creates)
        self.db_total       = db_total
        self.db_creates     = db_creates
        self.db_destroys    = db_destroys
        self.evt_satisfies = evt_satisfies
        self.mask           = mask

    def __str__(self):
        out_str = "\n"
        ivs = [v for v in dir(self) if not v.startswith('__')]
        ivs.remove("edt_name")
        d   = vars(self)
        out_str += "EDT : " + d["edt_name"] +"\n"
        for var in ivs:
            if var in d:
              out_str += "     " + var + " = " + str(d[var]) + "\n"
        return out_str



#    def __str__(self):
#        #TODO: format this string better to match the fields:
#        return self.edt_name+" ("+self.addr+")\t"+ "\t".join([self.count, self.l1_hits, self.l1_miss, self.float_ops, self.edt_creates, self.db_total, self.db_creates, self.db_destroys, self.evt_satisfies, self.mask])

def run_exe(exe=args.executable, args=args.args):
    arguments = [exe]+args
    #print arguments
    p=subprocess.Popen(arguments, stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    out,err=p.communicate()
    return out

def run_nm():
    p = subprocess.Popen(["nm",args.executable],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    out,err=p.communicate()
    lines = out.splitlines()
    addr_funcs = {}
    for line in lines:
        line = line.rstrip()
        if re.search('^[0-9]',line):
            #print line
            addr,mode,name = line.split()
            #print "addr: "+addr+" mode: "+mode+" name: "+name
            addr = int(addr,16)
            addr_funcs[addr]=name
    #print addr_funcs
    return addr_funcs

def process_output(results,addr_funcs):
    tail = False
    stats = []
    #results = open(args.filename)
    for line in results.splitlines():
        line = line.rstrip()
        fields = line.split()
        if tail:
            if len(fields) == 12:
                func_name = addr_funcs[int(fields[0],16)]
                ##replace the addres with the function name
                #fields.pop(0)
                print "func_name is: "+ func_name
                fields = [func_name]+fields
                stats.append(PerfStats(*fields))
                #print(addr_funcs[int(fields[0],16)] + "\t " + line)
        if len(fields)>2 and \
           fields[0] == "EDT" and \
           fields[1] == "Count" and \
           fields[2] == "HW_CYCLES" and \
           fields[3] == "L1_HITS":
            tail = True
    return stats


hf = open(header_file, 'w')
stats = process_output(run_exe(),run_nm())
#print PerfStats.fields
for stat in stats:
   #print stat.edt_name + " instantiated " + stat.count + " times"
   print stat
   if stat.edt_creates > 0:
      print stat.edt_creates
      hf.write("#define "+stat.edt_name[0].upper()+stat.edt_name[1:]+"_PRIORITY "+str(stat.edt_creates)+"\n")

hf.close()




