# Alpha Compiler & VM

## Overview

This project implements a compiler and virtual machine for Alpha, developed as part of the HY-340 (Compiler Construction) course at the University of Crete. The system takes Alpha source code and compiles it down to bytecode, then runs that bytecode on a custom-built virtual machine.

The main goal was to build a working compiler pipeline from scratch, generating intermediate code directly during parsing instead of building a separate AST pass.

## Key Features

Lexical & Syntax Analysis A hand-written Flex scanner paired with a Bison grammar covering the full Alpha language. Intermediate Code Generation Quads are emitted directly during parsing, with backpatching used to resolve jump targets for control flow and short-circuit booleans. Symbol Table Custom hash table with scope-chained entries, supporting proper shadowing through scope hiding. Target Code Generation Quads are translated into custom bytecode, written out as both a binary file and a readable text dump. Virtual Machine A stack-based interpreter that executes the compiled bytecode directly, including function calls, table operations, and control flow.

## Pipeline

1. Scanning & Parsing

Tokenizes and parses Alpha source, emitting quads on the fly as the grammar reduces.

2. Intermediate Code

Quads are generated with backpatch lists for jumps, scope-aware temp variables, and constant folding on literal arithmetic.

3. Target Code

Quads are translated into bytecode instructions and written to a `.abc` file.

4. Execution

The VM loads the `.abc` file and runs it on a stack-based machine.

## Language Support

Variables, arithmetic, relational and logical operators with short-circuit evaluation. Functions, including named, anonymous, and library functions. Tables with both array-style and key-value initialization. Control flow: if/else, while, for, break, continue. Proper lexical scoping with shadowing.

## Build & Run

```bash
make
./compiler program.asc
./avm program.abc
```

## Notes

Built incrementally across the course phases — scanning and parsing, then intermediate code, target code, and finally the VM. Some AI assistance was used for debugging and code review along the way.
