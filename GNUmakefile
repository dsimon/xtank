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
# Top Level GNUmakefile
#

SUBDIR= Src

all clean install phase1-headless phase1-server phase1-server-net phase1-core:
	cd ${SUBDIR} && ${MAKE} $@

# ---------------------------------------------------------------------------
# P1.16 -- Phase 1 exit gate.  Runs the frozen-pin X versus headless
# characterization (the full regression oracle), then the geometry, intent,
# lifecycle, content, package and X-boundary acceptance checks.  The 15 Hz
# timing measurement (Tests/phase1/timing.sh) and the 60-minute bot soak
# (Tests/phase1/bots.sh) are recorded separately and are NOT part of this
# target, per the P1.16 contract.
#
# All dumps, logs, binaries and reference worktrees are kept outside the
# checkout under ${XTANK_P1_EVIDENCE:-/tmp/opencode/xtank-phase1/p1.16}.
#
# Usage: make phase1-check
# ---------------------------------------------------------------------------
PHASE1_EVIDENCE ?= /tmp/opencode/xtank-phase1/p1.16
PHASE1_SUBJECT  ?= null

phase1-check:
	@set -e; \
	export XTANK_P1_EVIDENCE="$(PHASE1_EVIDENCE)"; \
	export TAR_OPTIONS="$${TAR_OPTIONS:---no-same-owner}"; \
	mkdir -p "$$XTANK_P1_EVIDENCE"; \
	echo "== P1.16 phase1-check (subject $(PHASE1_SUBJECT))"; \
	echo "== full oracle: frozen X vs headless characterization"; \
	bash Tests/phase1/regression.sh --suite full --stage after --subject $(PHASE1_SUBJECT); \
	echo "== geometry"; bash Tests/phase1/geometry.sh; \
	echo "== intent";   bash Tests/phase1/intent.sh; \
	echo "== lifecycle"; bash Tests/phase1/lifecycle.sh; \
	echo "== content";  bash Tests/phase1/content.sh; \
	echo "== package";  bash Tests/phase1/package.sh; \
	echo "== boundary: headers"; bash Tests/phase1/check-x-boundary.sh --headers; \
	echo "== boundary: sources"; bash Tests/phase1/check-x-boundary.sh --sources; \
	echo "== boundary: build";   bash Tests/phase1/check-x-boundary.sh --build; \
	echo "== boundary: runtime"; bash Tests/phase1/check-x-boundary.sh --runtime; \
	echo "== P1.16 PHASE1-CHECK PASS"

# ---------------------------------------------------------------------------
# P2.9 -- Phase 2 client shell (SDL3 3.4.18 + ENet 1.3.18 + Protocol codec).
# CMake builds only the new client/protocol/transport shell; the Phase 1
# server, headless and oracle keep the GNUmakefile build above, untouched.
#
# Provide the pinned dependency locations, then:
#   make phase2-client-linux   # native Linux frontend (tests)
#   make phase2-client-win     # MinGW-w64 x86-64 cross-build (D2-D)
#   make phase2-client-clean
#
# XTK_SDL3_LINUX_PREFIX  install prefix of the Linux-built SDL3 3.4.18
# XTK_SDL3_MINGW_PREFIX  x86_64-w64-mingw32/ dir of the SDL3 MinGW devel pkg
# XTK_ENET_DIR           pinned ENet 1.3.18 source tree
# ---------------------------------------------------------------------------
XTK_CMAKE           ?= cmake
XTK_CLIENT_BUILD    ?= /tmp/opencode/xtank-phase2/p2.9/build/root
XTK_SDL3_LINUX_PREFIX ?=
XTK_SDL3_MINGW_PREFIX ?=
XTK_ENET_DIR          ?=

phase2-client-linux:
	$(XTK_CMAKE) -S . -B $(XTK_CLIENT_BUILD)/linux \
	    -DCMAKE_PREFIX_PATH=$(XTK_SDL3_LINUX_PREFIX) -DXTK_ENET_DIR=$(XTK_ENET_DIR)
	$(XTK_CMAKE) --build $(XTK_CLIENT_BUILD)/linux

phase2-client-win:
	$(XTK_CMAKE) -S . -B $(XTK_CLIENT_BUILD)/win \
	    -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
	    -DCMAKE_PREFIX_PATH=$(XTK_SDL3_MINGW_PREFIX) -DXTK_ENET_DIR=$(XTK_ENET_DIR)
	$(XTK_CMAKE) --build $(XTK_CLIENT_BUILD)/win

phase2-client-clean:
	rm -rf $(XTK_CLIENT_BUILD)

.PHONY: all clean install phase1-headless phase1-server phase1-core \
	phase1-check phase2-client-linux phase2-client-win \
	phase2-client-clean ${SUBDIR}
