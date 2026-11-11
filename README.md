# qqmem

A small Linux command-line tool for explicit register access through `/dev/mem`.
No board database, website, daemon, or kernel module.

## Status

The first milestone implements explicit 8/16/32-bit reads and writes. Local tests
use regular files, not hardware. Hardware validation is still pending.

## Build and test

Build host: Linux, a C11 compiler, CMake 3.16+, and Python 3.8+ for tests.
There are no third-party library dependencies.

```sh
cmake -S . -B ../work/build -DCMAKE_BUILD_TYPE=Release
cmake --build ../work/build
(cd ../work/build && ctest --output-on-failure)
```

Tests create temporary memory files under the build directory and remove them
afterwards. They cover access widths, page offsets, unchanged neighboring bytes,
invalid inputs, and access errors. They never access `/dev/mem` or require root.

For a build without Python or tests, add `-DBUILD_TESTING=OFF`.

## Usage

```sh
qqmem read ADDRESS --width 32
qqmem write ADDRESS VALUE --width 32
```

Operands precede options. `--width` is required: `8`, `16`, or `32`.
Addresses and values accept decimal, `0x`-prefixed hex, and `0`-prefixed octal.
Negative numbers, overflow, and unaligned addresses are rejected before opening
the device. Addresses must fit the supported signed 64-bit offset range.

- Reads print one zero-padded hex value, such as `0x00000005`.
- Writes are silent on success, without implicit reads or readback verification.
- Accesses use fixed-width volatile types and native CPU byte order.
- The default device is `/dev/mem`. `--device PATH` selects another device or an
  existing regular file; its addresses are offsets into that file or device.
- Exit codes: `0` success, `1` access/output failure, `2` invalid arguments.

To try it without hardware, create a small file in the playground:

```sh
truncate -s 4096 ../work/memory.bin
../work/build/qqmem write 0x10 0x12345678 --width 32 --device ../work/memory.bin
../work/build/qqmem read 0x10 --width 32 --device ../work/memory.bin
# 0x12345678
```

## Layout

```text
repo/
  CMakeLists.txt
  LICENSE
  README.md
  src/qqmem.c
  tests/test_cli.py
work/                 builds, experiments, and temporary fixtures
archive/              previous work, untouched
```

## Installation and future packaging

```sh
cmake --install ../work/build --prefix ../work/install
```

CMake provides a standard install target, honors `DESTDIR`, and supports
cross-compiling with `-DCMAKE_TOOLCHAIN_FILE=...`. Build tools and Python are not
needed on the target; the executable must match its architecture and runtime.

A later OpenEmbedded/Yocto recipe can use `inherit cmake` and disable host tests
with `-DBUILD_TESTING=OFF`. A recipe, CI, and release automation are not included
yet.

## License

Licensed under the [Apache License, Version 2.0](LICENSE)
(`SPDX-License-Identifier: Apache-2.0`). Commercial use and modification are
permitted under its terms, including its notice requirements and explicit
contributor patent license.

The install target includes the license under `share/licenses/qqmem/`.

## Boundaries

Register access can change hardware state or cause a hardware fault. Even reads
can have side effects. This tool cannot bypass kernel access restrictions, manage
clocks, or coordinate with drivers. Access can terminate with a hardware fault
even after a mapping succeeds. A successful write means the store was issued,
not that the peripheral accepted it or completed an operation.

Use only addresses, widths, and access types that the hardware documentation
allows. No automatic snapshot or restoration is planned. Regular-file bounds
checks do not protect against another process truncating that file concurrently.

## Next steps

1. Validate the basic commands on hardware.
2. Add masked updates for ordinary read/write registers (not atomic against drivers).
3. Add assertions and polling with a timeout.
4. Consider text recipes after the individual commands work well.
5. Add CI, release builds, and an OpenEmbedded recipe when the interface settles.

Each step should be usable and tested before the next begins.
