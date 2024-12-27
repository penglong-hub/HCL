#!/bin/bash -x

SRC_PATH=$(pwd)/../..
echo "SRC_PATH is: $SRC_PATH"
rm -rf CMakeCache.txt CMakeFiles/ Makefile cmake_install.cmake
HCL_SRC_PKG_DIR=${SRC_PATH} HCL_LIB_DIR=/usr/lib/habanalabs/ cmake .

make -j 100 VERBOSE=1
