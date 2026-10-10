cmake_minimum_required(VERSION 3.16)
project(umc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Java 1.8 REQUIRED COMPONENTS Development)
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

# The bridge class is defined into the target JVM at runtime, so it is compiled
# for Java 8 to stay loadable by old Minecraft versions.
set(BRIDGE_SRC ${CMAKE_SOURCE_DIR}/src/java/umc/Bridge.java)
set(BRIDGE_CLASS ${CMAKE_BINARY_DIR}/java/umc/Bridge.class)
set(BRIDGE_HEADER ${CMAKE_BINARY_DIR}/generated/bridge_class.h)
add_custom_command(
  OUTPUT ${BRIDGE_HEADER}
  COMMAND ${Java_JAVAC_EXECUTABLE} --release 8 -nowarn -d ${CMAKE_BINARY_DIR}/java ${BRIDGE_SRC}
  COMMAND ${CMAKE_COMMAND} -DIN=${BRIDGE_CLASS} -DOUT=${BRIDGE_HEADER} -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${BRIDGE_SRC} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  COMMENT "Compiling umc.Bridge")

add_library(umc_agent SHARED
  src/agent/agent.cpp
  src/agent/core.cpp
  src/agent/commands.cpp
  src/agent/scripts.cpp
  src/agent/game.cpp
  src/agent/classfile.cpp
  src/agent/mappings.cpp
  src/agent/server.cpp
  src/common/platform.cpp
  ${BRIDGE_HEADER})
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
  src/cli/inject.cpp
  src/cli/process.cpp
  src/cli/process_linux.cpp
  src/cli/process_macos.cpp
  src/cli/process_windows.cpp
  src/cli/hotspot_attach.cpp
  src/cli/mappings_fetch.cpp
  src/common/platform.cpp)
target_include_directories(umc PRIVATE src)
target_link_libraries(umc PRIVATE Threads::Threads)
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
