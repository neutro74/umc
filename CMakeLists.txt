cmake_minimum_required(VERSION 3.16)
project(umc VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(JNI REQUIRED COMPONENTS JVM)
find_package(Threads REQUIRED)

# QuickJS-ng runs script plugins. Built without its std/os modules.
include(FetchContent)
FetchContent_Declare(quickjs
  GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
  GIT_TAG v0.17.0
  GIT_SHALLOW TRUE)
set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(quickjs)

# miniz reads the MCP zip archives used to build mappings for pre-1.14 versions.
FetchContent_Declare(miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG 3.0.2
  GIT_SHALLOW TRUE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)  # miniz declares an older minimum than CMake 4 accepts
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(miniz)

# linenoise gives the interactive shell history, completion and live output on Linux and macOS.
# The Windows console already provides line editing, so it isn't built there.
if(NOT WIN32)
  FetchContent_Declare(linenoise
    GIT_REPOSITORY https://github.com/antirez/linenoise.git
    GIT_TAG a473823d74b93eab2ba83480df16ed37617493f2)
  FetchContent_MakeAvailable(linenoise)
  add_library(linenoise STATIC ${linenoise_SOURCE_DIR}/linenoise.c)
  target_include_directories(linenoise PUBLIC ${linenoise_SOURCE_DIR})
endif()

# The injected bootstrap bridge is embedded in src/agent/bridge_class.h.
set(ACCESSORS_SRC ${CMAKE_SOURCE_DIR}/src/agent/accessors.txt)
set(ACCESSORS_HEADER ${CMAKE_BINARY_DIR}/generated/accessors_table.h)
add_custom_command(
  OUTPUT ${ACCESSORS_HEADER}
  COMMAND ${CMAKE_COMMAND} -DIN=${ACCESSORS_SRC} -DOUT=${ACCESSORS_HEADER} -DNAME=kAccessorsTable -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${ACCESSORS_SRC} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  COMMENT "Embedding accessors.txt")

set(UMC_CORE_SOURCES
  src/agent/core.cpp
  src/agent/commands.cpp
  src/agent/scripts.cpp
  src/agent/game.cpp
  src/agent/java.cpp
  src/agent/classfile.cpp
  src/agent/mappings.cpp
  src/common/platform.cpp
  ${ACCESSORS_HEADER})
add_library(umc_agent SHARED src/agent/agent.cpp src/agent/server.cpp ${UMC_CORE_SOURCES})
set_target_properties(umc_agent PROPERTIES OUTPUT_NAME umc)
target_include_directories(umc_agent PRIVATE include src ${CMAKE_BINARY_DIR}/generated ${JNI_INCLUDE_DIRS})
target_link_libraries(umc_agent PRIVATE qjs Threads::Threads ${CMAKE_DL_LIBS})
if(MSVC)
  target_compile_options(umc_agent PRIVATE /W4)
else()
  target_compile_options(umc_agent PRIVATE -Wall -Wextra -fvisibility=hidden)
endif()

add_executable(umc
  src/cli/main.cpp
  src/cli/dispatch.cpp
  src/cli/daemon.cpp
  src/cli/repl.cpp
  src/cli/inject.cpp
  src/cli/process.cpp
  src/cli/process_linux.cpp
  src/cli/process_macos.cpp
  src/cli/process_windows.cpp
  src/cli/hotspot_attach.cpp
  src/cli/mappings_fetch.cpp
  src/cli/scripts.cpp
  src/cli/legacy_mappings.cpp
  src/common/platform.cpp)
target_include_directories(umc PRIVATE src)
target_link_libraries(umc PRIVATE miniz Threads::Threads)
if(NOT WIN32)
  target_link_libraries(umc PRIVATE linenoise)
endif()
if(MSVC)
  target_compile_options(umc PRIVATE /W4)
else()
  target_compile_options(umc PRIVATE -Wall -Wextra)
endif()

if(WIN32)
  target_link_libraries(umc_agent PRIVATE ws2_32)
  target_link_libraries(umc PRIVATE ws2_32)
endif()

add_library(tick_counter MODULE examples/tick_counter/tick_counter.cpp)
set_target_properties(tick_counter PROPERTIES PREFIX "")
target_include_directories(tick_counter PRIVATE include ${JNI_INCLUDE_DIRS})

include(GNUInstallDirs)
install(TARGETS umc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS umc_agent LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/umc)
install(DIRECTORY include/umc DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# In-process JVM tests also run where the sandbox prohibits attach/control sockets.
add_executable(umc_script_test EXCLUDE_FROM_ALL test/script_api.cpp ${UMC_CORE_SOURCES})
target_include_directories(umc_script_test PRIVATE include src ${CMAKE_BINARY_DIR}/generated ${JNI_INCLUDE_DIRS})
target_link_libraries(umc_script_test PRIVATE qjs ${JNI_LIBRARIES} Threads::Threads ${CMAKE_DL_LIBS})
