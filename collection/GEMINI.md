# Project Overview

This project appears to be a low-level C codebase focused on a "TGW Full Dispatch Layer". It involves processing "TGWResult" through a pipeline that includes "blueprint to geopkt" conversion and "geomatrix_batch_verdict" for routing decisions. The code emphasizes memory efficiency ("No malloc. No heap."), suggesting an embedded or performance-critical application context.

# Building and Running

The build system is not immediately apparent, as no standard build configuration files (e.g., `Makefile`, `CMakeLists.txt`) were found. The presence of `test_*.c` files indicates a testing framework, but how tests are compiled and run is also unknown.

**TODO:** Identify the build system and document build/run/test commands.

# Development Conventions

*   **Language:** C
*   **Memory Management:** Explicitly avoids dynamic memory allocation (`malloc`, `heap`), indicating a focus on deterministic performance and resource management.
*   **Testing:** Unit/integration tests are present (e.g., `test_minimal.c`), but the testing framework or execution method is not defined in the explored files.
*   **Code Structure:** Header files (`.h`) define interfaces and inline functions, while `.c` files likely contain implementations or test cases.
