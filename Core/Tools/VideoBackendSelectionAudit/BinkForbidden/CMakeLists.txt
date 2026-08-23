cmake_minimum_required(VERSION 3.25)
project(BinkForbidden NONE)

# If the FFmpeg graph accidentally includes the Bink FetchContent declaration,
# this target makes that regression visible without downloading anything.
add_library(binkstub INTERFACE)
