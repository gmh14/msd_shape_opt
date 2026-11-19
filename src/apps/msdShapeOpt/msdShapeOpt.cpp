// [msd_shape_opt] msdShapeOpt: topology-constrained shape optimization (compliance minimization) of an
// arbitrary, user-specified 3D skeletal graph, following Sec. 6.2.1 of "Medial Skeletal Diagram: A Generalized
// Medial Axis Approach for 3D Shape Representation" (arXiv:2310.09395).
//
// This file is new; it replaces the hard-coded experiments of the original driver
// (src/tests/shapeOpt/shapeOpt.cpp, which is kept unchanged) by a JSON description of the problem.
// The optimization itself is the original MedialAxisRepresentation::optimizeContinuous().
//
//   usage: msdShapeOpt <config.json> [-o <output_dir>] [--templates <dir>] [--threads <n>] [--validate-only]
//
// See README.md for the JSON format and for the content of the output directory.

#include "opt/gradientOptimizer.h"
#include "logger.h"
#include "libiglInterface.h"

#include "EigenSupport.h"
#include "createTriMesh.h"
#include "triMeshGeo.h"

#include "../common/primitiveTemplates.h"

#include <nlohmann/json.hpp>
#include <tbb/global_control.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
namespace ES = VegaFEM::EigenSupport;
using json = nlohmann::json;
using namespace MedialAxisRepresentation;

namespace
{
// Voxel size of the paper's experiments. The constants of the original implementation (minRadius in
// rfunc.cpp, the width of the smoothed Heaviside in gradientOptimizer.cpp, the default radius, ...) are
// absolute lengths tuned for this voxel size, so every problem is mapped onto a grid with this cell size and
// all user-facing lengths are expressed in voxels.
constexpr double kCellSize = 0.01;
constexpr double kMinRadius = 0.02;  // = minRadius in rfunc.cpp (radii are clamped to it)
constexpr double kThetaMin = 1e-2;   // = theta_min in gradientOptimizer.cpp
constexpr double kConvergenceThreshold = 1e-5;  // = stopping criterion of gradientOptimizer.cpp
constexpr long long kDefaultMaxVoxels = 500000;
constexpr int kFormatVersion = 1;

struct InputError : public std::runtime_error
{
  using std::runtime_error::runtime_error;
};

using GridKey = std::array<int, 3>;
using GridKeyMap = std::unordered_map<GridKey, int, ES::IntArrayHash<3>, ES::IntArrayEqual<3>>;

struct LoadRecord
{
  int vertex;
  ES::V3d force;
};

struct Problem
{
  std::string name, description;
  std::array<int, 3> res{ 0, 0, 0 };

  std::vector<ES::V3d> gridVertices;
  std::vector<std::array<int, 8>> cells;
  std::vector<GridKey> gridVertexKey;  // vertex id -> integer grid coordinates
  GridKeyMap vertexMap;                // integer grid coordinates -> vertex id

  std::vector<ES::V3d> skeletonVertices;    // world coordinates
  std::vector<std::array<int, 2>> edges;    // as passed to the optimizer
  std::vector<int> flippedEdges;            // edges whose end points were swapped (see orientEdges)

  std::vector<int> fixedVertices;  // sorted, unique
  ES::VXd fext;
  std::vector<LoadRecord> loads;  // accumulated per vertex

  double volumeFraction = 0.2;
  int method = 0;
  std::string methodName = "msd";
  int maxIterations = 200;
  double initialRadiusVoxels = 4.0;
  bool keepIterationObjs = false;

  std::vector<std::string> warnings;
  json normalizedConfig;
};

// ------------------------------------------------------------------------------------------------
// small helpers
// ------------------------------------------------------------------------------------------------
std::string fmtDouble(double v)
{
  std::ostringstream oss;
  oss << v;
  return oss.str();
}

// rounds to a multiple of q (a power of ten) and returns the double closest to the decimal value, so that the
// JSON outputs read 0.105 rather than 0.10500000000000001
double roundTo(double v, double q)
{
  int decimals = std::clamp((int)std::lround(-std::log10(q)), 0, 17);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, std::round(v / q) * q);
  return std::strtod(buf, nullptr);
}

void checkKeys(const json &obj, const std::set<std::string> &allowed, const std::string &where, Problem &P)
{
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    if (!allowed.count(it.key()))
      P.warnings.emplace_back("unknown key '" + it.key() + "' in " + where + " (ignored)");
  }
}

double readNumber(const json &j, const std::string &what)
{
  if (!j.is_number())
    throw InputError(what + " must be a number");
  double v = j.get<double>();
  if (!std::isfinite(v))
    throw InputError(what + " must be finite");
  return v;
}

int readInt(const json &j, const std::string &what)
{
  if (j.is_number_integer() || j.is_number_unsigned())
    return j.get<int>();
  if (j.is_number_float()) {
    double v = j.get<double>();
    if (std::isfinite(v) && std::floor(v) == v && std::abs(v) < 2e9)
      return (int)v;
  }
  throw InputError(what + " must be an integer");
}

ES::V3d readVec3(const json &j, const std::string &what)
{
  if (!j.is_array() || j.size() != 3)
    throw InputError(what + " must be an array of 3 numbers");
  return ES::V3d(readNumber(j[0], what + "[0]"), readNumber(j[1], what + "[1]"), readNumber(j[2], what + "[2]"));
}

json vecToJson(const ES::V3d &v, double q = 1e-9)
{
  return json::array({ roundTo(v[0], q), roundTo(v[1], q), roundTo(v[2], q) });
}

bool isIntegerValued(double v)
{
  return std::abs(v - std::round(v)) < 1e-12;
}

// ------------------------------------------------------------------------------------------------
// grid: identical to src/tests/shapeOpt/shapeOpt.cpp (same vertex numbering, same arithmetic), so that a
// paper experiment described in JSON reproduces the original driver exactly
// ------------------------------------------------------------------------------------------------
void buildGrid(Problem &P)
{
  ES::V3i corners[] = {
    Eigen::Vector3i(0, 0, 0),
    Eigen::Vector3i(0, 0, 1),
    Eigen::Vector3i(0, 1, 0),
    Eigen::Vector3i(0, 1, 1),
    Eigen::Vector3i(1, 0, 0),
    Eigen::Vector3i(1, 0, 1),
    Eigen::Vector3i(1, 1, 0),
    Eigen::Vector3i(1, 1, 1)
  };

  ES::V3i cellResolution{ P.res[0], P.res[1], P.res[2] };
  double cellSize = kCellSize;

  int vtxID_counter = 0;
  P.cells.reserve((size_t)P.res[0] * P.res[1] * P.res[2]);
  for (int x = 0; x < cellResolution[0]; x++) {
    for (int y = 0; y < cellResolution[1]; y++) {
      for (int z = 0; z < cellResolution[2]; z++) {
        std::array<int, 8> cellVertexID;

        for (int i = 0; i < 8; i++) {
          GridKey vidx{
            x + corners[i][0],
            y + corners[i][1],
            z + corners[i][2],
          };

          auto it = P.vertexMap.find(vidx);
          if (it == P.vertexMap.end()) {
            P.vertexMap.emplace(vidx, vtxID_counter);
            cellVertexID[i] = vtxID_counter;
            vtxID_counter++;
          }
          else {
            cellVertexID[i] = it->second;
          }
        }

        P.cells.emplace_back(cellVertexID);
      }
    }
  }

  P.gridVertices.resize(vtxID_counter);
  P.gridVertexKey.resize(vtxID_counter);
  for (const auto &pr_v : P.vertexMap) {
    ES::V3i vtxGridID(pr_v.first[0], pr_v.first[1], pr_v.first[2]);
    ES::V3d vtxPos = vtxGridID.cast<double>() * cellSize;
    P.gridVertices[pr_v.second] = vtxPos;
    P.gridVertexKey[pr_v.second] = pr_v.first;
  }
}

int cellIndex(const Problem &P, int x, int y, int z)
{
  return (x * P.res[1] + y) * P.res[2] + z;
}

// world position of a point given in voxel coordinates (integer i = center of voxel i)
ES::V3d voxelToWorld(const Problem &P, const ES::V3d &v)
{
  bool integral = isIntegerValued(v[0]) && isIntegerValued(v[1]) && isIntegerValued(v[2]);
  if (integral) {
    int x = (int)std::round(v[0]), y = (int)std::round(v[1]), z = (int)std::round(v[2]);
    if (x >= 0 && y >= 0 && z >= 0 && x < P.res[0] && y < P.res[1] && z < P.res[2]) {
      // exactly as shapeOpt.cpp: the average of the 8 corners of the cell
      const auto &cell = P.cells[cellIndex(P, x, y, z)];
      ES::V3d center(0, 0, 0);
      for (int j = 0; j < 8; j++) {
        center += P.gridVertices[cell[j]];
      }
      center /= 8.0;
      return center;
    }
  }
  return (v + ES::V3d::Constant(0.5)) * kCellSize;
}

// ------------------------------------------------------------------------------------------------
// skeleton
// ------------------------------------------------------------------------------------------------
void readSkeleton(const json &jsk, Problem &P)
{
  if (!jsk.is_object())
    throw InputError("'skeleton' must be an object");
  checkKeys(jsk, { "coordinates", "vertices", "edges", "comment" }, "skeleton", P);

  std::string coords = jsk.value("coordinates", std::string("voxel"));
  if (coords != "voxel" && coords != "grid")
    throw InputError("skeleton.coordinates must be \"voxel\" (integer i = center of voxel i) or \"grid\" (integer i = grid plane i)");

  if (!jsk.contains("vertices") || !jsk["vertices"].is_array() || jsk["vertices"].empty())
    throw InputError("skeleton.vertices must be a non-empty array of [x, y, z]");

  const json &jv = jsk["vertices"];
  for (size_t i = 0; i < jv.size(); i++) {
    ES::V3d v = readVec3(jv[i], "skeleton.vertices[" + std::to_string(i) + "]");
    ES::V3d gridCoord = (coords == "voxel") ? (v + ES::V3d::Constant(0.5)) : v;
    for (int d = 0; d < 3; d++) {
      if (gridCoord[d] < -1e-9 || gridCoord[d] > P.res[d] + 1e-9) {
        throw InputError("skeleton.vertices[" + std::to_string(i) + "] = [" + fmtDouble(v[0]) + ", " + fmtDouble(v[1]) + ", " + fmtDouble(v[2]) +
          "] lies outside of the design domain (" + coords + " coordinates; the domain spans " +
          (coords == "voxel" ? std::string("[-0.5, n - 0.5]") : std::string("[0, n]")) + " along each axis)");
      }
    }
    P.skeletonVertices.emplace_back(coords == "voxel" ? voxelToWorld(P, v) : ES::V3d(v * kCellSize));
  }

  if (P.skeletonVertices.size() > 1000)
    throw InputError("too many skeleton vertices (" + std::to_string(P.skeletonVertices.size()) + " > 1000)");

  for (size_t i = 0; i < P.skeletonVertices.size(); i++) {
    for (size_t j = i + 1; j < P.skeletonVertices.size(); j++) {
      if ((P.skeletonVertices[i] - P.skeletonVertices[j]).norm() < 1e-9)
        throw InputError("skeleton.vertices[" + std::to_string(i) + "] and skeleton.vertices[" + std::to_string(j) + "] coincide");
    }
  }

  if (jsk.contains("edges") && !jsk["edges"].is_array())
    throw InputError("skeleton.edges must be an array of [i, j]");

  std::set<std::pair<int, int>> seen;
  const json jEdges = jsk.value("edges", json::array());
  for (size_t ei = 0; ei < jEdges.size(); ei++) {
    const json &je = jEdges[ei];
    std::string what = "skeleton.edges[" + std::to_string(ei) + "]";
    if (!je.is_array() || je.size() != 2)
      throw InputError(what + " must be an array [i, j] of two vertex indices");
    int a = readInt(je[0], what + "[0]"), b = readInt(je[1], what + "[1]");
    int nv = (int)P.skeletonVertices.size();
    if (a < 0 || b < 0 || a >= nv || b >= nv)
      throw InputError(what + " = [" + std::to_string(a) + ", " + std::to_string(b) + "] references a vertex that does not exist (there are " + std::to_string(nv) + " vertices)");
    if (a == b)
      throw InputError(what + " is a self-loop");
    if (!seen.emplace(std::min(a, b), std::max(a, b)).second)
      throw InputError(what + " = [" + std::to_string(a) + ", " + std::to_string(b) + "] is a duplicate edge");
    P.edges.push_back({ a, b });
  }

  // EdgeRFunction (rfunc.cpp) builds the rotation that maps +x onto the edge direction and uses the identity
  // whenever the edge is (anti-)parallel to x. For an edge pointing along -x this places the primitive on the
  // wrong side of its first end point. The edges of the paper's experiments never point along -x; for user
  // input we simply swap the end points of such edges, which describes the same edge.
  for (int ei = 0; ei < (int)P.edges.size(); ei++) {
    ES::V3d d = (P.skeletonVertices[P.edges[ei][1]] - P.skeletonVertices[P.edges[ei][0]]).normalized();
    if (d[0] < -0.999) {
      std::swap(P.edges[ei][0], P.edges[ei][1]);
      P.flippedEdges.push_back(ei);
    }
  }

  // connectivity (informative only: the optimizer does not require a connected graph)
  if (!P.skeletonVertices.empty()) {
    std::vector<int> parent(P.skeletonVertices.size());
    for (size_t i = 0; i < parent.size(); i++)
      parent[i] = (int)i;
    std::function<int(int)> find = [&](int x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
    for (const auto &e : P.edges)
      parent[find(e[0])] = find(e[1]);
    std::set<int> roots;
    for (size_t i = 0; i < parent.size(); i++)
      roots.insert(find((int)i));
    if (roots.size() > 1)
      P.warnings.emplace_back("the skeletal graph has " + std::to_string(roots.size()) + " connected components");
  }
}

// ------------------------------------------------------------------------------------------------
// boundary conditions: selectors over the grid vertices (integer grid coordinates 0..n)
// ------------------------------------------------------------------------------------------------
std::vector<int> collectBox(const Problem &P, const ES::V3d &bmin, const ES::V3d &bmax)
{
  std::array<int, 3> lo, hi;
  for (int d = 0; d < 3; d++) {
    lo[d] = std::max(0, (int)std::ceil(std::min(bmin[d], bmax[d]) - 1e-9));
    hi[d] = std::min(P.res[d], (int)std::floor(std::max(bmin[d], bmax[d]) + 1e-9));
  }

  std::vector<int> ids;
  for (int x = lo[0]; x <= hi[0]; x++)
    for (int y = lo[1]; y <= hi[1]; y++)
      for (int z = lo[2]; z <= hi[2]; z++) {
        auto it = P.vertexMap.find(GridKey{ x, y, z });
        if (it != P.vertexMap.end())
          ids.push_back(it->second);
      }
  return ids;
}

std::vector<int> collectSphere(const Problem &P, const ES::V3d &centerGrid, double radius)
{
  ES::V3d r3 = ES::V3d::Constant(radius);
  std::vector<int> candidates = collectBox(P, centerGrid - r3, centerGrid + r3);
  std::vector<int> ids;
  for (int vid : candidates) {
    ES::V3d g = P.gridVertices[vid] / kCellSize;
    if ((g - centerGrid).norm() < radius)
      ids.push_back(vid);
  }
  return ids;
}

std::vector<int> selectGridVertices(const json &sel, const std::string &where, Problem &P, const std::set<std::string> &extraKeys)
{
  if (!sel.is_object() || !sel.contains("type") || !sel["type"].is_string())
    throw InputError(where + " must be an object with a \"type\" (face | box | sphere | vertex | points)");

  std::string type = sel["type"].get<std::string>();
  std::set<std::string> keys = extraKeys;
  keys.insert({ "type", "comment" });

  std::vector<int> ids;
  if (type == "face") {
    keys.insert("face");
    checkKeys(sel, keys, where, P);
    std::string face = sel.value("face", std::string());
    static const std::map<std::string, std::pair<int, int>> faces = {
      { "x-", { 0, 0 } }, { "x+", { 0, 1 } }, { "y-", { 1, 0 } }, { "y+", { 1, 1 } }, { "z-", { 2, 0 } }, { "z+", { 2, 1 } }
    };
    auto it = faces.find(face);
    if (it == faces.end())
      throw InputError(where + ".face must be one of x-, x+, y-, y+, z-, z+");
    ES::V3d bmin(0, 0, 0), bmax(P.res[0], P.res[1], P.res[2]);
    int axis = it->second.first;
    double v = it->second.second ? P.res[axis] : 0.0;
    bmin[axis] = bmax[axis] = v;
    ids = collectBox(P, bmin, bmax);
  }
  else if (type == "box") {
    keys.insert({ "min", "max" });
    checkKeys(sel, keys, where, P);
    if (!sel.contains("min") || !sel.contains("max"))
      throw InputError(where + " (box) needs \"min\" and \"max\" (grid coordinates, inclusive)");
    ids = collectBox(P, readVec3(sel["min"], where + ".min"), readVec3(sel["max"], where + ".max"));
  }
  else if (type == "sphere") {
    keys.insert({ "center", "radius", "coordinates" });
    checkKeys(sel, keys, where, P);
    if (!sel.contains("center") || !sel.contains("radius"))
      throw InputError(where + " (sphere) needs \"center\" and \"radius\" (in voxels)");
    ES::V3d c = readVec3(sel["center"], where + ".center");
    std::string coords = sel.value("coordinates", std::string("grid"));
    if (coords == "voxel")
      c += ES::V3d::Constant(0.5);
    else if (coords != "grid")
      throw InputError(where + ".coordinates must be \"grid\" or \"voxel\"");
    double r = readNumber(sel["radius"], where + ".radius");
    if (r <= 0)
      throw InputError(where + ".radius must be positive");
    ids = collectSphere(P, c, r);
  }
  else if (type == "vertex") {
    keys.insert({ "vertex", "radius" });
    checkKeys(sel, keys, where, P);
    if (!sel.contains("vertex"))
      throw InputError(where + " (vertex) needs \"vertex\": the index of a skeleton vertex");
    int k = readInt(sel["vertex"], where + ".vertex");
    if (k < 0 || k >= (int)P.skeletonVertices.size())
      throw InputError(where + ".vertex = " + std::to_string(k) + " is not a skeleton vertex index");
    double r = sel.contains("radius") ? readNumber(sel["radius"], where + ".radius") : 2.0;
    if (r <= 0)
      throw InputError(where + ".radius must be positive");
    ids = collectSphere(P, P.skeletonVertices[k] / kCellSize, r);
  }
  else if (type == "points") {
    keys.insert("points");
    checkKeys(sel, keys, where, P);
    if (!sel.contains("points") || !sel["points"].is_array() || sel["points"].empty())
      throw InputError(where + " (points) needs \"points\": a non-empty array of integer grid coordinates [i, j, k]");
    for (size_t i = 0; i < sel["points"].size(); i++) {
      const json &jp = sel["points"][i];
      std::string w = where + ".points[" + std::to_string(i) + "]";
      if (!jp.is_array() || jp.size() != 3)
        throw InputError(w + " must be [i, j, k]");
      GridKey key{ readInt(jp[0], w + "[0]"), readInt(jp[1], w + "[1]"), readInt(jp[2], w + "[2]") };
      auto it = P.vertexMap.find(key);
      if (it == P.vertexMap.end())
        throw InputError(w + " is not a grid vertex (valid range: 0..n along each axis)");
      ids.push_back(it->second);
    }
  }
  else {
    throw InputError(where + ".type '" + type + "' is unknown (expected face | box | sphere | vertex | points)");
  }

  if (ids.empty())
    throw InputError(where + " does not select any grid vertex");

  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

void readBoundaryConditions(const json &jbc, Problem &P)
{
  if (!jbc.is_object())
    throw InputError("'boundary_conditions' must be an object");
  checkKeys(jbc, { "fixed", "loads", "comment" }, "boundary_conditions", P);

  if (!jbc.contains("fixed") || !jbc["fixed"].is_array() || jbc["fixed"].empty())
    throw InputError("boundary_conditions.fixed must be a non-empty array of selectors");
  if (!jbc.contains("loads") || !jbc["loads"].is_array() || jbc["loads"].empty())
    throw InputError("boundary_conditions.loads must be a non-empty array of selectors with a \"force\"");

  std::vector<int> fixedIDs;
  for (size_t i = 0; i < jbc["fixed"].size(); i++) {
    std::vector<int> ids = selectGridVertices(jbc["fixed"][i], "boundary_conditions.fixed[" + std::to_string(i) + "]", P, {});
    fixedIDs.insert(fixedIDs.end(), ids.begin(), ids.end());
  }
  std::sort(fixedIDs.begin(), fixedIDs.end());
  fixedIDs.erase(std::unique(fixedIDs.begin(), fixedIDs.end()), fixedIDs.end());

  // the fixed vertices must remove all rigid motions, i.e. contain three non-collinear points
  {
    const ES::V3d &p0 = P.gridVertices[fixedIDs[0]];
    int i1 = -1;
    for (int vid : fixedIDs) {
      if ((P.gridVertices[vid] - p0).norm() > 0.5 * kCellSize) {
        i1 = vid;
        break;
      }
    }
    bool ok = false;
    if (i1 >= 0) {
      ES::V3d e1 = (P.gridVertices[i1] - p0).normalized();
      for (int vid : fixedIDs) {
        if ((P.gridVertices[vid] - p0).cross(e1).norm() > 0.5 * kCellSize) {
          ok = true;
          break;
        }
      }
    }
    if (!ok)
      throw InputError("the fixed grid vertices are collinear (" + std::to_string(fixedIDs.size()) +
        " vertices); fix at least three non-collinear grid vertices, otherwise the structure can move rigidly and the stiffness matrix is singular");
  }

  P.fixedVertices = fixedIDs;
  P.fext.setZero(P.gridVertices.size() * 3);

  std::set<int> fixedSet(fixedIDs.begin(), fixedIDs.end());
  std::map<int, ES::V3d> accumulated;
  for (size_t i = 0; i < jbc["loads"].size(); i++) {
    const json &jl = jbc["loads"][i];
    std::string where = "boundary_conditions.loads[" + std::to_string(i) + "]";
    std::vector<int> ids = selectGridVertices(jl, where, P, { "force", "total_force" });

    bool hasForce = jl.contains("force"), hasTotal = jl.contains("total_force");
    if (hasForce == hasTotal)
      throw InputError(where + " needs exactly one of \"force\" (applied to every selected grid vertex) or \"total_force\" (split evenly)");
    ES::V3d f = hasForce ? readVec3(jl["force"], where + ".force") : ES::V3d(readVec3(jl["total_force"], where + ".total_force") / (double)ids.size());
    if (f.norm() == 0.0)
      throw InputError(where + " has a zero force");

    int numOnFixed = 0;
    for (int vid : ids) {
      if (fixedSet.count(vid))
        numOnFixed++;
      auto it = accumulated.find(vid);
      if (it == accumulated.end())
        accumulated.emplace(vid, f);
      else
        it->second += f;
    }
    if (numOnFixed)
      P.warnings.emplace_back(where + ": " + std::to_string(numOnFixed) + " of the " + std::to_string(ids.size()) + " loaded grid vertices are fixed; the force has no effect there");
  }

  bool anyEffective = false;
  for (const auto &[vid, f] : accumulated) {
    if (!f.allFinite() || f.norm() > 1e9)
      throw InputError("loads: the force on grid vertex " + std::to_string(vid) + " is not finite or larger than 1e9");
    P.fext.segment<3>(vid * 3) = f;
    P.loads.push_back({ vid, f });
    if (!fixedSet.count(vid) && f.norm() > 0)
      anyEffective = true;
  }
  if (!anyEffective)
    throw InputError("all loads are applied to fixed grid vertices; the structure would not deform");
}

void readOptimization(const json &jo, Problem &P)
{
  if (!jo.is_object())
    throw InputError("'optimization' must be an object");
  checkKeys(jo, { "volume_fraction", "method", "max_iterations", "initial_radius", "allow_large_grid", "comment" }, "optimization", P);

  if (jo.contains("volume_fraction")) {
    P.volumeFraction = readNumber(jo["volume_fraction"], "optimization.volume_fraction");
    if (P.volumeFraction <= 0 || P.volumeFraction > 1)
      throw InputError("optimization.volume_fraction must be in (0, 1]");
  }

  if (jo.contains("method")) {
    const json &jm = jo["method"];
    std::string m = jm.is_string() ? jm.get<std::string>() : (jm.is_number_integer() ? std::to_string(jm.get<int>()) : std::string("?"));
    std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (m == "msd" || m == "ours" || m == "0") {
      P.method = 0, P.methodName = "msd";
    }
    else if (m == "ma" || m == "medial_axis" || m == "1") {
      P.method = 1, P.methodName = "ma";
    }
    else {
      throw InputError("optimization.method must be \"msd\" (medial skeletal diagram, the paper's method) or \"ma\" (medial-axis baseline: spheres + cones)");
    }
  }

  if (jo.contains("max_iterations")) {
    P.maxIterations = readInt(jo["max_iterations"], "optimization.max_iterations");
    if (P.maxIterations < 1 || P.maxIterations > 100000)
      throw InputError("optimization.max_iterations must be in [1, 100000]");
  }

  if (jo.contains("initial_radius")) {
    P.initialRadiusVoxels = readNumber(jo["initial_radius"], "optimization.initial_radius");
    if (P.initialRadiusVoxels <= 0)
      throw InputError("optimization.initial_radius must be positive (in voxels)");
    // VertexRFunction ray-casts the primitive with a segment of length 5 (world units = 500 voxels) and aborts
    // when it misses, so primitives must stay well below that size
    if (P.initialRadiusVoxels > 100)
      throw InputError("optimization.initial_radius must be at most 100 voxels");
  }
  if (P.initialRadiusVoxels * kCellSize < kMinRadius)
    throw InputError("optimization.initial_radius must be at least " + fmtDouble(kMinRadius / kCellSize) + " voxels, the minimal primitive radius of the implementation (smaller radii have a zero gradient and never change)");
}

Problem readProblem(const json &jin)
{
  Problem P;
  if (!jin.is_object())
    throw InputError("the configuration must be a JSON object");
  checkKeys(jin, { "name", "description", "grid", "skeleton", "boundary_conditions", "optimization", "output", "comment", "paper_reference" }, "the configuration", P);

  P.name = jin.value("name", std::string("unnamed"));
  P.description = jin.value("description", std::string());

  // grid
  if (!jin.contains("grid"))
    throw InputError("'grid' is required, e.g. \"grid\": {\"resolution\": [60, 40, 6]}");
  if (!jin["grid"].is_object())
    throw InputError("'grid' must be an object, e.g. \"grid\": {\"resolution\": [60, 40, 6]}");
  const json &jg = jin["grid"];
  checkKeys(jg, { "resolution", "comment" }, "grid", P);
  if (!jg.contains("resolution") || !jg["resolution"].is_array() || jg["resolution"].size() != 3)
    throw InputError("grid.resolution must be [nx, ny, nz] (number of voxels along each axis)");
  for (int d = 0; d < 3; d++) {
    P.res[d] = readInt(jg["resolution"][d], "grid.resolution[" + std::to_string(d) + "]");
    if (P.res[d] < 1 || P.res[d] > 2000)
      throw InputError("grid.resolution entries must be in [1, 2000]");
  }

  // optimization (read before allocating the grid because of allow_large_grid)
  const json jopt = jin.value("optimization", json::object());
  readOptimization(jopt, P);

  long long numVoxels = (long long)P.res[0] * P.res[1] * P.res[2];
  bool allowLarge = jopt.is_object() && jopt.value("allow_large_grid", false);
  if (numVoxels > kDefaultMaxVoxels && !allowLarge)
    throw InputError("the grid has " + std::to_string(numVoxels) + " voxels (> " + std::to_string(kDefaultMaxVoxels) +
      "); every iteration factorizes a stiffness matrix with ~3 x #voxels unknowns. Set optimization.allow_large_grid = true to run it anyway");

  buildGrid(P);

  if (!jin.contains("skeleton"))
    throw InputError("'skeleton' is required");
  readSkeleton(jin["skeleton"], P);

  if (!jin.contains("boundary_conditions"))
    throw InputError("'boundary_conditions' is required");
  readBoundaryConditions(jin["boundary_conditions"], P);

  if (jin.contains("output")) {
    const json &jout = jin["output"];
    if (!jout.is_object())
      throw InputError("'output' must be an object");
    checkKeys(jout, { "keep_iteration_objs", "comment" }, "output", P);
    P.keepIterationObjs = jout.value("keep_iteration_objs", false);
  }

  P.normalizedConfig = jin;
  P.normalizedConfig["optimization"] = json{
    { "volume_fraction", P.volumeFraction },
    { "method", P.methodName },
    { "max_iterations", P.maxIterations },
    { "initial_radius", P.initialRadiusVoxels },
  };
  return P;
}

// ------------------------------------------------------------------------------------------------
// meta data (everything the viewer needs to display the problem)
// ------------------------------------------------------------------------------------------------
json makeMeta(const Problem &P, const TriMeshGeo &sphereTemplate, const TriMeshGeo &cylinderTemplate)
{
  json jSkeleton;
  jSkeleton["vertices"] = json::array();
  for (const auto &v : P.skeletonVertices)
    jSkeleton["vertices"].push_back(vecToJson(v));
  jSkeleton["edges"] = json::array();
  for (const auto &e : P.edges)
    jSkeleton["edges"].push_back({ e[0], e[1] });
  jSkeleton["flipped_edges"] = P.flippedEdges;

  json jFixed = json::array();
  for (int vid : P.fixedVertices)
    jFixed.push_back(P.gridVertexKey[vid]);

  json jLoads = json::array();
  ES::V3d total(0, 0, 0);
  for (const auto &l : P.loads) {
    jLoads.push_back({ { "vertex", P.gridVertexKey[l.vertex] }, { "force", vecToJson(l.force, 1e-12) } });
    total += l.force;
  }

  auto trianglesToJson = [](const TriMeshGeo &m) {
    json t = json::array();
    for (int ti = 0; ti < m.numTriangles(); ti++)
      t.push_back({ m.triVtxID(ti, 0), m.triVtxID(ti, 1), m.triVtxID(ti, 2) });
    return t;
  };

  json meta;
  meta["format_version"] = kFormatVersion;
  meta["name"] = P.name;
  meta["description"] = P.description;
  meta["grid"] = {
    { "resolution", P.res },
    { "cell_size", kCellSize },
    { "num_voxels", P.cells.size() },
    { "num_grid_vertices", P.gridVertices.size() },
    { "voxel_index", "index = (x * ny + y) * nz + z; voxel (x, y, z) spans [x, x+1] x [y, y+1] x [z, z+1] in grid coordinates" },
    { "world_from_grid", "world = grid * cell_size" },
  };
  meta["skeleton"] = jSkeleton;
  meta["boundary_conditions"] = {
    { "fixed_vertices", jFixed },
    { "loads", jLoads },
    { "total_force", vecToJson(total, 1e-12) },
  };
  meta["optimization"] = {
    { "method", P.methodName },
    { "method_id", P.method },
    { "volume_fraction", P.volumeFraction },
    { "max_iterations", P.maxIterations },
    { "initial_radius_voxels", P.initialRadiusVoxels },
    { "initial_radius", P.initialRadiusVoxels * kCellSize },
    { "convergence_threshold", kConvergenceThreshold },
    { "young_modulus", 1e6 },
    { "poisson_ratio", 0.45 },
    { "theta_min", kThetaMin },
  };
  meta["primitives"] = {
    { "num_vertex_primitives", P.skeletonVertices.size() },
    { "num_edge_primitives", P.edges.size() },
    { "sphere_template", { { "num_vertices", sphereTemplate.numVertices() }, { "triangles", trianglesToJson(sphereTemplate) } } },
    { "cylinder_template", { { "num_vertices", cylinderTemplate.numVertices() }, { "triangles", trianglesToJson(cylinderTemplate) } } },
    { "positions_layout", "per frame: vertex primitives first (sphere_template.num_vertices points each), then edge primitives (cylinder_template.num_vertices points each), xyz world coordinates" },
  };
  meta["theta_encoding"] = "frames/theta_XXXX.u8: one byte per voxel, round(255 * (theta - theta_min) / (1 - theta_min)); the shape is theta > 0.5";
  meta["warnings"] = P.warnings;
  return meta;
}

// ------------------------------------------------------------------------------------------------
// optimization run
// ------------------------------------------------------------------------------------------------
void writeJsonFile(const fs::path &path, const json &j, int indent = 2)
{
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream ofs(tmp);
    if (!ofs)
      throw std::runtime_error("cannot write " + tmp.string());
    ofs << j.dump(indent) << '\n';
  }
  fs::rename(tmp, path);  // atomic for readers (the web server polls these files)
}

void emitEvent(const json &j)
{
  std::cout << "@@MSD " << j.dump() << std::endl;
}

// positions of the deformed template meshes, computed exactly as in VertexRFunction::func / EdgeRFunction::func
void computePrimitivePositions(const ES::VXd &x, int nV, int nE, const TriMeshGeo &sphereUnit, const TriMeshGeo &cylinder,
  std::vector<ES::V3d> &positions)
{
  int nS = sphereUnit.numVertices(), nC = cylinder.numVertices();
  int nSphereDOFs = nV * (nS + 6);
  positions.clear();
  positions.reserve(nV * nS + nE * nC);

  for (int vi = 0; vi < nV; vi++) {
    const double *xv = x.data() + vi * (nS + 6);
    ES::V3d center(xv[3], xv[4], xv[5]);
    for (int k = 0; k < nS; k++) {
      double r = std::max(xv[6 + k], kMinRadius);
      positions.emplace_back(center + ES::V3d(sphereUnit.pos(k)[0], sphereUnit.pos(k)[1], sphereUnit.pos(k)[2]) * r);
    }
  }

  for (int ei = 0; ei < nE; ei++) {
    const double *xe = x.data() + nSphereDOFs + ei * (nC + 9);
    ES::V3d c0(xe[3], xe[4], xe[5]), c1(xe[6], xe[7], xe[8]);

    ES::V3d d0(1, 0, 0);
    ES::V3d d1 = (c1 - c0).normalized();
    ES::M3d R;
    if (std::abs(std::abs(d0.dot(d1)) - 1) < 1e-5) {
      R.setIdentity();
    }
    else {
      ES::V3d rotAxis = d0.cross(d1).normalized();
      double rotAngle = std::acos(std::clamp(d0.dot(d1), -1.0, 1.0));
      R = Eigen::AngleAxisd(rotAngle, rotAxis).toRotationMatrix();
    }

    double s = (c1 - c0).norm();
    for (int k = 0; k < nC - 2; k++) {
      double r = std::max(xe[9 + k], kMinRadius);
      ES::V3d p0(cylinder.pos(k)[0] * s, cylinder.pos(k)[1] * r, cylinder.pos(k)[2] * r);
      positions.emplace_back(R * p0 + c0);
    }
    double rA = std::max(xe[9 + nC - 2], kMinRadius);
    positions.emplace_back(R * ES::V3d(s + rA, 0, 0) + c0);
    double rB = std::max(xe[9 + nC - 1], kMinRadius);
    positions.emplace_back(R * ES::V3d(-rB, 0, 0) + c0);
  }
}

struct RunContext
{
  fs::path outDir;
  const Problem *P = nullptr;
  TriMeshGeo sphereUnit, cylinder;
  std::chrono::steady_clock::time_point t0;
  std::ofstream history;
  json lastRecord;
  int lastIteration = -1;
  std::vector<ES::V3d> lastPrimitivePositions;
  std::vector<ES::V3d> lastSkeleton;
};

std::string frameName(const char *prefix, int iter, const char *ext)
{
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s%04d%s", prefix, iter, ext);
  return buf;
}

void onIteration(RunContext &ctx, const OptimizeContinuousIterationInfo &info)
{
  const Problem &P = *ctx.P;
  const ES::VXd &theta = *info.theta;
  const ES::VXd &x = *info.reducedTheta;
  if (!std::isfinite(info.compliance) || !std::isfinite(info.energy) || !x.allFinite())
    throw std::runtime_error("the optimization diverged at iteration " + std::to_string(info.iteration) + " (non-finite compliance or primitive parameters); check the load magnitudes");
  int nV = (int)P.skeletonVertices.size(), nE = (int)P.edges.size();
  int nS = info.numSphereTemplateVertices;
  double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - ctx.t0).count();

  // current skeleton (centers of the vertex primitives)
  std::vector<ES::V3d> skeleton(nV);
  for (int vi = 0; vi < nV; vi++)
    skeleton[vi] = x.segment<3>(vi * (nS + 6) + 3);

  std::vector<ES::V3d> positions;
  computePrimitivePositions(x, nV, nE, ctx.sphereUnit, ctx.cylinder, positions);

  // voxel densities, one byte per voxel
  std::vector<std::uint8_t> q(theta.size());
  int filled = 0;
  for (Eigen::Index i = 0; i < theta.size(); i++) {
    double h = (theta[i] - kThetaMin) / (1.0 - kThetaMin);
    q[i] = (std::uint8_t)std::clamp((int)std::lround(h * 255.0), 0, 255);
    if (theta[i] > 0.5)
      filled++;
  }
  {
    std::ofstream ofs(ctx.outDir / "frames" / frameName("theta_", info.iteration, ".u8"), std::ios::binary);
    ofs.write(reinterpret_cast<const char *>(q.data()), (std::streamsize)q.size());
  }

  json jSkeleton = json::array();
  for (const auto &v : skeleton)
    jSkeleton.push_back(vecToJson(v, 1e-8));

  json jPositions = json::array();
  for (const auto &p : positions) {
    jPositions.push_back(roundTo(p[0], 1e-7));
    jPositions.push_back(roundTo(p[1], 1e-7));
    jPositions.push_back(roundTo(p[2], 1e-7));
  }

  // radii of the primitives (diagnostics)
  double rmin = 1e100, rmax = 0;
  for (int vi = 0; vi < nV; vi++)
    for (int k = 0; k < nS; k++) {
      double r = x[vi * (nS + 6) + 6 + k];
      rmin = std::min(rmin, r), rmax = std::max(rmax, r);
    }
  int nC = info.numCylinderTemplateVertices;
  for (int ei = 0; ei < nE; ei++)
    for (int k = 0; k < nC; k++) {
      double r = x[nV * (nS + 6) + ei * (nC + 9) + 9 + k];
      rmin = std::min(rmin, r), rmax = std::max(rmax, r);
    }

  json record = {
    { "iteration", info.iteration },
    { "compliance", info.compliance },
    { "volume_penalty", info.volumePenalty },
    { "energy", info.energy },
    { "volume_fraction", info.volumeFraction },
    { "filled_voxels", filled },
    { "gradient_norm", info.gradientNorm },
    { "step_size", info.stepSize },
    { "max_change", info.maxChange },
    { "line_search_iterations", info.lineSearchIterations },
    { "radius_min_voxels", roundTo(rmin / kCellSize, 1e-6) },
    { "radius_max_voxels", roundTo(rmax / kCellSize, 1e-6) },
    { "elapsed", roundTo(elapsed, 1e-3) },
    { "vertices", jSkeleton },
  };

  writeJsonFile(ctx.outDir / "frames" / frameName("frame_", info.iteration, ".json"),
    json{ { "iteration", info.iteration }, { "vertices", jSkeleton }, { "primitive_positions", jPositions } }, -1);

  ctx.history << record.dump() << std::endl;  // flushed: the web server tails this file
  json event = record;
  event["event"] = "iteration";
  emitEvent(event);

  // the original optimizer writes iterXXXX-sample.obj / iterXXXX-sk.obj (voxel shape / skeleton) at every
  // iteration; only the most recent ones are kept unless output.keep_iteration_objs is true
  if (!P.keepIterationObjs && info.iteration >= 2) {
    std::error_code ec;
    fs::remove(ctx.outDir / frameName("iter", info.iteration - 2, "-sample.obj"), ec);
    fs::remove(ctx.outDir / frameName("iter", info.iteration - 2, "-sk.obj"), ec);
  }

  ctx.lastRecord = record;
  ctx.lastIteration = info.iteration;
  ctx.lastPrimitivePositions = positions;
  ctx.lastSkeleton = skeleton;
}

void writeFinalOutputs(RunContext &ctx, bool converged)
{
  const Problem &P = *ctx.P;

  // voxel shape of the last iteration, as written by the original code (boxes of the voxels with theta > 0.5)
  fs::path lastVoxelObj = ctx.lastIteration >= 1 ? ctx.outDir / frameName("iter", ctx.lastIteration - 1, "-sample.obj") : ctx.outDir / "initial-sample.obj";
  std::error_code ec;
  if (fs::exists(lastVoxelObj))
    fs::copy_file(lastVoxelObj, ctx.outDir / "final_voxels.obj", fs::copy_options::overwrite_existing, ec);

  // skeleton as polylines
  {
    std::ofstream ofs(ctx.outDir / "final_skeleton.obj");
    ofs << "# optimized skeletal graph (msdShapeOpt)\n";
    for (const auto &v : ctx.lastSkeleton)
      ofs << "v " << v[0] << ' ' << v[1] << ' ' << v[2] << '\n';
    for (const auto &e : P.edges)
      ofs << "l " << e[0] + 1 << ' ' << e[1] + 1 << '\n';
  }

  // union of the generalized enveloping primitives (the zero level set of the R-function is the boundary of
  // this union; voxels with theta > 0.5 are the voxels whose centers are inside it)
  {
    TriMeshGeo prims;
    int nV = (int)P.skeletonVertices.size(), nE = (int)P.edges.size();
    int nS = ctx.sphereUnit.numVertices(), nC = ctx.cylinder.numVertices();
    int offset = 0;
    for (int pi = 0; pi < nV + nE; pi++) {
      const TriMeshGeo &tmpl = pi < nV ? ctx.sphereUnit : ctx.cylinder;
      int n = pi < nV ? nS : nC;
      TriMeshGeo m = tmpl;
      for (int k = 0; k < n; k++)
        m.pos(k) = Vec3d(ctx.lastPrimitivePositions[offset + k].data());
      prims.addMesh(m);
      offset += n;
    }
    prims.save((ctx.outDir / "final_primitives.obj").string());
  }

  json result = {
    { "status", "completed" },
    { "converged", converged },
    { "iterations", ctx.lastIteration },
    { "final", ctx.lastRecord },
    { "elapsed", ctx.lastRecord.value("elapsed", 0.0) },
    { "files", { "final_voxels.obj", "final_primitives.obj", "final_skeleton.obj", "history.jsonl", "energy.txt" } },
  };
  writeJsonFile(ctx.outDir / "result.json", result);
}

int runOptimization(Problem &P, const fs::path &outDirIn, const std::optional<fs::path> &templatesDir)
{
  fs::path outDir = fs::absolute(outDirIn);
  fs::create_directories(outDir / "frames");

  // template primitives: the optimizer loads s.obj / c1.obj from its working directory
  if (templatesDir) {
    for (const char *f : { "s.obj", "c1.obj" }) {
      if (!fs::exists(*templatesDir / f))
        throw InputError("template file " + (*templatesDir / f).string() + " does not exist");
      fs::copy_file(*templatesDir / f, outDir / f, fs::copy_options::overwrite_existing);
    }
  }
  else {
    if (!MSDShapeOpt::makeSphereTemplate().save((outDir / "s.obj").string()) ||
      !MSDShapeOpt::makeCylinderTemplate().save((outDir / "c1.obj").string()))
      throw std::runtime_error("cannot write the template meshes to " + outDir.string());
  }

  RunContext ctx;
  ctx.outDir = outDir;
  ctx.P = &P;

  // load the templates exactly as optimizeContinuous() does
  if (LibIGLInterface::loadObjMesh((outDir / "s.obj").string().c_str(), ctx.sphereUnit) != 0 ||
    LibIGLInterface::loadObjMesh((outDir / "c1.obj").string().c_str(), ctx.cylinder) != 0)
    throw std::runtime_error("cannot load the template meshes");
  for (int vi = 0; vi < ctx.sphereUnit.numVertices(); vi++)
    ctx.sphereUnit.pos(vi) /= len(ctx.sphereUnit.pos(vi));
  if (ctx.cylinder.numVertices() != 14)
    throw InputError("the cylinder template must have 14 vertices (the medial-axis subspace of the original code assumes it)");

  json meta = makeMeta(P, ctx.sphereUnit, ctx.cylinder);
  writeJsonFile(outDir / "meta.json", meta);
  writeJsonFile(outDir / "config.json", P.normalizedConfig);

  for (const auto &w : P.warnings)
    std::cout << "warning: " << w << std::endl;

  ctx.history.open(outDir / "history.jsonl", std::ios::trunc);
  if (!ctx.history)
    throw std::runtime_error("cannot write " + (outDir / "history.jsonl").string());

  emitEvent(json{ { "event", "start" }, { "name", P.name }, { "num_voxels", P.cells.size() }, { "num_grid_vertices", P.gridVertices.size() },
    { "num_vertices", P.skeletonVertices.size() }, { "num_edges", P.edges.size() }, { "method", P.methodName }, { "max_iterations", P.maxIterations } });

  // the original code writes its outputs (energy.txt, iterXXXX-*.obj, ...) to the working directory
  fs::current_path(outDir);

  OptimizeContinuousOptions options;
  options.maxIterations = P.maxIterations;
  options.initialRadius = P.initialRadiusVoxels * kCellSize;
  options.iterationCallback = [&ctx](const OptimizeContinuousIterationInfo &info) { onIteration(ctx, info); };

  ctx.t0 = std::chrono::steady_clock::now();
  optimizeContinuous(P.gridVertices, P.cells, P.skeletonVertices, P.edges, P.fext, P.fixedVertices, P.volumeFraction, P.method, options);

  bool converged = ctx.lastIteration >= 1 && ctx.lastRecord.value("max_change", 1.0) < kConvergenceThreshold &&
                   ctx.lastRecord.value("gradient_norm", 0.0) > 0 && ctx.lastRecord.value("line_search_iterations", 0) < 15;
  writeFinalOutputs(ctx, converged);

  emitEvent(json{ { "event", "finished" }, { "converged", converged }, { "iterations", ctx.lastIteration }, { "final", ctx.lastRecord } });
  return 0;
}

void printUsage()
{
  std::cerr << "usage: msdShapeOpt <config.json> [-o <output_dir>] [--templates <dir>] [--threads <n>] [--validate-only]\n"
               "  -o, --output-dir   directory for the results (default: ./msd_run_<name>)\n"
               "  --templates        directory containing s.obj and c1.obj (default: the built-in templates)\n"
               "  --threads          maximum number of threads (default: all cores)\n"
               "  --validate-only    check the configuration, print {\"ok\": ..., \"meta\": ...} and exit\n";
}
}  // namespace

int main(int argc, char *argv[])
{
  std::optional<fs::path> configPath, outDir, templatesDir;
  int numThreads = 0;
  bool validateOnly = false;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&](const char *opt) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value after " << opt << "\n";
        printUsage();
        std::exit(2);
      }
      return argv[++i];
    };

    if (a == "-o" || a == "--output-dir")
      outDir = next("--output-dir");
    else if (a == "--templates")
      templatesDir = next("--templates");
    else if (a == "--threads")
      numThreads = std::atoi(next("--threads").c_str());
    else if (a == "--validate-only")
      validateOnly = true;
    else if (a == "-h" || a == "--help") {
      printUsage();
      return 0;
    }
    else if (!a.empty() && a[0] == '-') {
      std::cerr << "unknown option " << a << "\n";
      printUsage();
      return 2;
    }
    else if (!configPath)
      configPath = a;
    else {
      printUsage();
      return 2;
    }
  }

  if (!configPath) {
    printUsage();
    return 2;
  }

  Problem P;
  try {
    std::ifstream ifs(*configPath);
    if (!ifs)
      throw InputError("cannot open " + configPath->string());

    json jin;
    try {
      jin = json::parse(ifs, nullptr, true, /*ignore_comments=*/true);
    }
    catch (const json::parse_error &e) {
      throw InputError(std::string("invalid JSON: ") + e.what());
    }

    P = readProblem(jin);

    if (validateOnly) {
      json meta = makeMeta(P, [] { TriMeshGeo s = MSDShapeOpt::makeSphereTemplate(); for (int i = 0; i < s.numVertices(); i++) s.pos(i) /= len(s.pos(i)); return s; }(),
        MSDShapeOpt::makeCylinderTemplate());
      std::cout << json{ { "ok", true }, { "warnings", P.warnings }, { "meta", meta } }.dump() << std::endl;
      return 0;
    }
  }
  catch (const std::exception &e) {
    if (validateOnly) {
      std::cout << json{ { "ok", false }, { "error", e.what() } }.dump() << std::endl;
    }
    else {
      std::cerr << "error: " << e.what() << std::endl;
      emitEvent(json{ { "event", "error" }, { "kind", "input" }, { "message", e.what() } });
    }
    return 2;
  }

  std::unique_ptr<tbb::global_control> threadLimit;
  if (numThreads > 0)
    threadLimit = std::make_unique<tbb::global_control>(tbb::global_control::max_allowed_parallelism, (size_t)numThreads);

  if (!outDir) {
    std::string safe;
    for (char c : P.name)
      safe += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    outDir = fs::current_path() / ("msd_run_" + safe);
  }

  Logger::init();

  try {
    return runOptimization(P, *outDir, templatesDir);
  }
  catch (const InputError &e) {
    std::cerr << "error: " << e.what() << std::endl;
    emitEvent(json{ { "event", "error" }, { "kind", "input" }, { "message", e.what() } });
    return 2;
  }
  catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << std::endl;
    emitEvent(json{ { "event", "error" }, { "kind", "runtime" }, { "message", e.what() } });
    return 3;
  }
}
