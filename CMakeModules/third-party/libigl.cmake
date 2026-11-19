if(TARGET igl::core)
    return()
endif()

message(STATUS "Loading libigl...")

# [msd_shape_opt] Borrowed from libpgo (CMakeModules/third-party/libigl.cmake) with three changes:
#   1. pinned to the v2.4.0 release instead of `GIT_TAG main`: reproducible, and v2.4.0 is the libigl
#      generation the original medial-axis code was written against (its libiglInterface.cpp includes
#      headers such as igl/diag.h that were removed in later releases);
#   2. header-only igl::core, since the carved-out code only needs igl::readOBJ
#      (through LibIGLInterface::loadObjMesh), so there is no point in compiling all of libigl;
#   3. SYSTEM, so that warnings raised inside libigl's headers do not clutter the build output.
set(LIBIGL_USE_STATIC_LIBRARY OFF CACHE BOOL "Use libigl as static library" FORCE)

include(FetchContent)
FetchContent_Declare(
    libigl
    URL https://github.com/libigl/libigl/archive/refs/tags/v2.4.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SYSTEM
)

FetchContent_MakeAvailable(libigl)

message(STATUS "Done.")
