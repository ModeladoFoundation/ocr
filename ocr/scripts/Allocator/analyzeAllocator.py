#!/usr/bin/env python

import sys
import os


#global set containing allocator related data
trace_items = {'PD', 'WORKER_ID', 'TASK', 'TIMESTAMP', 'STARTTIME','ACTION', 'FUNC', 'MEMSIZE', 'MEMHINT', 'MEMPTR'}


def getFuncName(func):
    switcher = {
	0: "OCR_ALLOC_PDMALLOC",
	1: "OCR_ALLOC_PDFREE",
	2: "OCR_ALLOC_MEMALLOC",
	3: "OCR_ALLOC_MEMUNALLOC",
    }
    return switcher.get(int(func), "error")


def processLog(log):

    # create output file
    excelData = open('alloc_out.csv', 'w')
    
    for i in range (len(log)):
        cvsstring = ""
        #print "newline..." + log[i]

        notrace = log[i].strip("[TRACE]")
        terms = notrace.split('|')
        if(i==0):
	    cvsstring ="PD,WORKER_ID,TASK,TIME,ACTION,FUNC,MEMSIZE,MEMHINT,MEMPTR\n"
            #print cvsstring
            excelData.write(cvsstring) 
	    cvsstring = ""
	
        timestamp=0
	starttime=0
	for j in range(len(terms)):
	    pair = terms[j].split(':')
	    name = pair[0].strip()
	    value = pair[1].strip()
	    #print "{0}={1}".format(name, value)

	    if (name=="TIMESTAMP"):
		timestamp=int(value)
	    elif (name=="STARTTIME"):
		starttime=int(value)
		cvsstring += str(timestamp-starttime) +','
	    elif (name=="FUNC"):
		cvsstring += getFuncName(value) +','
            elif (name in trace_items):
		cvsstring += value+','

	# output 1 line
        cvsstring = cvsstring.rstrip(',') + '\n'
        excelData.write(cvsstring);
       # print "newline..." + log[i]

    excelData.close()

	
def usage():
    print "incorrect Usage..."
    print "To run: ./analyzeSchedOverhead.py <trace_output_file>"

#========= Strip necessary records from debug logs with grep =========
def runShellStrip(dbgLog):
    os.system("grep 'TYPE: ALLOCATOR' " + str(dbgLog) + " > alloc.log")


#======== Remove temporarily created files =============
def cleanup():
    os.remove('alloc.log')


###########################################################
###########################################################

def main():
    global total_exe_time

    if len(sys.argv) < 2:
        usage()

    dbgLog = sys.argv[1]
    runShellStrip(dbgLog)
    
    #Open and read trace file
    logFP = open('alloc.log', 'r')
    log = logFP.readlines()
    if(len(log) == 0):
        sys.exit("No scheduler TRACE events found in log")
 
    #process trace log and output
    print "processing..."
    processLog(log);

    print "...processing complete"
    logFP.close()

    cleanup()

    sys.exit(0)

if __name__ == "__main__":
    main()
