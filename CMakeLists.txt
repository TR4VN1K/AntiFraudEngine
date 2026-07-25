cmake_minimum_required(VERSION 3.15)
project(antifraud_core LANGUAGES CXX)

# --- Language standard -------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Position-independent code is required for building a shared object
# that Python can dlopen.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- Dependencies --------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
find_package(pybind11 CONFIG REQUIRED)
find_package(Threads REQUIRED)

# --- Core library (pure C++, no Python dependency) ------------------------
add_library(antifraud_engine STATIC
    src/AntiFraudEngine.cpp
    src/Rules.cpp
)
target_include_directories(antifraud_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(antifraud_engine PUBLIC Threads::Threads)
target_compile_options(antifraud_engine PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -O3>
)

# --- Python extension module ---------------------------------------------
pybind11_add_module(antifraud_core src/bindings.cpp)
target_link_libraries(antifraud_core PRIVATE antifraud_engine)
target_include_directories(antifraud_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

install(TARGETS antifraud_core LIBRARY DESTINATION .)
