# Contributing

## C++ conventions

- Keep first-party code compatible with the standard selected in `CMakeLists.txt`; prefer clear C++20/23
  standard-library facilities over hand-written equivalents.
- Use `std::format` for composed human-readable strings. Stream output remains appropriate for incremental CSV and
  report serialization when it is clearer or avoids unnecessary temporary strings.
- Use raw string literals when they make multiline text, regular expressions, or escaped content easier to read.
- Use doctest for C++ tests.
- Use Doxygen comments for every class, structure, enumeration, public method, public free function, and utility
  function. Document public API data members and enumerators as well.

## Formatting

clang-format is the primary formatter. Uncrustify supplements it with the statement-level blank-line rules that
clang-format cannot express. In particular, complete condition and loop blocks are visually separated from adjacent
statements, and a `return` is separated from preceding statements in the same block.

Both tools must be available at the versions used by CI: clang-format 20.1.8 and Uncrustify 0.83.0. After configuring
the project, apply or check the complete formatting policy with:

```sh
cmake --build build --target format
cmake --build build --target check-format
```

The same commands work with multi-configuration generators; no build configuration is required for these utility
targets. In CLion, they appear in the list of CMake targets and can be run directly.
