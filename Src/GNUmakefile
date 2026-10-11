#/*-
# * Copyright (c) 2016, 2017 Kurt Lidl
# * All rights reserved.
# *
# * Redistribution and use in source and binary forms, with or without
# * modification, are permitted provided that the following conditions
# * are met:
# * 1. Redistributions of source code must retain the above copyright
# *    notice, this list of conditions and the following disclaimer.
# * 2. Redistributions in binary form must reproduce the above copyright
# *    notice, this list of conditions and the following disclaimer in the
# *    documentation and/or other materials provided with the distribution.
# *
# * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
# * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
# * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# * SUCH DAMAGE.
# */

#
# GNUmakefile for the 'xtank' program
#

.CURDIR= .
PREFIX=/usr/local

# When this was built using Imake, the Display, Programs and Threads
# directories were built into different libraries.  Here, we're just
# going to build them all into this directory (and, for P1.5, two isolated
# object trees) and forgo linking the code into libraries.

VPATH=	${.CURDIR}/Display:${.CURDIR}/Programs:${.CURDIR}/Threads

ifneq ("$(wildcard /opt/X11/include)","")
X11INC= -I/opt/X11/include -D_XOPEN_SOURCE
LDFLAGS+= -L/opt/X11/lib
endif
ifneq ("$(wildcard /usr/local/include/X11)","")
X11INC+= -I/usr/local/include
LDFLAGS+= -L/usr/local/lib
endif

include ${.CURDIR}/Makefile.inc1

ifeq ($(shell uname),Linux)
LDLIBS+= -ldl
endif

# ---------------------------------------------------------------------------
# Flags for the two builds: the X front-end and the display-free server.
# ---------------------------------------------------------------------------
XCFLAGS= -I${.CURDIR} -I${.CURDIR}/Include -I/usr/local/include -O0 -g \
	${X11INC} ${COMMON_DEFS} ${X_DEFS} -DXTANK_DIR="\"${.CURDIR}/..\""
NCFLAGS= -I${.CURDIR} -I${.CURDIR}/Include -I/usr/local/include -O0 -g \
	${COMMON_DEFS} ${NULL_DEFS} -DXTANK_DIR="\"${.CURDIR}/..\""
XCFLAGS+= -DALLDEFINES="\"X11 frontend (P1.5)\""
NCFLAGS+= -DALLDEFINES="\"null backend server (P1.5)\""
NCFLAGS+= -MMD -MP

# Shipped local server (P1.15): display-free, terminal-free, no DYNALOAD,
# only the drone program, no designers and no X front-end entry point.
SCFLAGS= -I${.CURDIR} -I${.CURDIR}/Include -I/usr/local/include -O0 -g \
	${SERVER_COMMON_DEFS} ${NULL_DEFS} ${SERVER_ONLY_DEFS} \
	-DXTANK_DIR="\"${.CURDIR}/..\""
SCFLAGS+= -DALLDEFINES="\"local server (P1.15)\""
SCFLAGS+= -MMD -MP

XOBJ=	xobj
NOBJ=	nobj
SOBJ=	sobj

XOBJS=	$(addprefix ${XOBJ}/,$(SRCS:.c=.o))
NOBJS=	$(addprefix ${NOBJ}/,$(SERVER_SRCS:.c=.o))

# The reusable core object set plus the server's own entry point.
CORE_SRCS= ${SERVER_CORE_SRCS} server_main.c
SOBJS=	$(addprefix ${SOBJ}/,$(CORE_SRCS:.c=.o))

XTANK=	xtank
XTANK_HEADLESS=	xtank-headless
XTANK_SERVER=	xtank-server

all:	${XTANK} ${XTANK_HEADLESS} ${XTANK_SERVER}

phase1-headless: ${XTANK_HEADLESS}

phase1-server: ${XTANK_SERVER}

# Reusable display-free simulation core (server object set without the entry).
phase1-core: ${SOBJS}

# P1.16 -- forward the Phase 1 exit gate to the top-level GNUmakefile, where
# the orchestrator lives (the Tests/phase1 scripts resolve paths from the
# repository root).
phase1-check:
	cd ${.CURDIR}/.. && ${MAKE} phase1-check

install: ${XTANK}
	mkdir -p ${DESTDIR}${PREFIX}/bin
	install -s ${XTANK} ${DESTDIR}${PREFIX}/bin

${XTANK}: ${XOBJS}
	${CC} ${LDFLAGS} $^ -lX11 -lm ${LDLIBS} -o $@

${XTANK_HEADLESS}: ${NOBJS}
	${CC} ${LDFLAGS} $^ -lm ${LDLIBS} -o $@

# The shipped server links the display-free core and its own main(); no
# DYNALOAD (-ldl), no Xlib (-lX11) and no editors/front-end entry point.
${XTANK_SERVER}: ${SOBJS}
	${CC} ${LDFLAGS} $^ -lm -o $@

# The server object tree records compiler dependency files (.d) that
# Tests/phase1/check-x-boundary.sh --build inspects.
${NOBJ}/%.o: %.c
	@mkdir -p ${NOBJ}
	${CC} ${NCFLAGS} -c $< -o $@

${SOBJ}/%.o: %.c
	@mkdir -p ${SOBJ}
	${CC} ${SCFLAGS} -c $< -o $@

${XOBJ}/%.o: %.c
	@mkdir -p ${XOBJ}
	${CC} ${XCFLAGS} -c $< -o $@

# Print the explicit server translation-unit list (used by the boundary
# checker); does not build anything.
print-server-srcs:
	@echo $(SERVER_SRCS)

# Print the shipped server's reusable core translation units (no entry point).
print-core-srcs:
	@echo $(SERVER_CORE_SRCS)

# Server dependency files (from -MMD) so backend includes (e.g. Display/null.c)
# are tracked; also what check-x-boundary.sh --build inspects.
-include $(NOBJS:.o=.d)
-include $(SOBJS:.o=.d)

include ${.CURDIR}/Makefile.inc2

clean:
	rm -f ${XTANK} ${XTANK_HEADLESS} ${XTANK_SERVER} ${CLEANFILES} *.core
	rm -rf ${XOBJ} ${NOBJ} ${SOBJ}

.PHONY: all clean install phase1-headless phase1-server phase1-core \
	phase1-check print-server-srcs print-core-srcs
