# Palisade

Palisade is a research C language extension and compiler for selective object
protection on Armv8.1-M microcontrollers. It targets non-control-data attacks
that utilizes memory errors to corrupt security-sensitive state while
preserving valid control flow.

The design for C language extension introduces a type qualifier that distinguishes
ordinary and protected logical storage for memory objects and their references. it
allows compiler to issue different write instruction to achieve runtime isolation.
Compiler also uses pointer authentication (PAC) to enforce the integrity
of protected references. Selective protection aims to preserve designated
security invariants within MCU runtime and memory budgets. A more comprehensive of
design description is in this
[RFC issue](https://github.com/URSec/palisade-compiler/issues/1).

**Status:** work in progress.

## Source

Imported from and rebased onto
[LLVM 23.1.1](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.1),
tag `llvmorg-23.1.1`, commit `6dfe1677ab8dffbc6ec13d53a1e0215d75147689`.

## Build

Requires Bash, CMake 3.20+, Ninja, Python 3.8+, and a C++17-capable host compiler.
From this directory:

```sh
./scripts/build.sh
```

## License

see LICENSE.TXT
