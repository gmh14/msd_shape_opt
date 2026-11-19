#pragma once

#include "triMeshGeo.h"
#include "EigenDef.h"
#include <vector>

namespace MedialAxisRepresentation
{
namespace LibIGLInterface
{
int loadObjMesh(const char *filename, TriMeshGeo &meshOut);
int loadOffMesh(const char *filename, TriMeshGeo &meshOut);
int loadStlMesh(const char *filename, TriMeshGeo &meshOut);

int saveOffMesh(const char *filename, const TriMeshGeo &meshIn, const double *scalarField = nullptr);
int saveObjMesh(const char *filename, const TriMeshGeo &meshIn, const std::vector<Vec3d> &normals, const std::vector<Vec2d> &texs);

void harmonic(const TriMeshGeo &meshIn, int order, VegaFEM::EigenSupport::SpMatD &K);
void harmonic(const VegaFEM::EigenSupport::MXd &vtx, const VegaFEM::EigenSupport::MXi &tet, int order, VegaFEM::EigenSupport::SpMatD &K);
void graph_laplacian_matrix(const VegaFEM::EigenSupport::MXi &F, VegaFEM::EigenSupport::SpMatD &K);
void graph_laplacian_matrix(const VegaFEM::EigenSupport::SpMatD &Adj, VegaFEM::EigenSupport::SpMatD &K);
void cotangent(const TriMeshGeo &meshIn, VegaFEM::EigenSupport::SpMatD &L);
void mass(const TriMeshGeo &meshIn, VegaFEM::EigenSupport::SpMatD &M);
void diag(const VegaFEM::EigenSupport::VXd &v, VegaFEM::EigenSupport::SpMatD &M);

void boundaryLoop(const TriMeshGeo &meshIn, std::vector<int> &boundary);
void boundaryLoops(const TriMeshGeo &meshIn, std::vector<std::vector<int>> &boundary);

void arapParametrization(const TriMeshGeo &meshIn, const std::vector<std::tuple<int, double, double>> &boundaryConditions, TriMeshGeo &meshOut);
void harmonicParametrization(const TriMeshGeo &meshIn, TriMeshGeo &meshOut);

void bbwWeights(const VegaFEM::EigenSupport::MXd &vtx, const VegaFEM::EigenSupport::MXi &tet,  
  const VegaFEM::EigenSupport::MXd &vtxHandles, 
  VegaFEM::EigenSupport::MXd &weights);

void windingNumber(const VegaFEM::EigenSupport::MXd &vtx, const VegaFEM::EigenSupport::MXi &pieces, const VegaFEM::EigenSupport::MXd &queryPt, VegaFEM::EigenSupport::VXd &outValues);

void gaussianCurvature(const TriMeshGeo &meshIn, VegaFEM::EigenSupport::VXd &K);

void computeVertexNormal(const TriMeshGeo &meshIn, std::vector<Vec3d> &normals);
void meanCuravtures(const TriMeshGeo &meshIn, VegaFEM::EigenSupport::VXd &h);
}  // namespace LibIGLInterface
}  // namespace MedialAxisRepresentation