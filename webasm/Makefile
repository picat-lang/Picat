# webasm/Makefile -- build a browser-runnable (wasm32) Picat with emscripten.
#
# This build is deliberately unmingled with the rest of the tree:
#   1. the C sources are COPIED into build/stage/,
#   2. patches from patches/*.patch (lexicographic order) are applied
#      to that copy only -- the tree under emu/ is never modified,
#   3. emcc compiles the staged copy into dist/picat.js + picat.wasm.
#
# Usage:
#   make            # build dist/picat.js + dist/picat.wasm
#   make serve      # serve dist/ on http://localhost:8000
#   make clean
#
# Prerequisites: an emsdk installation (EMSDK, default ~/emsdk) and
# the precompiled runtime libraries in ../lib2 (as produced by the
# normal native build).

EMSDK ?= $(HOME)/emsdk

ifeq ($(wildcard $(EMSDK)/upstream/emscripten/emcc),)
$(error emcc not found under $(EMSDK); install emsdk and run: emsdk install latest; emsdk activate latest)
endif
EMCC  := $(EMSDK)/upstream/emscripten/emcc
EMCXX  := $(EMSDK)/upstream/emscripten/em++

EMU     := ../emu
LIB2    := ../lib2
BUILDDIR:= build
STAGE   := $(BUILDDIR)/stage
DIST    := dist

# Compile exactly the object list of the native build (emu/common.mak),
# minus the pieces that cannot run in a browser:
#   thread.c, satext.c, sapi.c : pthreads / fork-exec external solvers
#   kissat_picat.c             : SAT built-in (needs -DSAT + external
#                                solvers); without it the SAT stub
#                                predicates report "sat_not_supported"
#   kissat/ (kis_*.c)          : the kissat solver, no longer referenced
#   glpk_bp.c scip_picat.c qc.c maple_interface.cpp : LP back-ends and
#   kissat24_picat.c sat_bp.c plc_java.c            : not in the
#                                                      standard build at all
#   jmp_table.c assert.c expand.c load_inst.c temp.c : helpers that are
#   satshim.c                                 : not compiled separately
#                                                      (or unused) in the
#                                                      native build
CORE_O := dis init init_sym loader inst_inf main toam unify file domain \
          cfd float1 arith token global builtins mic numbervars cpreds \
          univ assert_bp findall clause delay clpfd clpfd_libs event \
          toamprofile kapi getline table gcstack gcheap gcqueue debug par \
          expand_bp bigint picat_utilities
CORE_C := $(addprefix $(EMU)/,$(addsuffix .c,$(CORE_O)))
FANN_C := $(EMU)/fann/src/fann.c $(EMU)/fann/src/fann_cascade.c \
          $(EMU)/fann/src/fann_error.c $(EMU)/fann/src/fann_io.c \
          $(EMU)/fann/src/fann_train.c $(EMU)/fann/src/fann_train_data.c
FANN_CPP := $(EMU)/fann/fann_interface.cpp
ESP_C  := $(wildcard $(EMU)/espresso/*.c)

STAGE_C  := $(CORE_C:$(EMU)/%=$(STAGE)/%)
STAGE_E  := $(ESP_C:$(EMU)/%=$(STAGE)/%)
STAGE_F  := $(FANN_C:$(EMU)/%=$(STAGE)/%)
FANN_CPP_O := $(STAGE)/fann/fann_interface.o

CFLAGS := -O2 -DGC -DGCC -DPICAT -Dunix -DPOSIX -DFANN_NO_DLL -DFANN \
          -I$(STAGE) -I$(STAGE)/espresso -I$(STAGE)/fann/src/include

GLUE_O := $(STAGE)/browser_stub.o

EMFLAGS := -sMODULARIZE=1 -sEXPORT_NAME=PicatWasm \
           -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=16777216 \
           -sNO_EXIT_RUNTIME=1 \
           -sINVOKE_RUN=0 \
           -sEXPORTED_FUNCTIONS='["_browser_boot","_browser_rerun"]' \
           -sEXPORTED_RUNTIME_METHODS='["FS","ccall"]' \
           --preload-file $(LIB2)@/lib2 \
           --preload-file examples@/examples \
           -lc++ -lc++abi

OBJS := $(STAGE_C:$(STAGE)/%.c=$(STAGE)/%.o) $(STAGE_E:$(STAGE)/%.c=$(STAGE)/%.o) \
        $(STAGE_F:$(STAGE)/%.c=$(STAGE)/%.o) $(FANN_CPP_O) $(GLUE_O)

# Two passes: make resolves the pattern rules for build/stage/*.o only
# once the staged .c files exist, so the first pass stages the sources
# and the second compiles + links.
all:
	$(MAKE) $(STAGE)/stamp
	$(MAKE) $(DIST)/picat.js

# ---- stage: copy sources, apply patches to the COPY -----------------
# (add .patch files to patches/ -- they are applied to the staged
#  copy in lexicographic order, never to the tree itself; after
#  adding/removing a patch run `make clean` first)
$(STAGE)/stamp: $(CORE_C) $(ESP_C) $(FANN_C) $(FANN_CPP) browser_stub.c \
                $(wildcard patches/*.patch)
	rm -rf $(STAGE)
	mkdir -p $(STAGE)/espresso $(STAGE)/fann/src/include
	cp $(CORE_C) $(STAGE)/
	# included (not compiled) by toam.c:
	cp $(EMU)/jmp_table.c $(STAGE)/
	# stand-ins for symbols whose sources are excluded (see above):
	cp browser_stub.c $(STAGE)/
	cp $(EMU)/*.h $(STAGE)/
	cp $(ESP_C) $(STAGE)/espresso/
	cp $(wildcard $(EMU)/espresso/*.h) $(STAGE)/espresso/ 2>/dev/null || true
	cp $(FANN_C) $(STAGE)/fann/src/
	cp $(FANN_CPP) $(STAGE)/fann/
	cp $(wildcard $(EMU)/fann/src/include/*.h) $(STAGE)/fann/src/include/
	@for p in $(wildcard patches/*.patch); do \
	  echo "applying $$p to staged copy"; \
	  patch -p1 -d $(STAGE) --batch < $$p || exit 1; \
	done
	touch $@

# ---- compile (parallel-friendly) -------------------------------------
$(STAGE)/%.o: $(STAGE)/%.c
	$(EMCC) $(CFLAGS) -c $< -o $@

$(STAGE)/espresso/%.o: $(STAGE)/espresso/%.c
	$(EMCC) $(CFLAGS) -c $< -o $@

$(FANN_CPP_O): $(STAGE)/fann/fann_interface.cpp
	$(EMCXX) $(CFLAGS) -c $< -o $@

# ---- link -------------------------------------------------------------
$(DIST)/picat.js: $(STAGE)/stamp $(OBJS) browser/index.html examples
	mkdir -p $(DIST)
	$(EMCC) $(CFLAGS) $(OBJS) $(EMFLAGS) -o $@
	cp browser/index.html $(DIST)/

serve: $(DIST)/picat.js
	@echo "serving $(DIST) on http://localhost:8000 (Ctrl-C to stop)"
	cd $(DIST) && python3 -m http.server 8000

clean:
	rm -rf $(BUILDDIR) $(DIST)

.PHONY: all serve clean
