cmake_minimum_required(VERSION 3.16)
project(orion VERSION 0.1 LANGUAGES CXX)

# Orion: agente autónomo en C++ con un único modelo local (Ollama). Sin dependencias externas que descargar:
# nlohmann/json va incluido en third_party/.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_library(orion_core STATIC
  src/util.cpp src/http.cpp src/ollama.cpp src/memoria.cpp src/sistema.cpp src/mente.cpp src/nucleo.cpp)
target_include_directories(orion_core PUBLIC include third_party)
target_link_libraries(orion_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(orion_core PUBLIC ws2_32 advapi32)
  target_compile_definitions(orion_core PUBLIC _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
  target_compile_options(orion_core PUBLIC /utf-8 /W4 /permissive- /EHsc)
  target_compile_definitions(orion_core PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(orion_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(orion src/main_orion.cpp)
target_link_libraries(orion PRIVATE orion_core)
add_executable(guardian src/main_guardian.cpp)
target_link_libraries(guardian PRIVATE orion_core)

option(ORION_TESTS "Compilar las pruebas" ON)
if(ORION_TESTS)
  enable_testing()
  add_executable(orion_tests tests/test_orion.cpp)
  target_link_libraries(orion_tests PRIVATE orion_core)
  target_compile_definitions(orion_tests PRIVATE ORION_EXE="$<TARGET_FILE:orion>" GUARDIAN_EXE="$<TARGET_FILE:guardian>")
  add_dependencies(orion_tests orion guardian)
  add_test(NAME orion_tests COMMAND orion_tests)
endif()
