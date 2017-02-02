#!/usr/bin/env python

import getopt, logging, os, re, shutil, sys, tempfile

from string import Template

# Set up the logger
class MyFilter(object):
    def __init__(self, highLevel):
        self.highLevel = highLevel

    def filter(self, record):
        if record.levelno < self.highLevel:
            return 1
        return 0

logging.getLogger().setLevel(logging.DEBUG)
streamHandlerStdOut = logging.StreamHandler(sys.stdout)
streamHandlerStdErr = logging.StreamHandler()
streamHandlerStdErr.setLevel(logging.WARNING)
streamHandlerStdOut.setLevel(logging.INFO)
streamFormatter = logging.Formatter('%(levelname)-8s %(message)s')
streamHandlerStdErr.setFormatter(streamFormatter)
streamHandlerStdOut.setFormatter(streamFormatter)
streamHandlerStdOut.addFilter(MyFilter(logging.WARNING)) # Leave WARNING and ERROR to stderr
logging.getLogger().addHandler(streamHandlerStdErr)
logging.getLogger().addHandler(streamHandlerStdOut)


class ProcessMode(object):
    CTXOPTI  = 0x0
    CTXCHECK = 0x1

    TRACEOFF = 0x0
    TRACEON  = 0x1

    def __init__(self):
        self.logger = logging.getLogger()
        self.ctxMode = None
        self.traceMode = None
        # Default values for macros
        self.macros = dict({
            'PDEVT_SCRATCH_BYTES': 1024,
            'PDEVT_MERGE_SIZE': 4,
            'PDEVT_LIST_SIZE': 4
            })

    def parseMode(self, s):
        modeFlags = s.split(',')
        for flag in modeFlags:
            if flag == "optimized":
                self._setCtxMode(self.CTXOPTI)
            elif flag == "ctxcheck":
                self._setCtxMode(self.CTXCHECK)
            elif flag == "trace":
                self._setTrace(self.TRACEON)
            else:
                raise Usage("Unknown value given to 'mode' parameter: '%s'\n" % (flag))

    def validateMode(self):
        self._fillDefaults()

    def setVariable(self, name, value):
        if name in self.macros.keys():
            self.macros[name] = value
        else:
            raise Usage("Unknown variable name '%s'" % (name))

    def _setCtxMode(self, val):
        if self.ctxMode is not None:
            self.logger.warning("Setting the context mode multiple times, overriding with %d\n" % (val))
        self.ctxMode = val

    def _setTrace(self, val):
        if self.traceMode is not None:
            self.logger.warning("Setting the trace mode multiple times, overriding with %d\n" % (val))
        self.traceMode = val

    def _fillDefaults(self):
        if self.ctxMode is None:
            self.ctxMode = self.CTXCHECK
        if self.traceMode is None:
            self.traceMode = self.TRACEON

# Globals

# File to process
gInputFileName = None
gInputFile = None

# Mode of operation
gProcessMode = ProcessMode()


# Print usage
class Usage:
    def __init__(self, msg):
        self.msg = msg

class CtxVar(object):
    """Contains information on a context variable"""
    def __init__(self, varType, varName, declLine, lineNo):
        self.varType = varType
        self.varName = varName
        self.declLine = declLine    # Text of the declaration
        self.lineNo = lineNo        # Line number this declaration appears at
        self.isEvt = False
        if self.varType.startswith('pdEvent') and self.varType.endswith('_t*'):
            self.isEvt = True

class ParseState(object):
    matchers = (
        # We restrict the matches to be on one line. This is not pure C but
        # is a reasonable restriction
        (re.compile('^(?P<space>\s*)START_FUNC\([^)]+\)\s*;\s*$'), '_startCallback'),
        (re.compile('^(?P<space>\s*)END_FUNC\s*;\s*$'), '_endCallback'),
        # A bit of a simplified parsing for the context but should be OK for now
        # It just matches single declarations (a single variable), with no type attributes
        # except for the fact that it is a pointer (no const, attribute, etc.)
        (re.compile('^(?P<space>\s*)__context\s+(?P<line>(?P<type>[a-zA-Z_$][0-9a-zA-Z_$]*)(?P<ptr>[\s*]+)(?P<varname>[a-zA-Z_$][0-9a-zA-Z_$]*)[^;]*;.*$)'),
         '_ctxVarCallback'),
        # Match for WAIT_ macros
        (re.compile('^(?P<space>\s*)(?P<line>WAIT_EVT(?P<numEvts>[0-9]+)\s*\((?P<vars>(?:[a-zA-Z_$][0-9a-zA-Z_$]*\s*(?:,\s*)?)+)\))\s*;'), '_waitCallback')
    )

    def __init__(self, outFile):
        self.outFile = outFile
        # Determine whether we have seen a START_FUNC
        # If we have, this will contain the line number where we saw it
        self.inFunction = None
        # Array of lines from START_FUNC onwards if we need to
        # This is used to "hoist" all context variables to the
        # beginning of the function
        self.lineBuffer = None

        # Dictionary of context variables
        # Key: name of variable
        # Value: a CtxVar object
        self.ctxVariables = dict()

        self.curIndent = None       # String of the whitespace for the current indent
        self.indentIncrement = None # One indent (used to generate aligned code)

        # Get a log file to write to
        self.log = logging.getLogger()

    def parseLine(self, line, lineNo):
        for check, callback in ParseState.matchers:
            m = check.match(line)
            if m is not None:
                self.log.debug("Found a pattern match on line %d -- calling callback %s"\
                               % (lineNo, callback))
                self.curIndent = m.group('space')
                return getattr(self, callback)(m, line, lineNo);
        self.curIndent = None
        # If we do not have any thing special to do
        # we buffer or print out the line
        self.log.debug("No match on line %d" % (lineNo))
        self._writeLine(lineNo, line)
        return True

    # Callback functions
    def _startCallback(self, matchObject, line, lineNo):
        self.log.debug("START_FUNC callback on line %d" % (lineNo))
        if self.inFunction is not None:
            self.log.error("Found START_FUNC on line %d before intervening END_FUNC\n" \
                           "\tPrevious START_FUNC on line %d" % (lineNo, self.inFunction))
            return False
        # Extract information about this call. As far as we are concerned, this is the
        # start of the function
        self.inFunction = lineNo
        self.indentIncrement = matchObject.group('space')

        # Initialize to the event passed in the input chain
        self.ctxVariables = dict()
        self.ctxVariables['inChain'] = CtxVar('pdEvent_t*', 'inChain', None, lineNo)

        # Initialize other variables
        self.currentIndent = None

        if gProcessMode.ctxMode == ProcessMode.CTXOPTI:
            # We need to buffer things
            self.log.debug("Optimized context mode is on -- starting buffering")
            self.lineBuffer = []
        else:
            self.lineBuffer = None
        self._writeLine(lineNo, line)
        return True

    def _endCallback(self, matchObject, line, lineNo):
        self.log.debug("END_FUNC callback on line %d" % (lineNo))
        # First check to see if we need to output all the buffered lines
        if self.lineBuffer is not None:
            # We will start by outputing the context variables and then
            # everything that was buffered
            self.log.debug("Outputting hoisted variables")
            for name, var in self.ctxVariables.iteritems():
                if name <> 'inChain':
                    self.log.debug("Outputting variable '%s'" % (name))
                    self.outFile.write(self.indentIncrement + var.declLine + ' /* Hoisted; originally on line %d */\n' % (var.lineNo))
            self.log.debug("Dumping buffered lines")
            for bline in self.lineBuffer:
                self.outFile.write(bline)
            self.lineBuffer = None
        self._writeLine(lineNo, line)
        self.inFunction = None
        return True

    def _ctxVarCallback(self, matchObject, line, lineNo):
        self.log.debug("Context variable callback on line %d" % (lineNo))
        varname = matchObject.group('varname')
        vartype = matchObject.group('type') + matchObject.group('ptr').replace(' ', '')
        ctxVar = self.ctxVariables.get(varname, None)
        if ctxVar is not None:
            self.log.error("Variable '%s' on line %d is a duplicate. Previous declaration on line %d"\
                           % (varname, lineNo, ctxVar.lineNo))
            return False
        self.log.debug("Found variable '%s' of type '%s' at line %d" % (varname, vartype, lineNo))
        ctxVar = CtxVar(vartype, varname, matchObject.group('line'), lineNo)
        self.ctxVariables[varname] = ctxVar
        # If buffering, we don't actually emit the variable but just a comment
        self._writeLine(lineNo, matchObject.group('line') + '\n', '/* Variable \'%s\' hoisted */\n' % (varname))
        return True

    def _waitCallback(self, matchObject, line, lineNo):
        global gProcessMode
        self.log.debug("WAIT_EVT callback on line %d" % (lineNo))
        numEvts = int(matchObject.group('numEvts'))
        inVars = (v.strip() for v in matchObject.group('vars').split(','))
        if numEvts <> 1:
            self.log.error("Currently only one event is supports in WAIT_EVT. Got %d events" % (numEvts))
            return False
        ctxEvts = []    # Events that we need to wait for
        ctxEvtVars = [] # Other events that need to be carried forward
        ctxVars = []    # Variables that need to be carried forward
        inChainStatus = -1 # Status of the inChain variable: -1: no present; 0: in ctxEvtVars; 1: in ctxEvts
        allNames = set()
        allNamesLen = 0
        pastEvts = False # Will be true when we start getting into regular context variables (non events)
        for ctxVar in inVars:
            allNames.add(ctxVar)
            if len(allNames) == allNamesLen:
                self.log.error("'%s' is listed twice (used in WAIT_EVT on line %d)" % (ctxVar, lineNo))
                return False
            allNamesLen += 1
            if numEvts > 0:
                # This is an event
                ctxVarObj = self.ctxVariables.get(ctxVar, None)
                if ctxVarObj is None:
                    self.log.error("'%s' is not a context variable (used in WAIT_EVT on line %d)" % (ctxVar, lineNo))
                    return False
                if not ctxVarObj.isEvt:
                    self.log.error("'%s' is not an event (used as an event in WAIT_EVT on line %d)" % (ctxVar, lineNo))
                    return False
                numEvts -= 1
                if ctxVar == 'inChain':
                    self.log.debug("Appended inChain event '%s'" % (ctxVarObj.varName))
                    if inChainStatus <> -1:
                        self.log.error("'%s' is listed twice in the event list (used in WAIT_EVT on line %d)" % (ctxVar, lineNo))
                        return False
                    inChainStatus = 1
                else:
                    self.log.debug("Appended regular event '%s'" % (ctxVarObj.varName))
                ctxEvts.append(ctxVarObj)
            else:
                # This is just a context variable. By convention, events must come first
                ctxVarObj = self.ctxVariables.get(ctxVar, None)
                if ctxVarObj is None:
                    self.log.error("'%s' is not a context variable (used in WAIT_EVT on line %d)" % (ctxVar, lineNo))
                    return False
                if ctxVarObj.isEvt:
                    if pastEvts:
                        self.log.error("Event '%s' listed after non-events (used in WAIT_EVT on line %d)" % (ctxVar, lineNo))
                        return False
                    self.log.debug("Appended event '%s' as a context event" % (ctxVarObj.varName))
                    ctxEvtVars.append(ctxVarObj)
                else:
                    pastEvts = True
                    self.log.debug("Appended '%s' as a context variable" % (ctxVarObj.varName))
                    ctxVars.append(ctxVarObj)
            # End numEvts == 0
        # else inVars for loop
        if inChainStatus == -1:
            self.log.warning("'inChain' is not present; adding as a context event (in WAIT_EVT on line %d)" % (lineNo))
            ctxEvtVars.append(self.ctxVariables.get('inChain', None))
            inChainStatus = 0
        # Start outputing the text for the wait. First line is just to tell the user what is happening
        lines = ['%s/* *** %s { *** */' % (self.curIndent, matchObject.group('line'))]
        # We use outChain to store the event we need to use to recover from
        lines.append('%soutChain = (pdEvent_t*)%s;' % (self.curIndent, ctxEvts[0].varName))
        if gProcessMode.ctxMode <> ProcessMode.CTXOPTI:
            # We need to save the context so we can access it after the end of the scope
            # _continuation is always "empty" because it was allocated in START_FUNC. Note that
            # it can't be used up by a previous UNWIND because it would have been cleared
            # at the beginning of the case statement
            lines.extend(self._addCtxEvents(self.curIndent, '_continuation', ctxEvtVars))
            lines.extend(self._addCtxVars(self.curIndent, '_continuation', ctxVars))
        lines.append(Template(\
"""${indent}if(outChain->properties & PDEVT_IS_READY) {
${indent1}/* Continue execution, nothing to do */
${indent1}_fallthrough = true;
${indent}} else {
${indent1}/* We need to block here */"""
                          ).substitute(indent=self.curIndent, indent1=self.curIndent+self.indentIncrement))
        if gProcessMode.ctxMode == ProcessMode.CTXOPTI:
            # Since we are blocking, we need to create the context properly
            lines.extend(self._addCtxEvents(self.curIndent + self.indentIncrement, '_continuation', ctxEvtVars))
            lines.extend(self._addCtxVars(self.curIndent + self.indentIncrement, '_continuation', ctxVars))
        lines.append(Template(\
"""${indent1}_blockedNextJump = __LINE__ + 4;
${indent1}goto blockedReturn;
${indent}}
${indent}} /* End of case statement */
${indent}case __LINE__: {
${indent}/* _continuation always contains the context whether this was
${indent} * a fallthrough (saved above) or not (set in START_FUNC). Similarly,
${indent} * outChain contains whatever we were waiting on */"""
                          ).substitute(indent=self.curIndent, indent1=self.curIndent + self.indentIncrement))
        # If not optimized context, reset everything. This works whether or not this is a fall through
        if gProcessMode.ctxMode <> ProcessMode.CTXOPTI:
            # Extract things from _continuation
            lines.extend(self._extractCtxEvents(self.curIndent, '_continuation', ctxEvtVars, True))
            lines.extend(self._extractCtxVars(self.curIndent, '_continuation', ctxVars, True))
            # If the event we are waiting on is not inChain, we need to redeclare it
            if ctxEvts[0].varName <> 'inChain':
                lines.append('%s%s %s = NULL;' % (self.curIndent, ctxEvts[0].varType, ctxEvts[0].varName))
        # Now evaluate whether this is a fall through to determine what to do in the optimized case
        lines.append('%sif(_fallthrough) {' % (self.curIndent))
        # _fallthrough case
        if gProcessMode.ctxMode <> ProcessMode.CTXOPTI:
            # Here, we need to restore continuation and what not
            lines.append('%s/* FREE _continuation->evtCtx */;' % (self.curIndent + 1*self.indentIncrement))
            lines.append('%soutChain = inChain;' % (self.curIndent + 1*self.indentIncrement))
        else:
            lines.append('%s/* Nothing to do; continue execution */' % (self.curIndent + 1*self.indentIncrement))
        lines.append('%s} else {' % (self.curIndent))
        # !_fallthrough case
        if gProcessMode.ctxMode == ProcessMode.CTXOPTI:
            # We need to restore from _continuation
            lines.extend(self._extractCtxEvents(self.curIndent + 1*self.indentIncrement, '_continuation', ctxEvtVars, False))
            lines.extend(self._extractCtxVars(self.curIndent + 1*self.indentIncrement, '_continuation', ctxVars, False))
        # Restore the event we were waiting on (this may be inChain in case this is moot but won't break things)
        lines.append('%s%s = outChain;' % (self.curIndent + 1*self.indentIncrement, ctxEvts[0].varName))
        # Clear the continuation
        lines.append('%s/* FREE _continuation->evtCtx */;' % (self.curIndent + 1*self.indentIncrement))
        lines.append('%s}\n%s/* *** END %s ***/\n' % (self.curIndent, self.curIndent, matchObject.group('line')))
        self._writeLine(lineNo, '\n'.join(lines), noIndent = True)
        return True

    def _addCtxEvents(self, curIndent, varName, evts):
        """Adds the events in evts to evtCtx in varName->evtCtx"""
        global gProcessMode
        lines = ['%s{' % (curIndent)]
        origIndent = curIndent
        curIndent += self.indentIncrement
        if len(evts) > gProcessMode.macros['PDEVT_LIST_SIZE']:
            self.log.debug("Creating event context -- more than %d events; creating extended list event" % (gProcessMode.macros['PDEVT_LIST_SIZE']))
            lines.append('%spdEventList_t *_listEvt = /* ALLOCATE of SIZE pdEventList_t + (len(evts) - 4)*sizeof(pdEvent_t*) */;' % (curIndent))
        else:
            self.log.debug("Creating event context -- using single list event")
            lines.append('%spdEventList_t *_listEvt = /* ALLOCATE of SIZE pdEventList_t */' % (curIndent))
        # "Header" information
        lines.append(Template(\
"""${indent}_listEvt->count = ${evtCount};"""
                          ).substitute(indent=curIndent, evtCount=len(evts)))
        # Add each event.
        evtCount = 0
        evtMaxCount = gProcessMode.macros['PDEVT_LIST_SIZE']
        for evt in evts:
            #First the four PDEVT_LIST_SIZE
            if evtMaxCount > 0:
                lines.append('%s_listEvt->events[%d] = %s;' % (curIndent, evtCount, evt.varName))
                evtCount += 1
                if evtCount >= evtMaxCount:
                    # Move on to the 'next' field
                    evtCount = 0
                    evtMaxCount = -1
            else:
                # Now the other ones
                lines.append('%s_listEvt->next[%d] = %s;' % (curIndent, evtCount, evt.varName))
                evtCount += 1
        # "Footer" information
        lines.append(Template(\
"""${indent}${name}->evtCtx = _listEvt;
${oldIndent}}"""
                          ).substitute(indent=curIndent, name=varName, oldIndent=origIndent))
        return lines

    def _extractCtxEvents(self, curIndent, varName, evts, extractType = True):
        """Reverse operation to _addCtxEvents"""
        global gProcessMode
        lines = []
        # "Header" information
        lines.append(Template(\
"""${indent}pdEventList_t *_tlistEvt = ${name}->evtCtx;"""
                          ).substitute(indent=curIndent, name=varName))
        # Extract each variable
        evtCount = 0
        maxCount = gProcessMode.macros['PDEVT_LIST_SIZE']
        for evt in evts:
            if evtCount < maxCount:
                if evt.varName == 'inChain':
                    lines.append('%sinChain = _tlistEvt->events[%d];' % (curIndent, evtCount))
                else:
                    lines.append('%s%s%s = _tlistEvt->events[%d];' % (
                        curIndent, evt.varType + ' ' if extractType else '', evt.varName, evtCount))
            else:
                if evt.varName == 'inChain':
                    lines.append('%sinChain = _tlistEvt->next[%d];' % (curIndent, evtCount - maxCount))
                else:
                    lines.append('%s%s%s = _tlistEvt->next[%d];' % (
                        curIndent, evt.varType + ' ' if extractType else '', evt.varName, evtCount - maxCount))
            # end evtCount >= maxCount
        # end for over evts
        return lines

    def _addCtxVars(self, curIndent, varName, variables):
        """Similar to _addCtxEvents but adds things to varName->scratch"""
        global gProcessMode
        lines = ['%s{' % (curIndent)]
        origIndent = curIndent
        curIndent += self.indentIncrement
        # "Header" information
        lines.append(Template(\
"""${indent}char* _tScratch = ${name}->scratch;"""
                          ).substitute(indent=curIndent, name=varName))
        # Add each variable
        varOffsets = [] # Sizeof the variables being stored
        for var in variables:
            # Note that to keep things simple, we align everything at 8 byte boundaries
            # We could optimize if we knew more about sizes but this should be safe
            lines.append(Template(\
"""${indent}*(${vType}*)(_tScratch) = ${vName};
${indent}_tScratch += ((sizeof(${vType}) + 7) & ~0x7);"""
                              ).substitute(indent=curIndent, vType=var.varType, vName=var.varName))
            varOffsets.append('((sizeof(%s) + 7) & 0x7)' % (var.varType))
        # "Footer" information
        lines.append(Template(\
"""${indent}/* If the following assert fails, you have too much context to save.
${indent} * Increase PDEVT_SCRATCH_BYTES or restrict the number of variables
${indent} * in the context */
${indent}COMPILE_ASSERT((${typeLen}) < PDEVT_SCRATCH_BYTES);
${oldIndent}}"""
                          ).substitute(indent=curIndent, typeLen='+'.join(varOffsets), oldIndent=origIndent))
        return lines

    def _extractCtxVars(self, curIndent, varName, variables, extractType = True):
        """Reverse operation to _addCtxVars"""
        global gProcessMode
        lines = []
        # "Header" information
        lines.append(Template(\
"""${indent}char* _tScratch = ${name}->scratch;"""
                          ).substitute(indent=curIndent, name=varName))
        # Extract each variable
        for var in variables:
            lines.append(Template(\
"""${indent}${vType1} ${vName} = *(${vType}*)_tScratch;
${indent}_tScratch += ((sizeof(${vType}) + 7) & ~0x7);"""
                              ).substitute(indent=curIndent, vType1=var.varType if extractType else '',
                vType=var.varType, vName=var.varName))
        return lines


    def _writeLine(self, lineNo, line, lineBuffered = None, noIndent = False):
        if self.lineBuffer is not None:
            if lineBuffered is None:
                lineBuffered = line
            self.log.debug("Buffering line %d" % (lineNo))
            # curIndent will be None if no match was found
            if (not noIndent) and (self.curIndent is not None):
                self.lineBuffer.append(self.curIndent + lineBuffered.lstrip())
            else:
                self.lineBuffer.append(lineBuffered)
        else:
            self.log.debug("Outputting line %d" % (lineNo))
            if (not noIndent) and (self.curIndent is not None):
                self.outFile.write(self.curIndent + line.lstrip())
            else:
                self.outFile.write(line)

# Main function to rewrite a file
def processInputFile():
    # First create a temporary file that we can use to write our
    # output to
    with tempfile.NamedTemporaryFile(dir=os.path.dirname(gInputFileName), prefix='OCRRTPP_', suffix='.c', delete=False) as outFile:
        state = ParseState(outFile)
        counter = 1
        for line in gInputFile:
            state.parseLine(line, counter)
            counter += 1
    shutil.copy2(outFile.name, gInputFileName.replace('.c', '_pp.c'))



def main(argv=None):
    global gInputFileName, gInputFile, gProcessMode

    myLog = logging.getLogger()

    if argv is None:
        argv = sys.argv

    myLog.info("---- Starting pre-processing of source file ----")

    try:
        try:
            opts, args = getopt.getopt(argv[1:], "hdf:m:v:", [
                "help", "full-help", "debug", "file=", "mode=", "var="])
        except getopt.error, err:
            raise Usage(err)
        # Parse the options
        for o, a in opts:
            if o in ("-h", "--help", "--full-help"):
                raise Usage(\
"""
    -h,--help:      Prints this message
    --full-help:    Prints this message (for now)
    -d,--debug:     Enable debugging of this script (much more verbose)
    -f,--file:      Input file to process. This should be a C fil
    -m,--mode:      A comma separated list of modes to generate the code.
                    Currently supported:
                      - one of:
                        - optimized: Optimized code generation; all variables will be moved
                                     to the top of the function to avoid copies on the fast
                                     path
                        - ctxcheck:  Variables are declared within each continuation scope
                                     to generate compiler errors if the context is ill-defined
                                     (default)
                      - optionally:
                        - trace:     DPRINTF statements are inserted to allow the tracing of
                                     continuation execution in OCR (default)
    -v,--var:       Values for a macro variable used in the C code. Currently supported are:
                      - PDEVT_SCRATCH_BYTES: size of the scratch for continuation tasks. Defaults to 1024 bytes
                      - PDEVT_MERGE_SIZE   : number of events in a merge event. Defaults to 4
                      - PDEVT_LIST_SIZE    : number of events in a list event. Defaults to 4
                    The option can be specified as many times as needed. The later value overrides any
                    previously set value. For example: -v PDEVT_SCRATCH_BYTES=2048
""")
            elif o in ("-d", "--debug"):
                streamHandlerStdOut.setLevel(logging.DEBUG)
            elif o in ("-f", "--file"):
                gInputFileName = a
            elif o in ("-m", "--mode"):
                gProcessMode.parseMode(a)
            elif o in ("-v", "--var"):
                try:
                    vName, vValue = [s.strip() for s in a.split('=')]
                    vValue = int(vValue)
                except:
                    raise Usage("Illegal format for variable '%s'. Expected 'name=value' where value is an integer" % (a))
                gProcessMode.setVariable(vName, int(vValue))
            else:
                raise Usage("Unhandled option")
        # End loop over opts
        if args is not None and len(args) > 0:
            raise Usage("Extraneous arguments '%s'" % (args))

        # Check that we have everything we need
        # Check the file
        if gInputFileName is None:
            raise Usage("Missing input file")
        try:
            gInputFileName = os.path.expandvars(os.path.expanduser(gInputFileName))
            gInputFile = open(gInputFileName, 'r')
        except IOError:
            raise Usage("Could not open file '%s' for reading" % (gInputFileName))

        # Check the operation mode
        gProcessMode.validateMode()

        # Process the file
        processInputFile()
    except Usage, msg:
        print >> sys.stderr, msg.msg
        print >> sys.stderr, "For help, use -h"
        return 2


# Starts the program
if __name__ == '__main__':
    sys.exit(main())
