# Generators

Setup and the full regeneration procedure: docs/DEVSETUP.md
(`setup_ikemen.sh` builds the Ikemen GO tree, `gotoc/regen.sh` regenerates
all generated C; `unsupported_allowed.txt` lists accepted library placeholders).

- `gen_opcodes.py BYTECODE_GO`: opcode numbers (src/core/opcodes.h).
- `gencdefs/`: C declarations of Ikemen's data model from Go type
  information (src/core/gen_types.h, gen_consts.h). Run from the Ikemen
  tree with Go >= 1.27 sources:

      GENCDEFS_OVERRIDES=overrides.txt gencdefs ./src gen_consts.h gen_types.h \
          Char Explod Projectile CharGlobalInfo

  `overrides.txt` maps Go types to engine-native C types.
  Embedded structs become a union of the whole value and its promoted
  fields, so both `c->hitPauseTime` and `c->CharSystemVar` work.
