
set(HEADERS
        allocate.h autarky.h clause.h deduce.h error.h frames.h inline.h kitten.h parse.h propdense.h reduce.h
        resources.h strengthen.h value.h xors.h analyze.h averages.h clueue.h definition.h extend.h gates.h
        inlineheap.h learn.h phases.h prophyper.h reference.h restart.h substitute.h vector.h ands.h backbone.h
        collect.h dense.h failed.h handle.h inlinequeue.h limits.h print.h proplit.h reluctant.h search.h sweep.h
        vivify.h application.h backtrack.h colors.h dominate.h fastassign.h heap.h inlinevector.h literal.h probe.h
        proprobe.h rephase.h shrink.h terminate.h walk.h arena.h backward.h compact.h eliminate.h file.h ifthenelse.h
        internal.h logging.h profile.h propsearch.h report.h smooth.h ternary.h warmup.h array.h build.h config.h
        endianess.h flags.h import.h kimits.h minimize.h promote.h queue.h require.h sort.h trail.h watch.h assign.h
        bump.h cover.h endianness.h format.h inlineassign.h kissat_bool.h mode.h proof.h random.h resize.h stack.h
        transitive.h weaken.h attribute.h check.h decide.h equivalences.h forward.h inlineframes.h kissat.h options.h
        propbeyond.h rank.h resolve.h statistics.h utilities.h witness.h
)

set(SOURCES
        allocate.c analyze.c ands.c application.c arena.c assign.c autarky.c averages.c backtrack.c backward.c
        build.c bump.c check.c clause.c clueue.c collect.c colors.c compact.c config.c decide.c deduce.c dense.c
        dominate.c dump.c eliminate.c equivalences.c error.c extend.c failed.c file.c flags.c format.c forward.c
        frames.c gates.c handle.c heap.c ifthenelse.c import.c internal.c learn.c limits.c logging.c main.c minimize.c
        mode.c options.c parse.c phases.c print.c probe.c profile.c promote.c proof.c propdense.c prophyper.c
        proprobe.c propsearch.c queue.c reduce.c reluctant.c rephase.c report.c resize.c resolve.c resources.c
        restart.c search.c smooth.c sort.c stack.c statistics.c strengthen.c substitute.c terminate.c ternary.c
        trail.c transitive.c utilities.c vector.c vivify.c walk.c watch.c weaken.c witness.c xors.c
)

add_library(kissat STATIC ${HEADERS} ${SOURCES})
