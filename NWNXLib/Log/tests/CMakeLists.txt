# Logger tests. They build the logger with stubs for the engine symbols it references, so they run
# without nwserver. Configure with -DNWNX_BUILD_TESTS=ON, then run ctest in the build directory.

remove_definitions(-DTRACY_ENABLE)

# Test binaries stay in the build tree, out of Binaries/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

set(NWNXLIB_DIR ${CMAKE_SOURCE_DIR}/NWNXLib)

# Stands in for NWNX_Core.
add_library(LogTestCore SHARED
    ${NWNXLIB_DIR}/Log.cpp
    ${NWNXLIB_DIR}/MessageBus.cpp
    ${NWNXLIB_DIR}/Assert.cpp
    EngineStubs.cpp)
add_sanitizers(LogTestCore)
target_include_directories(LogTestCore PUBLIC ${NWNXLIB_DIR} ${NWNXLIB_DIR}/API)
target_compile_definitions(LogTestCore PRIVATE "-DPLUGIN_NAME=\"NWNXLib\"")

# ENABLE_EXPORTS lets the test's time() override the one libc gives the logger.
add_executable(LogGolden LogGolden.cpp)
add_sanitizers(LogGolden)
set_target_properties(LogGolden PROPERTIES ENABLE_EXPORTS ON)
target_link_libraries(LogGolden LogTestCore)
target_compile_definitions(LogGolden PRIVATE "-DPLUGIN_NAME=\"NWNX_LogGolden\"")
add_test(NAME LogGolden COMMAND LogGolden ${CMAKE_CURRENT_SOURCE_DIR}/LogGolden.txt)
