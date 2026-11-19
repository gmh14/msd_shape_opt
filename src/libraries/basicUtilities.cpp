#include "basicUtilities.h"

void MedialAxisRepresentation::BasicUtilities::triMeshGeoToMatrices(const TriMeshGeo &mesh, VegaFEM::EigenSupport::MXd &vtx, VegaFEM::EigenSupport::MXi &tri)
{
  vtx.resize(mesh.numVertices(), 3);
  tri.resize(mesh.numTriangles(), 3);

  for (int i = 0; i < mesh.numVertices(); i++) {
    const auto &v = mesh.pos(i);
    VegaFEM::EigenSupport::V3d v3d(v[0], v[1], v[2]);
    vtx.row(i) = v3d;
  }

  for (int i = 0; i < mesh.numTriangles(); i++) {
    const auto &t = mesh.tri(i);
    tri.row(i) = VegaFEM::EigenSupport::V3i(t[0], t[1], t[2]);
  }
}

void MedialAxisRepresentation::BasicUtilities::matricesToTriMeshGeo(const VegaFEM::EigenSupport::MXd &vtx, const VegaFEM::EigenSupport::MXi &tri, TriMeshGeo &mesh)
{
  mesh.clear();

  for (Eigen::Index i = 0; i < vtx.rows(); i++) {
    Vec3d p(vtx(i, 0), vtx(i, 1), vtx(i, 2));
    mesh.addPos(p);
  }

  for (Eigen::Index i = 0; i < tri.rows(); i++) {
    Vec3i t(tri(i, 0), tri(i, 1), tri(i, 2));
    mesh.addTri(t);
  }
}

TriMeshGeo MedialAxisRepresentation::BasicUtilities::matricesToTriMeshGeo(const VegaFEM::EigenSupport::MXd &vtx, const VegaFEM::EigenSupport::MXi &tri)
{
  TriMeshGeo mesh;
  matricesToTriMeshGeo(vtx, tri, mesh);

  return mesh;
}