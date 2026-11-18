# Oblivious-priority-queue

This repository contains two versions of the code:

- `implementation_old/`: the original implementation used for the first draft of the paper, kept unchanged for reference. It contains our buffered queue (`full_solution`), the Path Oblivious Heap baseline (`pathObliviousHeap`), simulation variants without a network (`*_simulation`) and the workload generator (`query_generator`). To build its tests, clone https://github.com/BLAKE3-team/BLAKE3 into the test directories.
- `implementation_new/`: the revised implementation, in which all further development happens. See `implementation_new/README.md` for building, running and choosing parameters.
