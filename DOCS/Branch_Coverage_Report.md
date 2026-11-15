# Branch Coverage Report

## Objective

Document branch coverage for the File Compression Tool's two measured C++ source modules after running the registered C++ tests.

## Methodology

The C++ code was built in Debug mode with GCC 16.2.0 using `--coverage -O0 -g` and CMake 4.4.4. The automated tests were run through CTest, then branch coverage was measured with gcov 16.2.0. All registered CTest tests passed.

gcov distinguishes a branch being **executed** (its decision point was reached) from a branch being **taken at least once** (control followed that particular branch). The percentages below report **branches taken at least once**. They are not the percentage of branches merely executed.

## Results

| Source module | Branches | Branches taken at least once |
| --- | ---: | ---: |
| `src/huffman.cpp` | 44 | 68.18% |
| `src/zip_archive.cpp` | 1,182 | 43.32% |
| **Combined** | **1,226** | **Approximately 44.21%** |

## Interpretation

The tests took a larger share of measured branches in `src/huffman.cpp` than in `src/zip_archive.cpp`. The combined percentage is weighted by the number of branches in each module; it is not the average of the two module percentages. A branch can be executed without being taken, so the reported metric specifically describes paths followed at least once.

## Limitations

These measurements cover only `src/huffman.cpp` and `src/zip_archive.cpp`. They do not represent branch coverage of the entire application, the Flask backend, or the React frontend. Taking a branch once does not establish that all inputs or behaviors along that path were tested. No separate percentage for branches merely executed is reported here.

## Conclusion

The measured C++ modules achieved **approximately 44.21% combined branch coverage, measured as branches taken at least once**, and **100% of registered CTest tests passed**. This is a scoped branch coverage result, not 100% code coverage of the project.
