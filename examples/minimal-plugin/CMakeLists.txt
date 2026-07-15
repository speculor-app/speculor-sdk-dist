cmake_minimum_required(VERSION 3.24)
project(minimal_plugin LANGUAGES CXX)

# Resolves from the extracted SDK bundle when CMAKE_PREFIX_PATH points at it.
# Brings in the SpeculorSDK:: targets and the spc_add_plugin() helper.
find_package(SpeculorSDK REQUIRED)

# Builds a correctly configured plugin shared library: no lib prefix, hidden
# visibility, RPATH wired for the bundled dependencies.
spc_add_plugin(my_plugin SOURCES my_plugin.cpp)
