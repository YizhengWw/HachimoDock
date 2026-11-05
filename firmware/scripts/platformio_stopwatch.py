"""Select the S3 main directory before PlatformIO reads its CMake codemodel.

This also makes incremental CMake invalidation observe the actual target
CMakeLists.txt instead of the default P4 main directory.
"""
Import("env")

env.Replace(PROJECT_SRC_DIR=env.subst("$PROJECT_DIR/targets/stopwatch/main"))
