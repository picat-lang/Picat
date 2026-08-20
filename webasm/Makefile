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
#   thread.c, satext.c, sapi.c, satshim.c : pthreads / fork-exec external
#                                           solver support.  SAT itself is
#                                           still IN: -DSAT is passed, and
#                                           the satext symbols that
#                                           kissat_picat.c references are
#                                           stubbed in browser_stub.c
#                                           (external solvers stay off)
#   glpk_bp.c scip_picat.c qc.c maple_interface.cpp : LP back-ends and
#   kissat24_picat.c sat_bp.c plc_java.c            : not in the
#                                                      standard build at all
#   jmp_table.c assert.c expand.c load_inst.c temp.c : helpers that are
#                                                      (or unused) in the
#                                                      native build
CORE_O := dis init init_sym loader inst_inf main toam unify file domain \
          cfd float1 arith token global builtins mic numbervars cpreds \
          univ assert_bp findall clause delay clpfd clpfd_libs event \
          toamprofile kapi getline table gcstack gcheap gcqueue debug par \
          expand_bp bigint picat_utilities kissat_picat espresso_bp
# the native KISSAT_OBJ (emu/common.mak): kis_<name>.o <- kissat/src/<name>.c
KISSAT_O := kis_allocate.o kis_analyze.o kis_ands.o kis_application.o \
            kis_arena.o kis_assign.o kis_autarky.o kis_averages.o \
            kis_backtrack.o kis_backward.o kis_build.o kis_bump.o \
            kis_check.o kis_clause.o kis_clueue.o kis_collect.o \
            kis_colors.o kis_compact.o kis_config.o kis_decide.o \
            kis_deduce.o kis_dense.o kis_dominate.o kis_dump.o \
            kis_eliminate.o kis_equivalences.o kis_error.o kis_extend.o \
            kis_failed.o kis_file.o kis_flags.o kis_format.o \
            kis_forward.o kis_frames.o kis_gates.o kis_handle.o \
            kis_heap.o kis_ifthenelse.o kis_import.o kis_internal.o \
            kis_learn.o kis_limits.o kis_logging.o kis_main.o \
            kis_minimize.o kis_mode.o kis_options.o kis_parse.o \
            kis_phases.o kis_print.o kis_probe.o kis_profile.o \
            kis_promote.o kis_proof.o kis_propdense.o kis_prophyper.o \
            kis_proprobe.o kis_propsearch.o kis_queue.o kis_reduce.o \
            kis_reluctant.o kis_rephase.o kis_report.o kis_resize.o \
            kis_resolve.o kis_resources.o kis_restart.o kis_search.o \
            kis_smooth.o kis_sort.o kis_stack.o kis_statistics.o \
            kis_strengthen.o kis_substitute.o kis_terminate.o \
            kis_ternary.o kis_trail.o kis_transitive.o kis_utilities.o \
            kis_vector.o kis_vivify.o kis_walk.o kis_watch.o \
            kis_weaken.o kis_witness.o kis_xors.o
CORE_C := $(addprefix $(EMU)/,$(addsuffix .c,$(CORE_O)))
# kis_<name>.o comes from kissat/src/<name>.c (as in emu/common.mak):
KISSAT_SRC := $(addprefix $(EMU)/kissat/src/,$(KISSAT_O:kis_%.o=%.c))
STAGE_K_O  := $(addprefix $(STAGE)/kissat/,$(KISSAT_O))
STAGE_K_C  := $(KISSAT_SRC:$(EMU)/kissat/src/%.c=$(STAGE)/kissat/src/%.c)

FANN_C := $(EMU)/fann/src/fann.c $(EMU)/fann/src/fann_cascade.c \
          $(EMU)/fann/src/fann_error.c $(EMU)/fann/src/fann_io.c \
          $(EMU)/fann/src/fann_train.c $(EMU)/fann/src/fann_train_data.c
FANN_CPP := $(EMU)/fann/fann_interface.cpp
ESP_C  := $(wildcard $(EMU)/espresso/*.c)

STAGE_C  := $(CORE_C:$(EMU)/%=$(STAGE)/%)
STAGE_E  := $(ESP_C:$(EMU)/%=$(STAGE)/%)
STAGE_F  := $(FANN_C:$(EMU)/%=$(STAGE)/%)
FANN_CPP_O := $(STAGE)/fann/fann_interface.o

# -DSAT: the SAT built-ins register themselves (cpreds.c Cboot) and the
# built-in kissat solver links in; the satext (fork/exec) layer is
# stubbed out in browser_stub.c.
CFLAGS := -O2 -DGC -DGCC -DPICAT -DSAT -Dunix -DPOSIX -DFANN_NO_DLL -DFANN \
          -I$(STAGE) -I$(STAGE)/espresso -I$(STAGE)/fann/src/include
# glibc drags UINT_MAX/INT_MIN in implicitly (kissat's headers use them
# without including <stdint.h>/<limits.h>); force the sysroot limits.h
# by ABSOLUTE path, since a bare -include limits.h would resolve to
# kissat's own limits.h through the -I search path.
SYSROOT_INC := $(abspath $(dir $(EMCC))/cache/sysroot/include)
KISSAT_FLAGS := -O2 -include $(SYSROOT_INC)/limits.h -DNEMBEDDED -DNDEBUG \
                -DNMETRICS -DQUIET -DNSTATISTICS -DNPROOFS \
                -I$(STAGE)/kissat/src

GLUE_O := $(STAGE)/browser_stub.o

# The packed examples in examples/ that read data files do so with
# relative names; the wasm CWD is "/", so the data files are preloaded
# at the filesystem root (no program changes needed).
DATA_SRC := \
  ../exs/euler/names.txt ../exs/euler/words.txt ../exs/euler/triangle.txt \
  ../exs/euler/base_exp.txt \
  ../exs/nn/xor/xor.data ../exs/nn/xor/xor.net \
  ../exs/nn/lang_classify/english1.txt ../exs/nn/lang_classify/english2.txt \
  ../exs/nn/lang_classify/english3.txt ../exs/nn/lang_classify/french1.txt \
  ../exs/nn/lang_classify/french2.txt ../exs/nn/lang_classify/french3.txt \
  ../exs/nn/lang_classify/spanish1.txt ../exs/nn/lang_classify/spanish2.txt \
  ../exs/nn/lang_classify/spanish3.txt ../exs/nn/lang_classify/frequencies.data \
  ../exs/nn/scaling/scaling.data ../exs/nn/scaling/scaling.net \
  ../exs/nn/spam_detector/Train.txt ../exs/nn/spam_detector/TestX.csv \
  ../exs/nn/spam_detector/spam.net \
  ../exs/nn/author_classify/chesterton1.txt ../exs/nn/author_classify/chesterton2.txt \
  ../exs/nn/author_classify/chesterton3.txt ../exs/nn/author_classify/melville1.txt \
  ../exs/nn/author_classify/melville2.txt ../exs/nn/author_classify/melville3.txt \
  ../exs/nn/author_classify/processed_chesterton1.txt \
  ../exs/nn/author_classify/processed_chesterton2.txt \
  ../exs/nn/author_classify/processed_chesterton3.txt \
  ../exs/nn/author_classify/processed_melville1.txt \
  ../exs/nn/author_classify/processed_melville2.txt \
  ../exs/nn/author_classify/processed_melville3.txt
DATA_PRELOADS := $(foreach s,$(DATA_SRC),--preload-file $(s)@/$(notdir $(s)))

EMFLAGS := -sMODULARIZE=1 -sEXPORT_NAME=PicatWasm \
           -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=16777216 \
           -sNO_EXIT_RUNTIME=1 \
           -sINVOKE_RUN=0 \
           -sEXPORTED_FUNCTIONS='["_browser_boot","_browser_rerun"]' \
           -sEXPORTED_RUNTIME_METHODS='["FS","ccall"]' \
           --preload-file $(LIB2)@/lib2 \
           --preload-file examples@/examples \
           $(DATA_PRELOADS) \
           -lc++ -lc++abi

OBJS := $(STAGE_C:$(STAGE)/%.c=$(STAGE)/%.o) $(STAGE_E:$(STAGE)/%.c=$(STAGE)/%.o) \
        $(STAGE_F:$(STAGE)/%.c=$(STAGE)/%.o) $(FANN_CPP_O) $(GLUE_O) $(STAGE_K_O)

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
$(STAGE)/stamp: $(CORE_C) $(ESP_C) $(FANN_C) $(FANN_CPP) $(KISSAT_SRC) \
                $(DATA_SRC) \
                browser_stub.c $(wildcard patches/*.patch)
	rm -rf $(STAGE)
	mkdir -p $(STAGE)/espresso $(STAGE)/fann/src/include $(STAGE)/kissat/src
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
	cp $(KISSAT_SRC) $(STAGE)/kissat/src/
	cp $(wildcard $(EMU)/kissat/src/*.h) $(STAGE)/kissat/src/ \
	    2>/dev/null || true
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

$(STAGE)/kissat/kis_%.o: $(STAGE)/kissat/src/%.c
	$(EMCC) $(KISSAT_FLAGS) -c $< -o $@

# ---- link -------------------------------------------------------------
$(DIST)/picat.js: $(STAGE)/stamp $(OBJS) browser/index.html \
                  $(wildcard examples/*) $(DATA_SRC)
	mkdir -p $(DIST)
	$(EMCC) $(CFLAGS) $(OBJS) $(EMFLAGS) -o $@
	cp browser/index.html $(DIST)/

serve: $(DIST)/picat.js
	@echo "serving $(DIST) on http://localhost:8000 (Ctrl-C to stop)"
	cd $(DIST) && python3 -m http.server 8000

clean:
	rm -rf $(BUILDDIR) $(DIST)

.PHONY: all serve clean
