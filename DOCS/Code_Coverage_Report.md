# Code Coverage Report

## Objective

Document line coverage for the File Compression Tool's two measured C++ source modules after running the registered C++ tests.

## Methodology

The C++ code was built in Debug mode with GCC 16.2.0 using `--coverage -O0 -g` and CMake 4.4.4. The automated tests were run through CTest, then line coverage was measured with gcov 16.2.0. All registered CTest tests passed.

## Results

| Source module | Executable lines | Line coverage |
| --- | ---: | ---: |
| `src/huffman.cpp` | 47 | 95.74% |
| `src/zip_archive.cpp` | 636 | 81.76% |
| **Combined** | **683** | **82.72%** |

## Interpretation

The tests exercised most executable lines in `src/huffman.cpp` and a smaller share in `src/zip_archive.cpp`. The combined figure is weighted by each module's executable line count; it is not the average of the two percentages. Passing tests and line coverage describe different outcomes: a passing test run does not imply that every line was executed.

## Limitations

These measurements cover only `src/huffman.cpp` and `src/zip_archive.cpp`. They do not represent coverage of the entire application, the Flask backend, or the React frontend. Line coverage records whether lines ran; it does not establish that every behavior or branch on an executed line was tested.

## Conclusion

The measured C++ modules achieved **82.72% combined line coverage**, and **100% of registered CTest tests passed**. This is a scoped coverage result, not 100% code coverage of the project.
