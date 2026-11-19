#pragma once

#include "logger.h"

#include "EigenDef.h"
#include "triMeshGeo.h"
#include "triMeshNeighbor.h"
#include "basicAlgorithms.h"

#include <ranges>
#include <iterator>
#include <type_traits>

namespace MedialAxisRepresentation
{
namespace BasicUtilities
{
void triMeshGeoToMatrices(const TriMeshGeo &mesh, VegaFEM::EigenSupport::MXd &vtx, VegaFEM::EigenSupport::MXi &tri);
void matricesToTriMeshGeo(const VegaFEM::EigenSupport::MXd &vtx, const VegaFEM::EigenSupport::MXi &tri, TriMeshGeo &mesh);
TriMeshGeo matricesToTriMeshGeo(const VegaFEM::EigenSupport::MXd &vtx, const VegaFEM::EigenSupport::MXi &tri);

template<typename Iter>
  requires std::forward_iterator<Iter> && std::is_integral_v<std::decay_t<typename std::iter_value_t<Iter>>>
bool isSubMeshValidDisk(const TriMeshGeo &meshOriginal, Iter itBeg, Iter itEnd)
{
  using namespace MedialAxisRepresentation;

  TriMeshGeo dummyMesh;
  dummyMesh.positions() = meshOriginal.positions();
  for (const auto &ti : std::ranges::subrange{ itBeg, itEnd })
    dummyMesh.triangles().emplace_back(meshOriginal.tri(ti));

  try {
    TriMeshNeighbor neighbor(dummyMesh);
    std::vector<int> vtxFlags(meshOriginal.numVertices(), 0);
    std::vector<int> vtxIDs;
    vtxIDs.reserve(meshOriginal.numVertices() * 10);
    for (const auto &t : dummyMesh.triangles()) {
      vtxIDs.emplace_back(t[0]);
      vtxIDs.emplace_back(t[1]);
      vtxIDs.emplace_back(t[2]);
    }
    sortAndDeduplicateWithErase(vtxIDs);

    int numLoops = 0;
    while (1) {
      int curVID = -1;
      int prevVID = -1;
      for (int vid : vtxIDs) {
        // find the shared edges with vid
        std::array<std::array<int, 2>, 2> neighboringEdges;
        int edgeCount = 0;
        for (int ntri : neighbor.getVtxNearbyTriangles(vid)) {
          for (int j = 0; j < 3; j++) {
            // is a neighbor edge and shared with vid
            if (neighbor.getTriangleNeighbors(ntri)[j] < 0 &&
              (dummyMesh.triVtxID(ntri, j) == vid || dummyMesh.triVtxID(ntri, (j + 1) % 3) == vid)) {
              if (edgeCount == 2) {
                return false;
              }
              else {
                neighboringEdges[edgeCount][0] = dummyMesh.triVtxID(ntri, j);
                neighboringEdges[edgeCount][1] = dummyMesh.triVtxID(ntri, (j + 1) % 3);
                edgeCount++;
              }
            }
          }
        }

        if (curVID < 0 && edgeCount == 2 && vtxFlags[vid] == 0) {
          curVID = vid;
        }
      }

      if (curVID < 0)
        break;

      if (numLoops) {
        return false;
      }

      vtxFlags[curVID] = 1;
      int startVID = curVID;
      do {
        std::array<int, 2> neighboringVID;
        int numVIDs = 0;
        for (int ntri : neighbor.getVtxNearbyTriangles(curVID)) {
          for (int j = 0; j < 3; j++) {
            // is a neighbor edge and shared with vid
            if (neighbor.getTriangleNeighbors(ntri)[j] < 0 &&
              (dummyMesh.triVtxID(ntri, j) == curVID || dummyMesh.triVtxID(ntri, (j + 1) % 3) == curVID)) {
              ALOG(numVIDs < 2);
              if (dummyMesh.triVtxID(ntri, j) == curVID)
                neighboringVID[numVIDs] = dummyMesh.triVtxID(ntri, (j + 1) % 3);
              else
                neighboringVID[numVIDs] = dummyMesh.triVtxID(ntri, j);

              numVIDs++;
            }
          }
        }

        if (prevVID < 0) {
          prevVID = curVID;
          curVID = neighboringVID[0];
        }
        else {
          if (prevVID == neighboringVID[0]) {
            prevVID = curVID;
            curVID = neighboringVID[1];
          }
          else {
            ALOG(prevVID == neighboringVID[1]);
            prevVID = curVID;
            curVID = neighboringVID[0];
          }
        }
        vtxFlags[curVID] = 1;
      } while (startVID != curVID);
      numLoops++;
    }

    if (numLoops == 1)
      return true;
    else
      return false;
  }
  catch (int err) {
    return false;
  }
}
}  // namespace BasicUtilities
}  // namespace MedialAxisRepresentation