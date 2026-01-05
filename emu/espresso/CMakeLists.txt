
set(HEADERS
        copyright.h  espresso.h  main.h  mincov.h  mincov_int.h  port.h  signature.h  sparse.h  sparse_int.h  utility.h
)

set(SOURCES
        black_white.c cols.c cpu_time.c cvrm.c dominate.c espresso_expand.c essentiality.c gimpel.c indep.c matrix.c
        pair.c prtime.c set.c sigma.c sminterf.c unate.c canonical.c compl.c cubestr.c cvrmisc.c equiv.c
        espresso_main.c exact.c globals.c irred.c mincov.c part.c reduce.c setc.c signature.c solution.c
        util_signature.c cofactor.c contain.c cvrin.c cvrout.c espresso.c essen.c gasp.c hack.c map.c opo.c primes.c
        rows.c sharp.c signature_exact.c sparse.c verify.c
)

add_library(espresso STATIC ${HEADERS} ${SOURCES})
