# Boilerplate order is fixed; cmake will not work if these are reordered.
# Structure verified against esp-idf v6.0.3 examples/get-started/hello_world.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Include only main and what it actually depends on. Keeps the safe probe
# firmware small enough to flash onto a board with an unknown partition layout.
idf_build_set_property(MINIMAL_BUILD ON)

project(esp32_workbench_probe)
