cmake_minimum_required(VERSION 3.28.0 FATAL_ERROR)

# =====================================================================================
# msd_shape_opt
#
# Topology-constrained shape optimization (compliance minimization) with the Medial
# Skeletal Diagram, carved out of the `medial-axis` code base (Sec. 6.2.1 of
# "Medial Skeletal Diagram: A Generalized Medial Axis Approach for 3D Shape
# Representation", Guo, Wang, Matusik, SIGGRAPH Asia 2024, arXiv:2310.09395).
#
# Dependency management is borrowed from libpgo (https://github.com/bohanwang/libpgo):
#   * MKL + TBB come from a conda environment (`conda install tbb tbb-devel mkl mkl-devel`);
#   * everything else (Eigen, fmt, spdlog, nlohmann_json, autodiff, Boost, CGAL, libigl)
#     is fetched by the CMake modules in CMakeModules/third-party (copied from libpgo).
# See README.md for the full build instructions.
# =====================================================================================

project(msd_shape_opt LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The original optimizer factorizes the stiffness matrix with MKL Pardiso
# (VegaFEM::EigenSupport::SymSolver = Eigen::PardisoLDLT), so MKL is required.
option(PGO_USE_MKL "Use MKL (required by the Pardiso-based solver)" ON)
option(PGO_CHECK_CONDA "Pick TBB from the active conda environment (same TBB that MKL's tbb_thread layer uses)" ON)
option(PGO_MKL_LINK_DYNAMIC "MKL Link" ON)

if(NOT PGO_USE_MKL)
  message(FATAL_ERROR "msd_shape_opt requires MKL (Pardiso). Re-run with -DPGO_USE_MKL=ON inside the conda env.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMakeModules")
include(CompilationUtils)

if(CMAKE_BUILD_TYPE STREQUAL "")
  message(STATUS "CMake build type is empty and will be set to Release")
  set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(WARNING "Only GCC 11-13 on Linux has been tested (same as libpgo).")
endif()

find_package(OpenMP)

if(OPENMP_FOUND)
  set(OPENMP_FLAG "${OpenMP_CXX_FLAGS}")
endif()

# ------------------------------------------------------------------------------------
# compilation flags (libpgo's `compilation_flag` + the flags of the original project)
# NOTE: the target must be called `compilation_flag`: libpgo's eigen.cmake reads it.
# ------------------------------------------------------------------------------------
add_library(compilation_flag INTERFACE)

add_flag_poxis(compilation_flag INTERFACE -frounding-math)   # required by CGAL's filtered predicates
add_flag_poxis(compilation_flag INTERFACE -fvisibility=hidden)
add_flag_poxis(compilation_flag INTERFACE -march=native)
add_flag_poxis(compilation_flag INTERFACE -mtune=native)
add_link_flag_poxis(compilation_flag -march=native)
add_link_flag_poxis(compilation_flag -mtune=native)

# static gcc runtime (as in libpgo): avoids clashes with the newer libstdc++ shipped in conda envs
add_flag(compilation_flag GNU All INTERFACE -static-libstdc++)
add_flag(compilation_flag GNU All INTERFACE -static-libgcc)
add_link_flag(compilation_flag GNU All -static-libstdc++)
add_link_flag(compilation_flag GNU All -static-libgcc)

if(OPENMP_FOUND)
  add_flag(compilation_flag GNU All INTERFACE ${OPENMP_FLAG})
  add_link_flag(compilation_flag GNU All ${OPENMP_FLAG})
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_flag_poxis(compilation_flag INTERFACE -O0)
  add_flag_poxis(compilation_flag INTERFACE -ggdb3)
else()
  add_flag_poxis(compilation_flag INTERFACE -O3)
  # same as the original medial-axis project: keep debug info and keep assert() enabled
  add_flag_poxis(compilation_flag INTERFACE -ggdb)
  add_flag_poxis(compilation_flag INTERFACE -UNDEBUG)
endif()

# from the original medial-axis project
add_flag_poxis(compilation_flag INTERFACE -fsignaling-nans)

if(CMAKE_GENERATOR STREQUAL "Ninja")
  add_flag_poxis(compilation_flag INTERFACE -fdiagnostics-color=always)
endif()

# definitions used by the original medial-axis / VegaFEM code
add_library(msd_definitions INTERFACE)
target_compile_definitions(msd_definitions INTERFACE
  VEGA_EXPORT=
  USE_TBB
  CGAL_HAS_THREADS
  SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

# warnings are only enabled for the code that is new in this project (see below);
# the carved-out third-party / research code is compiled exactly as it is.
add_library(msd_warnings INTERFACE)
target_compile_options(msd_warnings INTERFACE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)

# ------------------------------------------------------------------------------------
# third-party libraries (CMake modules borrowed from libpgo)
# ------------------------------------------------------------------------------------
find_package(Threads REQUIRED)

include(CMakeModules/third-party/tbb.cmake)
include(CMakeModules/third-party/mkl.cmake)
include(CMakeModules/third-party/eigen.cmake)
include(CMakeModules/third-party/fmt.cmake)
include(CMakeModules/third-party/nlohmann_json.cmake)
include(CMakeModules/third-party/spdlog.cmake)
include(CMakeModules/third-party/autodiff.cmake)
include(CMakeModules/third-party/boost.cmake)
include(CMakeModules/third-party/cgal.cmake)
include(CMakeModules/third-party/libigl.cmake)

foreach(tgt TBB::tbb MKL::MKL Eigen3::Eigen fmt::fmt-header-only spdlog::spdlog_header_only
    nlohmann_json::nlohmann_json autodiff::autodiff CGAL::CGAL igl::core)
  if(NOT TARGET ${tgt})
    message(FATAL_ERROR "Missing dependency target: ${tgt}")
  endif()
endforeach()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ------------------------------------------------------------------------------------
# VegaFEM subset (external/vegafem): only the translation units that the optimizer needs
# ------------------------------------------------------------------------------------
set(VEGA_ROOT ${CMAKE_SOURCE_DIR}/external/vegafem)

add_library(vegafem_subset STATIC
  ${VEGA_ROOT}/basicAlgorithms/stringHelper.cpp
  ${VEGA_ROOT}/eigenSupport/EigenSupportImpl.cpp
  ${VEGA_ROOT}/mesh/createTriMesh.cpp
  ${VEGA_ROOT}/mesh/geometryQuery.cpp
  ${VEGA_ROOT}/mesh/predicates.cpp
  ${VEGA_ROOT}/mesh/triMeshGeo.cpp
  ${VEGA_ROOT}/mesh/tribox3.cpp)

target_include_directories(vegafem_subset PUBLIC
  ${VEGA_ROOT}/macros
  ${VEGA_ROOT}/basicAlgorithms
  ${VEGA_ROOT}/minivector
  ${VEGA_ROOT}/mesh
  ${VEGA_ROOT}/eigenSupport
  ${VEGA_ROOT}/eigenVegaTypeConverter
  ${VEGA_ROOT}/nonlinearOptimization)

target_link_libraries(vegafem_subset PUBLIC msd_definitions Eigen3::Eigen MKL::MKL TBB::tbb CGAL::CGAL)
target_link_libraries(vegafem_subset PRIVATE compilation_flag)

# ------------------------------------------------------------------------------------
# carved-out medial-axis code (src/libraries): shape optimization
# ------------------------------------------------------------------------------------
set(MA_ROOT ${CMAKE_SOURCE_DIR}/src/libraries)

add_library(medialAxisShapeOpt STATIC
  ${MA_ROOT}/logger.cpp
  ${MA_ROOT}/basicUtilities.cpp
  ${MA_ROOT}/libiglInterface.cpp
  ${MA_ROOT}/opt/rfunc.cpp
  ${MA_ROOT}/opt/gradientOptimizer.cpp
  ${MA_ROOT}/opt/linearElasticity.cpp)

target_include_directories(medialAxisShapeOpt PUBLIC ${MA_ROOT})
target_link_libraries(medialAxisShapeOpt PUBLIC
  vegafem_subset igl::core autodiff::autodiff
  spdlog::spdlog_header_only fmt::fmt-header-only nlohmann_json::nlohmann_json)
target_link_libraries(medialAxisShapeOpt PRIVATE compilation_flag)

# ------------------------------------------------------------------------------------
# executables
# ------------------------------------------------------------------------------------
# 1) the original paper driver (src/tests/shapeOpt/shapeOpt.cpp, unchanged):
#    `shapeOpt <caseID> <method>` reproduces Fig. 19 (needs s.obj / c1.obj in the cwd)
add_executable(shapeOpt ${CMAKE_SOURCE_DIR}/src/tests/shapeOpt/shapeOpt.cpp)
target_link_libraries(shapeOpt PRIVATE medialAxisShapeOpt compilation_flag)

# 2) the JSON driver: any user-specified 3D skeleton graph + boundary conditions
add_executable(msdShapeOpt ${CMAKE_SOURCE_DIR}/src/apps/msdShapeOpt/msdShapeOpt.cpp)
target_link_libraries(msdShapeOpt PRIVATE medialAxisShapeOpt compilation_flag msd_warnings)

# 3) generator for the template primitive meshes (data/templates/s.obj, c1.obj)
add_executable(makeTemplates ${CMAKE_SOURCE_DIR}/src/apps/makeTemplates/makeTemplates.cpp)
target_link_libraries(makeTemplates PRIVATE medialAxisShapeOpt compilation_flag msd_warnings)
