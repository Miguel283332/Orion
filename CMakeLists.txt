cmake_minimum_required(VERSION 3.16)
project(Orion VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Tipo de build" FORCE)
endif()

option(ORION_OPENCL "Backend de GPU (OpenCL: dedicada, integrada o cualquier dispositivo)" ON)
option(ORION_OPENMP "Paralelismo y SIMD en CPU con OpenMP" ON)
option(ORION_NATIVE "Optimizar para la CPU de esta máquina (-march=native); desactívalo para binarios portables" ON)
option(ORION_BUILD_TESTS "Compilar los tests" ON)
option(ORION_WARNINGS_AS_ERRORS "Tratar warnings como errores" OFF)

# ------------------------------------------------------------------ dependencias
set(ORION_LIBS "")
if(ORION_OPENMP)
  find_package(OpenMP COMPONENTS CXX)
  if(OpenMP_CXX_FOUND)
    list(APPEND ORION_LIBS OpenMP::OpenMP_CXX)
  else()
    message(STATUS "OpenMP no encontrado: la CPU usará un solo hilo")
  endif()
endif()
if(NOT OpenMP_CXX_FOUND)
  set(ORION_NO_OMP_FLAGS -Wno-unknown-pragmas)
endif()

set(ORION_HAS_OPENCL OFF)
if(ORION_OPENCL)
  find_package(OpenCL QUIET)
  if(OpenCL_FOUND)
    set(ORION_HAS_OPENCL ON)
    list(APPEND ORION_LIBS OpenCL::OpenCL)
  else()
    message(STATUS "OpenCL no encontrado: solo backend de CPU (instala ocl-icd-opencl-dev y el driver de tu GPU)")
  endif()
endif()
find_package(Threads REQUIRED)
list(APPEND ORION_LIBS Threads::Threads)

# ------------------------------------------------------------------ biblioteca
add_library(orion_core STATIC
  src/backend.cpp
  src/backend_cpu.cpp
  src/backend_opencl.cpp
  src/model.cpp
  src/dataset.cpp
  src/evolve.cpp
  src/chat.cpp)
target_include_directories(orion_core PUBLIC include)
target_link_libraries(orion_core PUBLIC ${ORION_LIBS})
target_compile_definitions(orion_core PUBLIC
  $<$<BOOL:${ORION_HAS_OPENCL}>:ORION_HAS_OPENCL=1>
  $<$<BOOL:${ORION_HAS_OPENCL}>:CL_TARGET_OPENCL_VERSION=120>)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(orion_core PUBLIC -Wall -Wextra -Wno-unused-parameter -fno-math-errno ${ORION_NO_OMP_FLAGS})
  if(ORION_NATIVE AND NOT CMAKE_CROSSCOMPILING)
    target_compile_options(orion_core PUBLIC -march=native)
  endif()
  if(ORION_WARNINGS_AS_ERRORS)
    target_compile_options(orion_core PUBLIC -Werror)
  endif()
  # Evita avisos de atributos ignorados en OpenCL 1.2 desde cabeceras recientes
  if(ORION_HAS_OPENCL)
    target_compile_options(orion_core PUBLIC -Wno-deprecated-declarations -Wno-ignored-attributes)
  endif()
elseif(MSVC)
  target_compile_options(orion_core PUBLIC /W3 /O2 /fp:fast)
endif()

# ------------------------------------------------------------------ ejecutables
add_executable(orion src/main.cpp)
target_link_libraries(orion PRIVATE orion_core)
target_compile_definitions(orion PRIVATE
  ORION_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  ORION_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

if(ORION_BUILD_TESTS)
  enable_testing()
  add_executable(orion_tests tests/test_main.cpp)
  target_link_libraries(orion_tests PRIVATE orion_core)
  add_test(NAME orion_tests COMMAND orion_tests)
endif()

install(TARGETS orion RUNTIME DESTINATION bin)
install(DIRECTORY data/ DESTINATION share/orion/data)

message(STATUS "Orion ${PROJECT_VERSION}: OpenCL=${ORION_HAS_OPENCL} OpenMP=${OpenMP_CXX_FOUND} native=${ORION_NATIVE} build=${CMAKE_BUILD_TYPE}")
