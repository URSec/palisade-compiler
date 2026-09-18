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
security invariants within MCU runtime and memory budgets.

**Status:** work in progress.

## Source

Imported from [LLVM 23.1.1](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.1),
tag `llvmorg-23.1.1`, commit `6dfe1677ab8dffbc6ec13d53a1e0215d75147689`.

This repository keeps the full upstream LLVM revision history. Palisade commits
sit directly on top of the release commit, so the tree can be rebased onto a
later upstream revision.

## Checkout

Only `clang/`, `llvm/`, `lld/` and their dependencies are needed to build.
The other subprojects are left in the repository and hidden from the working
tree with sparse-checkout, so that upgrading LLVM does not have to replay a
tree-wide deletion:

```sh
./scripts/setup-sparse.sh
```

Run `./scripts/setup-sparse.sh --disable` to restore the full upstream tree.

Collaborators can skip downloading the unused blobs entirely:

```sh
git clone --filter=blob:none --sparse <repository-url>
cd palisade-compiler
git sparse-checkout set scripts
./scripts/setup-sparse.sh
```

## Build

Requires Bash, CMake 3.20+, Ninja, Python 3.8+, and a C++17-capable host compiler.
From this directory:

```sh
./scripts/build.sh
```

## Updating LLVM

```sh
git fetch upstream --tags
git rebase llvmorg-<new-version>
```

## License

see LICENSE.TXT
