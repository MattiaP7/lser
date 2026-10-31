cmake_minimum_required(VERSION 3.22)
project(LSer LANGUAGES CXX VERSION 1.0.0)

# ==============================================================================
# Opzioni
# ==============================================================================
option(LSER_USE_TBB          "Usa TBB per ordinare in parallelo le cartelle (se trovato)" ON)
option(LSER_BUILD_TESTS      "Compila i test (ctest)"                                     ON)
option(LSER_BUILD_BENCHMARKS "Compila i benchmark (richiede Google Benchmark)"         ON)
option(LSER_NATIVE           "Usa ottimizzazioni CPU correnti (binario non portabile)"    OFF)
option(LSER_SANITIZE         "Abilita AddressSanitizer (+ UBSan su GCC/Clang)"            OFF)
option(LSER_TSAN             "Abilita ThreadSanitizer (non combinabile con LSER_SANITIZE)" OFF)

if(LSER_SANITIZE AND LSER_TSAN)
  message(FATAL_ERROR "LSER_SANITIZE e LSER_TSAN non si possono usare insieme")
endif()

# Build type di default: Release (ottimizzato)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Tipo di build" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ==============================================================================
# Libreria header-only con le impostazioni comuni (Cross-Compiler)
# ==============================================================================
find_package(Threads REQUIRED)

add_library(lser_core INTERFACE)
target_include_directories(lser_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(lser_core INTERFACE cxx_std_23)
target_link_libraries(lser_core INTERFACE Threads::Threads)

# --- Gestione Specifica per Compilatore ---
if(MSVC)
  # Flag per Visual Studio (MSVC)
  target_compile_options(lser_core INTERFACE /W4 /Zc:__cplusplus /permissive-)
  
  if(LSER_NATIVE)
    target_compile_options(lser_core INTERFACE /arch:AVX2)
  endif()
  
  if(LSER_SANITIZE)
    target_compile_options(lser_core INTERFACE /fsanitize=address /Zi)
  endif()
else()
  # Flag per GCC / Clang / MinGW
  target_compile_options(lser_core INTERFACE -Wall -Wextra -Wpedantic)
  
  # Ottimizzazioni per Release pulite con generator expressions
  target_compile_options(lser_core INTERFACE
    $<$<CONFIG:Release>:-O3>
  )
  
  if(LSER_NATIVE)
    target_compile_options(lser_core INTERFACE
      $<$<CONFIG:Release>:-march=native>
    )
  endif()
  
  if(LSER_SANITIZE)
    target_compile_options(lser_core INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(lser_core INTERFACE -fsanitize=address,undefined)
  endif()
  
  if(LSER_TSAN)
    target_compile_options(lser_core INTERFACE -fsanitize=thread -fno-omit-frame-pointer -g)
    target_link_options(lser_core INTERFACE -fsanitize=thread)
  endif()
endif()

# Fix essenziale per GCC / MinGW: std::print richiede stdc++exp per risolvere i simboli del terminale
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_link_libraries(lser_core INTERFACE stdc++exp)
endif()

# Fix per il linking statico su MinGW-w64 (Windows)
if(MINGW)
  target_link_options(lser_core INTERFACE -static-libstdc++ -static-libgcc -static)
endif()

# ==============================================================================
# Integrazione TBB (Opzionale)
# ==============================================================================
if(LSER_USE_TBB)
  find_package(TBB QUIET)
  if(TBB_FOUND)
    target_compile_definitions(lser_core INTERFACE LSER_HAS_TBB)
    target_link_libraries(lser_core INTERFACE TBB::tbb)
    message(STATUS "LSer: TBB trovato, ordinamento parallelo abilitato")
  else()
    message(STATUS "LSer: TBB non trovato, ordinamento sequenziale")
  endif()
endif()

# ==============================================================================
# Integrazione TBB (Opzionale)
# ==============================================================================
if(LSER_USE_TBB)
  find_package(TBB QUIET)
  if(TBB_FOUND)
    target_compile_definitions(lser_core INTERFACE LSER_HAS_TBB)
    target_link_libraries(lser_core INTERFACE TBB::tbb)
    message(STATUS "LSer: TBB trovato, ordinamento parallelo abilitato")
  else()
    message(STATUS "LSer: TBB non trovato, ordinamento sequenziale")
  endif()
endif()

# ==============================================================================
# Target 1: LSer CLI
# ==============================================================================
add_executable(lser main.cpp lser.hpp)
target_link_libraries(lser PRIVATE lser_core)

# ==============================================================================
# Target 2: Test di correttezza
# ==============================================================================
if(LSER_BUILD_TESTS)
  enable_testing()
  add_executable(lser_tests tests/test_lser.cpp lser.hpp)
  target_link_libraries(lser_tests PRIVATE lser_core)
  add_test(NAME lser_tests COMMAND lser_tests)
  set_tests_properties(lser_tests PROPERTIES TIMEOUT 120)
endif()

# ==============================================================================
# Target 3: Benchmark
# ==============================================================================
if(LSER_BUILD_BENCHMARKS)
  find_package(benchmark QUIET)
  if(benchmark_FOUND)
    add_executable(bench_lser bench.cpp lser.hpp)
    target_link_libraries(bench_lser PRIVATE lser_core benchmark::benchmark)
  else()
    message(WARNING "Google Benchmark non trovato: bench_lser non verra' compilato "
                    "(pacman -S benchmark, oppure -DLSER_BUILD_BENCHMARKS=OFF)")
  endif()
endif()

# ==============================================================================
# Installazione e Packaging (CPack)
# ==============================================================================
include(GNUInstallDirs)

install(TARGETS lser RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES lser.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lser)

set(CPACK_PACKAGE_NAME "lser")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Clone C++23 multithreaded ad alte prestazioni di ls/tree")
set(CPACK_PACKAGE_CONTACT "Mattia Pirazzi")
set(CPACK_GENERATOR "DEB;TGZ")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")

include(CPack)