#include "opt/gradientOptimizer.h"
#include "logger.h"

#include "EigenSupport.h"
#include "createTriMesh.h"

#include <unordered_map>

namespace ES = VegaFEM::EigenSupport;
using namespace MedialAxisRepresentation;

int main00(int argc, char *argv[])
{
  Logger::init();

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

  ES::V3i cellResolution{ 30, 20, 3 };
  ES::V3i gridResolution = cellResolution + ES::V3i::Ones();
  double cellSize = 0.02;

  std::vector<ES::V3d> gridVertices;
  std::vector<std::array<int, 8>> cells;

  std::unordered_map<std::array<int, 3>, int, ES::IntArrayHash<3>, ES::IntArrayEqual<3>> vertexMap;
  std::unordered_map<std::array<int, 3>, int, ES::IntArrayHash<3>, ES::IntArrayEqual<3>> cellMap;
  int vtxID_counter = 0;
  for (int x = 0; x < cellResolution[0]; x++) {
    for (int y = 0; y < cellResolution[1]; y++) {
      for (int z = 0; z < cellResolution[2]; z++) {
        std::array<int, 8> cellVertexID;

        for (int i = 0; i < 8; i++) {
          std::array<int, 3> vidx{
            x + corners[i][0],
            y + corners[i][1],
            z + corners[i][2],
          };

          auto it = vertexMap.find(vidx);
          if (it == vertexMap.end()) {
            vertexMap.emplace(vidx, vtxID_counter);
            cellVertexID[i] = vtxID_counter;
            vtxID_counter++;
          }
          else {
            cellVertexID[i] = it->second;
          }
        }

        cells.emplace_back(cellVertexID);
        cellMap.emplace(std::array<int, 3>{ x, y, z }, (int)cells.size() - 1);
      }
    }
  }

  gridVertices.resize(vtxID_counter);
  for (const auto &pr_v : vertexMap) {
    ES::V3i vtxGridID(pr_v.first[0], pr_v.first[1], pr_v.first[2]);
    ES::V3d vtxPos = vtxGridID.cast<double>() * cellSize;
    gridVertices[pr_v.second] = vtxPos;
  }

  TriMeshGeo volMeshAll;
  for (int i = 0; i < (int)cells.size(); i++) {
    volMeshAll.addMesh(
      createBoxMesh(Vec3d(gridVertices[cells[i][0]].data()), Vec3d(gridVertices[cells[i][7]].data())));
  }

  volMeshAll.save("r.obj");

  std::vector<ES::V3i> skeletonVertexCellID = {
    ES::V3i(0, 9, 1),
    ES::V3i(14, 9, 1),
    ES::V3i(29, 9, 1),

    ES::V3i(0, 18, 1),
    ES::V3i(20, 18, 1),
    ES::V3i(10, 13, 1)
  };

  std::vector<std::array<int, 2>> skeletonEdges = {
    {0, 1},
    {1, 2},
    {3, 4},
    {2, 4},
    {1, 4},
    {1, 5},
    {3, 5},
    {0, 5}
  };

  std::vector<ES::V3d> skeletonVertices;
  for (const auto &vcid : skeletonVertexCellID) {
    auto it = cellMap.find(std::array<int, 3>{ vcid[0], vcid[1], vcid[2] });
    ALOG(it != cellMap.end());

    ES::V3d center(0, 0, 0);
    for (int j = 0; j < 8; j++) {
      center += gridVertices[cells[it->second][j]];
    }
    center /= 8.0;

    skeletonVertices.emplace_back(center);
  }

  TriMeshGeo skMesh;
  for (const auto &e : skeletonEdges) {
    skMesh.addMesh(createSingleTriangleMesh(Vec3d(skeletonVertices[e[0]].data()), Vec3d(skeletonVertices[e[1]].data()), Vec3d(skeletonVertices[e[0]].data()) + Vec3d(1e-6)));
  }
  skMesh.save("sk.obj");

  std::vector<int> fixedVertices;
  for (int vi = 0; vi < (int)gridVertices.size(); vi++) {
    if (std::abs(gridVertices[vi][0]) < 1e-5) {
      fixedVertices.emplace_back(vi);
    }
  }

  ES::VXd fext(gridVertices.size() * 3);
  fext.setZero();

  for (int vi = 0; vi < (int)gridVertices.size(); vi++) {
    if (std::abs(gridVertices[vi][0] - cellSize * cellResolution[0]) < 1e-5 &&
      std::abs(gridVertices[vi][1] - cellSize * 9) < 1e-5) {
      fext.segment<3>(vi * 3) = ES::V3d(0, -10, 0);
    }
  }

  optimizeContinuous(gridVertices, cells, skeletonVertices, skeletonEdges,
    fext, fixedVertices, 0.3);

  return 0;
}

int main(int argc, char *argv[])
{
  Logger::init();

  int caseID = std::atoi(argv[1]);
  int method = std::atoi(argv[2]);

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

  ES::V3i cellResolution{ 60, 40, 6 };
  ES::V3i gridResolution = cellResolution + ES::V3i::Ones();
  double cellSize = 0.01;

  std::vector<ES::V3d> gridVertices;
  std::vector<std::array<int, 8>> cells;

  std::unordered_map<std::array<int, 3>, int, ES::IntArrayHash<3>, ES::IntArrayEqual<3>> vertexMap;
  std::unordered_map<std::array<int, 3>, int, ES::IntArrayHash<3>, ES::IntArrayEqual<3>> cellMap;
  int vtxID_counter = 0;
  for (int x = 0; x < cellResolution[0]; x++) {
    for (int y = 0; y < cellResolution[1]; y++) {
      for (int z = 0; z < cellResolution[2]; z++) {
        std::array<int, 8> cellVertexID;

        for (int i = 0; i < 8; i++) {
          std::array<int, 3> vidx{
            x + corners[i][0],
            y + corners[i][1],
            z + corners[i][2],
          };

          auto it = vertexMap.find(vidx);
          if (it == vertexMap.end()) {
            vertexMap.emplace(vidx, vtxID_counter);
            cellVertexID[i] = vtxID_counter;
            vtxID_counter++;
          }
          else {
            cellVertexID[i] = it->second;
          }
        }

        cells.emplace_back(cellVertexID);
        cellMap.emplace(std::array<int, 3>{ x, y, z }, (int)cells.size() - 1);
      }
    }
  }

  gridVertices.resize(vtxID_counter);
  for (const auto &pr_v : vertexMap) {
    ES::V3i vtxGridID(pr_v.first[0], pr_v.first[1], pr_v.first[2]);
    ES::V3d vtxPos = vtxGridID.cast<double>() * cellSize;
    gridVertices[pr_v.second] = vtxPos;
  }

  TriMeshGeo volMeshAll;
  for (int i = 0; i < (int)cells.size(); i++) {
    volMeshAll.addMesh(
      createBoxMesh(Vec3d(gridVertices[cells[i][0]].data()), Vec3d(gridVertices[cells[i][7]].data())));
  }

  volMeshAll.save("r.obj");

  std::vector<ES::V3i> skeletonVertexCellID = {
    ES::V3i(0, 10, 2),
    ES::V3i(30, 10, 2),
    ES::V3i(50, 10, 2),

    ES::V3i(0, 30, 2),
    ES::V3i(50, 30, 2),
    ES::V3i(15, 20, 2)
  };

  if (caseID >= 4) {
    skeletonVertexCellID.pop_back();
  }

  std::vector<ES::V3d> skeletonVertices;
  for (const auto &vcid : skeletonVertexCellID) {
    auto it = cellMap.find(std::array<int, 3>{ vcid[0], vcid[1], vcid[2] });
    ALOG(it != cellMap.end());

    ES::V3d center(0, 0, 0);
    for (int j = 0; j < 8; j++) {
      center += gridVertices[cells[it->second][j]];
    }
    center /= 8.0;

    skeletonVertices.emplace_back(center);
  }

  std::vector<int> fixedVertices;
  for (int vi = 0; vi < (int)gridVertices.size(); vi++) {
    if (std::abs(gridVertices[vi][0]) < 1e-5) {
      fixedVertices.emplace_back(vi);
    }
  }

  ES::VXd fext(gridVertices.size() * 3);
  fext.setZero();

  TriMeshGeo mm;
  for (int vi = 0; vi < (int)gridVertices.size(); vi++) {
    if (std::abs(gridVertices[vi][0] - cellSize * 50) < 1e-5 &&
      std::abs(gridVertices[vi][1] - cellSize * 10) < 1e-5) {
      fext.segment<3>(vi * 3) = ES::V3d(0, -10, 0);
      mm.addPos(Vec3d(gridVertices[vi].data()));
    }
  }
  mm.save("f.obj");

  std::vector<std::array<int, 2>> skeletonEdges;

  if (caseID == 0) {
    skeletonEdges = {
      {0, 1},
      {1, 2},
      {3, 4},
      {2, 4},
      {1, 4},
      {1, 5},
      {3, 5},
      {0, 5}
    };
  }
  else if (caseID == 1) {
    skeletonEdges = {
      {0, 1},
      {1, 2},
      {3, 4},
      {2, 4},
      {1, 4},
      {1, 5},
      {3, 5},
      {0, 3}
    };
  }
  else if (caseID == 2) {
    skeletonEdges = {
      {0, 1},
      {1, 2},
      {3, 4},
      {2, 4},
      {0, 3}
    };
  }
  else if (caseID == 3) {
    skeletonEdges = {
      {0, 1},
      {1, 2},
      {3, 4},
      {2, 4},
      {4, 5},
      {2, 5},
      {3, 5},
      {0, 5}
    };
  }
  else if (caseID == 4) {
    skeletonEdges = {
      {0, 1},
      {1, 2},
      {3, 4},
      {2, 4}
    };
  }
  else if (caseID == 5) {
    skeletonEdges = {
      {0, 1},
      {1, 2},
      {3, 4},
      {2, 4},
      {0, 4}
    };
  }
  else {
    return 1;
  }

  TriMeshGeo skMesh;
  for (const auto &e : skeletonEdges) {
    skMesh.addMesh(createSingleTriangleMesh(Vec3d(skeletonVertices[e[0]].data()), Vec3d(skeletonVertices[e[1]].data()), Vec3d(skeletonVertices[e[0]].data()) + Vec3d(1e-6)));
  }
  skMesh.save("sk.obj");

  optimizeContinuous(gridVertices, cells, skeletonVertices, skeletonEdges,
    fext, fixedVertices, 0.2, method);

  return 0;
}

int mainz(int argc, char *argv[])
{
  Logger::init();

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

  ES::V3i cellResolution{ 60, 30, 60 };
  ES::V3i gridResolution = cellResolution + ES::V3i::Ones();
  double cellSize = 0.01;

  std::vector<ES::V3d> gridVertices;
  std::vector<std::array<int, 8>> cells;

  std::unordered_map<std::array<int, 3>, int, ES::IntArrayHash<3>, ES::IntArrayEqual<3>> vertexMap;
  std::unordered_map<std::array<int, 3>, int, ES::IntArrayHash<3>, ES::IntArrayEqual<3>> cellMap;
  int vtxID_counter = 0;
  for (int x = 0; x < cellResolution[0]; x++) {
    for (int y = 0; y < cellResolution[1]; y++) {
      for (int z = 0; z < cellResolution[2]; z++) {
        std::array<int, 8> cellVertexID;

        for (int i = 0; i < 8; i++) {
          std::array<int, 3> vidx{
            x + corners[i][0],
            y + corners[i][1],
            z + corners[i][2],
          };

          auto it = vertexMap.find(vidx);
          if (it == vertexMap.end()) {
            vertexMap.emplace(vidx, vtxID_counter);
            cellVertexID[i] = vtxID_counter;
            vtxID_counter++;
          }
          else {
            cellVertexID[i] = it->second;
          }
        }

        cells.emplace_back(cellVertexID);
        cellMap.emplace(std::array<int, 3>{ x, y, z }, (int)cells.size() - 1);
      }
    }
  }

  gridVertices.resize(vtxID_counter);
  for (const auto &pr_v : vertexMap) {
    ES::V3i vtxGridID(pr_v.first[0], pr_v.first[1], pr_v.first[2]);
    ES::V3d vtxPos = vtxGridID.cast<double>() * cellSize;
    gridVertices[pr_v.second] = vtxPos;
  }

  TriMeshGeo volMeshAll;
  for (int i = 0; i < (int)cells.size(); i++) {
    volMeshAll.addMesh(
      createBoxMesh(Vec3d(gridVertices[cells[i][0]].data()), Vec3d(gridVertices[cells[i][7]].data())));
  }

  volMeshAll.save("r.obj");

  std::vector<ES::V3i> skeletonVertexCellID = {
    ES::V3i(10, 15, 10),
    ES::V3i(30, 15, 10),
    ES::V3i(50, 15, 10),

    ES::V3i(10, 15, 30),
    ES::V3i(30, 15, 30),
    ES::V3i(50, 15, 30),

    ES::V3i(10, 15, 50),
    ES::V3i(30, 15, 50),
    ES::V3i(50, 15, 50),
  };

  std::vector<std::array<int, 2>> skeletonEdges = {
    {0, 1},
    {0, 3},
    {5, 8},
    {7, 8},
    {1, 4},
    {3, 4},
    {4, 5},
    {4, 7},
    {3, 6},
    {6, 7},
    {1, 2},
    {2, 5},
  };

  std::vector<ES::V3d> skeletonVertices;
  for (const auto &vcid : skeletonVertexCellID) {
    auto it = cellMap.find(std::array<int, 3>{ vcid[0], vcid[1], vcid[2] });
    ALOG(it != cellMap.end());

    ES::V3d center(0, 0, 0);
    for (int j = 0; j < 8; j++) {
      center += gridVertices[cells[it->second][j]];
    }
    center /= 8.0;

    skeletonVertices.emplace_back(center);
  }

  std::vector<int> fixedVertices;
  std::vector<ES::V3d> fixedLocations = {
    skeletonVertices[0],
    skeletonVertices[2],
    skeletonVertices[6],
    skeletonVertices[8],
  };
  TriMeshGeo mm0;
  for (int vi = 0; vi < (int)gridVertices.size(); vi++) {
    for (int j = 0; j < fixedLocations.size(); j++) {
      if ((fixedLocations[j] - gridVertices[vi]).norm() < cellSize * 2) {
        fixedVertices.emplace_back(vi);
        mm0.addPos(Vec3d(gridVertices[vi].data()));
      }
    }
  }
  mm0.save("fv.obj");

  ES::VXd fext(gridVertices.size() * 3);
  fext.setZero();

  TriMeshGeo mm;
  for (int vi = 0; vi < (int)gridVertices.size(); vi++) {
    if ((skeletonVertices[4] - gridVertices[vi]).norm() < cellSize * 2) {
      fext.segment<3>(vi * 3) = ES::V3d(0, -10, 0);
      mm.addPos(Vec3d(gridVertices[vi].data()));
    }
  }
  mm.save("f.obj");

  TriMeshGeo skMesh;
  for (const auto &e : skeletonEdges) {
    skMesh.addMesh(createSingleTriangleMesh(Vec3d(skeletonVertices[e[0]].data()), Vec3d(skeletonVertices[e[1]].data()), Vec3d(skeletonVertices[e[0]].data()) + Vec3d(1e-6)));
  }
  skMesh.save("sk.obj");

  optimizeContinuous(gridVertices, cells, skeletonVertices, skeletonEdges,
    fext, fixedVertices, 0.05);

  return 0;
}
