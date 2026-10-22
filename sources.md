# sources.md

All sources from which code or information was taken (references for this
project). Nothing here is vendored code; the project builds on the Picat
`experimental` branch and its own aspic/udf features.

## Upstream repository

- Picat language, `experimental` branch (local branch `aspicudf`, commit
  0d10ad0 "release: rebuild the linux64 tgz at 5e3eeb6"):
  https://github.com/picat-lang/Picat.git
  - aspic feature: `aspic/` (transpiler, embedded flow, runtime template)
  - udf feature: `lib2/udf.pi`, `lib2/udf_ops*.pi`, `doc/udf.tex`

## Documentation used

- `README.md` (experimental-branch overview; section 5 `udf`)
- `aspic/README.md` (the ASP-to-Picat transpiler and PICASP embedded flow)
- `doc/udf.tex` (user-defined functions in constraints)
- Headers of `exs/{cp,sat,mip,smt,fp}/udf_*.pi` (usage examples)

## Code provenance

- All aspic/udf source files in this repo come from the upstream branch
  listed above. The v0.4.0 rewrite (this project's work) modified:
  - `aspic/aspic_transpiler.pi`, `aspic/aspic_gen.pi` — new emitter shape
    (one udf term per aggregate/objective, `aspic_instances` enumeration,
    shimmed `'#='`)
  - `aspic/aspic_prep.pi` — shim splicing for the embedded flow
  - `aspic/aspic_runtime_template.pi` — `import udf.`, `aspic_flatten`,
    `aspic_udf_init` ($when/$nwhen/$suml definitions)
  - `aspic/test/test_aspic_syntax.sh` — PICATPATH + awk harness fixes
- The udf construct experiments (scratch/udfexp/exp1–exp13.pi) are original
  test programs written for this project, exercising documented udf behavior
  from `doc/udf.tex` and the udf module docstring.

## External tools used for verification

- Picat binary 4.0b8 (commit 9796071), installed at /home/jovyan/bin/picat
- clingo 5.8.2 (available in the environment; used by
  `aspic/test/clingo_examples.py` as a cross-check of example solutions)
