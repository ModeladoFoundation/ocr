#
# This file is subject to the license agreement located in the file
# LICENSE and cannot be distributed without it.  This notice
# cannot be removed or modified
#


import sys
import os
import subprocess
import collections
import math
from operator import itemgetter

#============ Fetch unique user EDT guid types ============
def getUniqueEDTNames(nameMap):
    names = []
    for i in range(len(nameMap)):
        curName = nameMap[i][1]
        names.append(curName)

    uniq = set(names)
    return list(uniq)

#=============== Generate key-val data structure for EDT guids to name =========
def getNameMap(names):
    nameMap = []
    for line in names:
        curLine = line.split()
        name = curLine[7]
        edt = curLine[5]
        nameMap.append([edt, name])
    return nameMap

#============= Generatle key-val data structure for EDT guids to exe time ==========
def getTimeMap(names):
    timeMap = []
    for line in names:
        curLine = line.split()
        edt = curLine[5]
        startTime = float(curLine[9])
        endTime = float(curLine[11])
        time = endTime - startTime
        timeMap.append([edt, time])
    return timeMap

#========= Create DB guid to DB size keyval pairs ========
EDTGUID_INDEX = 15
DBSIZE_INDEX = 11
def getDbMap(dbs):
    dbMap = []
    for line in dbs:
        curLine = line.split()
        curEdt = curLine[EDTGUID_INDEX][:-1]
        curSize = curLine[DBSIZE_INDEX]
        dbMap.append([curEdt, curSize])
    return dbMap

#============ More verbose version of getTimeMap w/ start and end times =============
def getRunTimes(names):
    runTimeMap = []
    sortedNames = sortAscendingStartTime(names)
    offSet = float(sortedNames[0].split()[9])
    for i in sortedNames:
        curLine = i.split()
        #Structure: each record has EDT name(0) guid(1) startTime(2) and endTime(3)
        curStart = (float(curLine[9]) - offSet)
        curEnd = (float(curLine[11]) - offSet)
        runTimeMap.append([curLine[7], curLine[5], curStart, curEnd])
    return runTimeMap

#=========Account for (possible) out of order log writes by sorting=====
def sortAscendingStartTime(logFile):
    splitLog = []
    sortedList = []
    for line in logFile:
        splitLog.append(line.split())

    for element in splitLog:
        element[9] = int(element[9])

    tempSorted = sorted(splitLog, key=itemgetter(9))

    for element in tempSorted:
        element[9] = str(element[9])

    for element in tempSorted:
        sortedList.append((' '.join(element))+'\n')

    return sortedList

#============== Get name from EDT guid ==============
def edtGuidToName(guid, nameMap):
    for i in nameMap:
        if i[0] == guid:
            return i[1]

    return 'runtimeEDT'

#========= Create EDT -> size keyval pairs ==========
def getSizeMap(dbs):
    sizeMap = []
    for line in dbs:
        curLine = line.split()
        curEdt = curLine[3][4:-1]
        curDbSize = curLine[13]
        sizeMap.append([curEdt, curDbSize])
    return sizeMap

#========= Returns time EDT guid took to execute =======
def edtGuidToTime(guid, timeMap):
    for i in timeMap:
        if i[0] == guid:
            return i[1]
    return 1

#========= Calculate total execution time =========
def getTotalTime(timeMap):
    total = 0
    for i in timeMap:
        total += int(i[1])
    return total

#========= Calculate cumulative total of acquired DBs ======
def getTotalSize(sizeMap):
    sizes = []
    for i in range(len(sizeMap)):
        curSize =  sizeMap[i][1]
        sizes.append(int(curSize))
    return sum(sizes)

#============== Get mainEDT's guid =================
def getMainEdtGuid(nameMap):
    for i in nameMap:
        if i[1] == 'mainEdt':
            return i[0]

    return "Error, no Main EDT"

#======= Simple fp rounding function =======
def roundToTwo(num):
    num = "%.2f" % round(num,2)
    return num

#=============== Conglomerate all data and calculate time and data stats per edt =====================
def statsPerEdt(edt, timeMap, rtMap, dbSizeMap, nameMap, exeTime, totalBytes, usrDbMap, rtDbMap, totalUser, totalRt, outFile):
    instCount = 0
    curMatches = []
    for i in range(len(nameMap)):
        if nameMap[i][1] == edt:
            curMatches.append(nameMap[i][0])
            instCount += 1

    curTimes = []
    for i in range(len(curMatches)):
        for j in range(len(timeMap)):
            if curMatches[i] == timeMap[j][0]:
                curTimes.append(int(timeMap[j][1]))

    curSizes = []
    for i in range(len(curMatches)):
        for j in range(len(dbSizeMap)):
            if curMatches[i] == dbSizeMap[j][0]:
                curSizes.append(int(dbSizeMap[j][1]))

    curUsrSizes = []
    for i in range(len(usrDbMap)):
        if edtGuidToName(usrDbMap[i][0], nameMap) == edt:
            curUsrSizes.append(int(usrDbMap[i][1]))

    curRtSizes = []
    for i in range(len(rtDbMap)):
        if edtGuidToName(rtDbMap[i][0], nameMap) == edt:
            curRtSizes.append(int(rtDbMap[i][1]))

    print '---------- EDT Name: ' + str(edt) + ' ----------\n'
    print 'Number of times observed: ', instCount
    print 'Total time attributed: ', sum(curTimes), 'ns\n'

    if len(curTimes) == 0:
        curAvg = 0
        curAvg = roundToTwo(curAvg)
        print 'Average time attributed: 0ns'
    else:
        curAvg = (sum(curTimes)/float(len(curTimes)))
        curAvg = roundToTwo(curAvg)
        print 'Average time attributed: ', curAvg, 'ns'

    if exeTime == 0:
        curPerc = 0
        curPerc = roundToTwo(curPerc)
        print 'Percentage of execution time attributed: 0%\n'
    else:
        curPerc = (sum(curTimes)/float(exeTime))*100
        curPerc = roundToTwo(curPerc)
        print 'Percentage of execution time attributed: ', curPerc, '%\n'

    print 'Number of DBs acquired by "', str(edt), '": ', len(curUsrSizes)
    print 'Cummulative DB size: ', sum(curUsrSizes), 'bytes'

    if instCount == 0:
        acqsPerEdt = len(curUsrSizes)
    else:
        acqsPerEdt = len(curUsrSizes)/instCount
    acqPerc = (sum(curUsrSizes)/float(totalUser))*100
    acqPerc = roundToTwo(acqPerc)
    print 'Percentage of DB acquires attributed: ', acqPerc, '%\n\n'

    outFile.write(str(edt) + ',' + str(instCount) + ',' + str(sum(curTimes)) + ',' + str(curAvg) + ',' + str(curPerc) + ',' + str(acqsPerEdt) + ',' + str(sum(curUsrSizes)) + ',' + str(acqPerc) + '\n')

#========= Strip necessary records from debug logs with grep =========
def runShellStrip(dbgLog):
    os.system("egrep -w \'API\\(EDTSTAT\\)\' " + str(dbgLog) + " | grep \'EXIT ocrDbCreate\' | grep -v INVAL > dbs.txt")
    os.system("grep FctName " + str(dbgLog) + " > names.txt")
    os.system("grep \'DB(EDTSTAT)\' " + str(dbgLog) + " | grep Acquiring | grep \'runtime acquire: 0\' > userAcqs.txt")
    os.system("grep \'DB(EDTSTAT)\' " + str(dbgLog) + " | grep Acquiring | grep \'runtime acquire: 1\' > rtAcqs.txt")

#======== Remove temporarily created files =============
def cleanup():
    os.remove('names.txt')
    os.remove('dbs.txt')
    os.remove('userAcqs.txt')
    os.remove('rtAcqs.txt')

#========= MAIN ==========
def main():

    #Get rid of events we are not interested in seeing
    if len(sys.argv) != 2:
        print "Error... Usage: python netformat.py <inputFile>"
        sys.exit(0)
    dbgLog = sys.argv[1]
    runShellStrip(dbgLog)

    #Read in temp files produced by grep
    apiDbsFP = open('dbs.txt', 'r')
    apiDbs = apiDbsFP.readlines()
    edtsFP = open('names.txt', 'r')
    names = edtsFP.readlines()
    userDbsFP = open('userAcqs.txt', 'r')
    userDbs = userDbsFP.readlines()
    rtDbsFP = open('rtAcqs.txt', 'r')
    rtDbs = rtDbsFP.readlines()

    #Close file pointers and cleanup temp files
    apiDbsFP.close()
    edtsFP.close()
    rtDbsFP.close()
    userDbsFP.close()
    cleanup()

    if len(names) == 0:
        print "Error: No EDT names found in debug log. Be sure proper flags are set and OCR has been rebuilt"
        sys.exit(0)

    #Fetch GUIDs of EDTs and Events, and src->dst pairs from satisfaction log records
    nameMap = getNameMap(names)
    timeMap = getTimeMap(names)
    rtMap = getRunTimes(names)
    dbSizeMap = getSizeMap(apiDbs)
    usrDbMap = getDbMap(userDbs)
    rtDbMap = getDbMap(rtDbs)


    totalExeTime = getTotalTime(timeMap)

    totalSize = getTotalSize(dbSizeMap)
    uniqEdtNames = getUniqueEDTNames(nameMap)
    uniqEdtNames.append('runtimeEDT')
    totalUser = 0
    totalRt = 0
    for i in range(len(usrDbMap)):
        totalUser += int(usrDbMap[i][1])
    for i in range(len(rtDbMap)):
        totalRt += int(rtDbMap[i][1])

    print '\n\nTotal user code (EDT) execution time: ', totalExeTime, 'ns'
    print 'Total user acquired bytes: ', totalUser, 'bytes'
    print 'Total runtime acquired bytes: ', totalRt, 'bytes\n\n'
    outFile = open('stats.csv', 'w')
    outFile.write('sep=,\n')
    outFile.write(' ,Times Observed,Total Time Attributed(ns),Average Time Attributed(ns),Percentage of Total Exe Time,Num DB Acquires Each,Total Bytes Acquired,Percentage Acquired by EDT\n')

    #Report stats to console and write to .csv
    for edt in uniqEdtNames:
        statsPerEdt(edt, timeMap, rtMap, dbSizeMap, nameMap, totalExeTime, totalSize, usrDbMap, rtDbMap, totalUser, totalRt, outFile)

    outFile.close()

    sys.exit(0)

main();
