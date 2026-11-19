# Provenance of the carved-out code

This project contains the topology-constrained shape optimization (Sec. 6.2.1 of *Medial Skeletal Diagram: A
Generalized Medial Axis Approach for 3D Shape Representation*, Guo, Wang & Matusik, SIGGRAPH Asia 2024) extracted
from the `medial-axis` research code base, commit `752070f38d6f0a7004a52ac1cccef30709e18fbe`
(see `ORIGINAL_COMMIT.txt`).

The guiding rule was: **change as little of the original code as possible, but make it build and run correctly.**
`patches/changes_vs_original.diff` is the complete, machine-generated diff of every carved-out file against the
original repository (regenerate it with the loop at the end of this file).

## Where the functionality lives in the original code base

| original file | role |
|---|---|
| `src/libraries/opt/gradientOptimizer.{h,cpp}` | `optimizeContinuous()`: voxel densities from the primitives, FEM solve, adjoint gradient, gradient descent with back-tracking line search, `msd` / `ma` parameterizations |
| `src/libraries/opt/rfunc.{h,cpp}` | implicit functions of the vertex ("sphere") and edge ("cylinder") primitives (ray casting against the deformed template meshes) and their R-function union |
| `src/libraries/opt/linearElasticity.{h,cpp}`, `opt/materials/*.hpp` | 8-node hexahedral linear-elasticity stiffness matrices (E = 1e6, nu = 0.45) |
| `src/tests/shapeOpt/shapeOpt.cpp` | the paper's experiments: `main` = Fig. 19 (`shapeOpt <caseID> <method>`), `mainz` = Fig. 20 (not reachable from `main`) |
| `src/tests/topologyOptTest/` | an unfinished experiment (builds a tet mesh only; not part of the paper) -- not carved out, but its skeleton graph became `examples/cantilever_truss.json` |

## Files

* **58 files are byte-identical copies** of the original (same relative paths): `src/libraries/{logger,basicUtilities,libiglInterface}.*`,
  `src/libraries/opt/{rfunc,linearElasticity}.*`, `src/libraries/opt/materials/*`, `src/tests/shapeOpt/shapeOpt.cpp`, and the
  subset of `external/vegafem` that the optimizer needs (7 translation units: `basicAlgorithms/stringHelper.cpp`,
  `eigenSupport/EigenSupportImpl.cpp`, `mesh/{createTriMesh,geometryQuery,predicates,triMeshGeo,tribox3}.cpp`, plus their headers).
  The subset was determined from the linker map and the compiler's dependency files, not by hand.
* **4 files are modified** (all changes are listed below).
* **New files**: `src/apps/msdShapeOpt/msdShapeOpt.cpp` (JSON driver), `src/apps/makeTemplates/makeTemplates.cpp` and
  `src/apps/common/primitiveTemplates.h` (template primitive meshes), `CMakeLists.txt`, `CMakeModules/` (from libpgo),
  `web/`, `examples/`, `scripts/`, `validation/`, `docs/`.

## Modifications of original files

1. `external/vegafem/mesh/edgeKey.h` -- `+ #include <cstdint>`.
   GCC 13 no longer includes `<cstdint>` transitively; `edgeKey.h` and `triKey.h` use `uint64_t`. Same fix as in libpgo.
2. `external/vegafem/mesh/predicates.cpp` -- `boost::get<T>(&*ret)` -> `std::get_if<T>(&*ret)` (4 lines).
   CGAL 6 (fetched by libpgo's `cgal.cmake`) returns `std::optional<std::variant<...>>` from `CGAL::intersection`. Same fix as in libpgo.
3. `src/libraries/opt/gradientOptimizer.h` -- adds `OptimizeContinuousIterationInfo` and `OptimizeContinuousOptions` and a
   defaulted last parameter `const OptimizeContinuousOptions &options = {}` to `optimizeContinuous()`. Existing callers
   (e.g. the unchanged `shapeOpt.cpp`) compile and behave as before.
4. `src/libraries/opt/gradientOptimizer.cpp`
   * `double defaultR = 0.04;` -> `options.initialRadius` (default 0.04) and `giter < 200` -> `giter < options.maxIterations`
     (default 200): **the defaults are the previously hard-coded values**;
   * an optional `iterationCallback`, invoked after the initial evaluation and after every accepted step, plus a few
     assignments that record values that are already computed (energy terms, gradient norm, step, line-search count).
     They only read the optimizer state;
   * `Eigen::saveMarket(K11, "K11.mat");` is commented out: it wrote the full stiffness matrix (~100 MB of text for the
     60x40x6 grid) at **every** linear solve, i.e. several times per iteration.

With the default options the optimizer is the original one: the JSON driver reproduces the original driver's
`energy.txt` (every energy evaluation, including line-search trials) textually for Fig. 19 case 0 (see README, "Validation").

## Things that were missing from the original repository

* **`s.obj` and `c1.obj`**, the template meshes that `optimizeContinuous()` loads from the working directory, were never
  committed (not in the tree, not in the git history). They are reconstructed from the constraints the code imposes on
  them -- see `src/apps/makeTemplates/makeTemplates.cpp` for the full argument:
  * vertex primitive: VegaFEM's `createCubeMesh` (8 vertices = the paper's `|r_k| = 8`; its corners are not unit length,
    which is why `optimizeContinuous()` normalizes the template vertices after loading);
  * edge primitive: VegaFEM's `createCylinderMesh(1, 1, 4, 2)` (14 vertices, `ALOG(UCylinder.rows() == 14)`), whose vertex
    numbering (3 rings x 4, then the two cap centers) is exactly the one assumed by the cone subspace `UCylinder` of the
    medial-axis baseline, mapped into `EdgeRFunction`'s frame (axis = x in [0, 1], ring 0 and cap 12 at the `c1` end).
  The reconstruction reproduces the paper's Fig. 19 compliance values (README, "Validation").
* The git submodules `external/libigl` and `external/autodiff` are empty in the original checkout. They are now fetched
  by CMake (libpgo modules; libigl pinned to v2.4.0, the version whose API the original `libiglInterface.cpp` uses), so
  `rfunc.cpp` (includes autodiff) and `libiglInterface.cpp` (uses `igl::readOBJ`, `igl::diag`, ...) compile unchanged.

## Build system

The original `CMakeLists.txt` requires Gurobi, Geogram, Ceres, NLopt, HDF5, METIS, ARPACK, GLFW/OpenGL, ... for the
whole research code base. The new top-level `CMakeLists.txt` builds only the optimizer, with dependency management
borrowed from [libpgo](https://github.com/bohanwang/libpgo) (`CMakeModules/`, MIT license, see `CMakeModules/LICENSE-libpgo`
and `CMakeModules/LIBPGO_COMMIT.txt`): MKL (`tbb_thread`, as in the original) and TBB from a conda environment, Eigen 3.4.0,
fmt, spdlog, nlohmann_json, autodiff, Boost, CGAL and libigl through `FetchContent`. Compile flags and definitions follow
the original project (`-O3 -march=native -frounding-math -fsignaling-nans -UNDEBUG`, `EIGEN_USE_MKL_ALL`,
`EIGEN_DONT_PARALLELIZE`, `CGAL_HAS_THREADS`, `VEGA_EXPORT=`, `USE_TBB`, `SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE`).

## Regenerating the diff

```bash
O=/path/to/medial_axis; cd msd_shape_opt; : > patches/changes_vs_original.diff
for f in $(find src external -type f | sort); do
  [ -f "$O/$f" ] && ! cmp -s "$O/$f" "$f" && diff -u --label "a/$f" --label "b/$f" "$O/$f" "$f" >> patches/changes_vs_original.diff
done
```
