# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.24)
project(yang_package_consumer LANGUAGES CXX)

find_package(yang CONFIG REQUIRED)
add_executable(yang_package_consumer main.cc)
target_compile_features(yang_package_consumer PRIVATE cxx_std_20)
target_link_libraries(yang_package_consumer PRIVATE yang::yang)
