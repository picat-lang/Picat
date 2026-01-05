
set(HEADERS
        bapi.h bprolog_cg_cgevent.h build.h dynamic.h extern_decl.h gc.h kapi.h load_inst.h picat.h sapi.h basicd.h
        bprolog.h clpfd.h emu_inst.h frame.h getline.h load_inst_frombplist.h maple_interface.h picat_utilities.h
        term.h basic.h bprolog_plc_plc.h dis_inst.h event.h fzn_picat_smt_bc.h inst.h load_inst_fromcarray.h picat_bc.h
        reasonls_interface.h toam.h
)

set(SOURCES
        arith.c builtins.c clpfd_libs.c dis.c expand_bp.c float1.c getline.c init_sym.c main.c
        toamprofile.c assert_bp.c cfd.c cpreds.c domain.c expand.c gcheap.c global.c
        inst_inf.c kissat_picat.c mic.c table.c token.c assert.c clause.c debug.c espresso_bp.c file.c gcqueue.c
        glpk_bp.c jmp_table.c loader.c numbervars.c sapi.c unify.c bigint.c clpfd.c delay.c event.c findall.c
        gcstack.c init.c kapi.c load_inst.c picat_utilities.c toam.c univ.c
)

add_subdirectory(kissat)
add_subdirectory(espresso)

add_executable(picat ${HEADERS} ${SOURCES})
target_link_libraries(picat PRIVATE m pthread espresso kissat)
target_include_directories(picat PRIVATE "${CMAKE_SOURCE_DIR}/emu/espresso")

install(TARGETS picat)
