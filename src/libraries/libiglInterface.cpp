#include "libiglInterface.h"
#include "basicUtilities.h"

#if defined(__GNUC__) || defined(__GNUG__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wsign-compare"
#  pragma GCC diagnostic ignored "-Wunused-local-typedefs"
#  pragma GCC diagnostic ignored "-Wunknown-pragmas"
#  pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#elif defined(_MSC_VER)
#  pragma warning(disable : 4244)
#endif

#include <igl/readOFF.h>
#include <igl/readOBJ.h>
#include <igl/readSTL.h>
#include <igl/writeOFF.h>
#include <igl/writeOBJ.h>

#include <igl/massmatrix.h>
#include <igl/cotmatrix.h>
#include <igl/harmonic.h>
#include <igl/boundary_loop.h>
#include <igl/adjacency_matrix.h>
#include <igl/diag.h>
#include <igl/sum.h>

#include <igl/winding_number.h>
#include <igl/gaussian_curvature.h>
#include <igl/invert_diag.h>

#include <igl/arap.h>
#include <igl/map_vertices_to_circle.h>
#include <igl/bbw.h>
#include <igl/boundary_conditions.h>

#include <igl/jet.h>

#include <igl/per_vertex_normals.h>

namespace ES = VegaFEM::EigenSupport;

int MedialAxisRepresentation::LibIGLInterface::loadObjMesh(const char *filename, TriMeshGeo &meshOut)
{
  ES::MXd vtx;
  ES::MXi tri;
  bool ret = igl::readOBJ(filename, vtx, tri);

  if (ret == false) {
    return 1;
  }

  meshOut = BasicUtilities::matricesToTriMeshGeo(vtx, tri);

  return 0;
}

int MedialAxisRepresentation::LibIGLInterface::loadOffMesh(const char *filename, TriMeshGeo &meshOut)
{
  ES::MXd vtx;
  ES::MXi tri;
  bool ret = igl::readOFF(filename, vtx, tri);

  if (ret == false) {
    return 1;
  }

  meshOut = BasicUtilities::matricesToTriMeshGeo(vtx, tri);

  return 0;
}

int MedialAxisRepresentation::LibIGLInterface::loadStlMesh(const char *filename, TriMeshGeo &meshOut)
{
  ES::MXd vtx, n;
  ES::MXi tri;

  std::ifstream infile(filename, std::ios_base::binary);
  if (!infile) {
    return 1;
  }

  bool ret = igl::readSTL(infile, vtx, tri, n);

  if (ret == false) {
    return 1;
  }

  meshOut = BasicUtilities::matricesToTriMeshGeo(vtx, tri);

  return 0;
}

int MedialAxisRepresentation::LibIGLInterface::saveOffMesh(const char *filename, const TriMeshGeo &meshIn, const double *scalarField)
{
  ES::MXd vtx;
  ES::MXi tri;
  BasicUtilities::triMeshGeoToMatrices(meshIn, vtx, tri);

  if (scalarField) {
    ES::VXd svec = ES::Mp<const ES::VXd>(scalarField, meshIn.numVertices());
    ES::MXd colors;

    igl::jet(svec, false, colors);
    bool ret = igl::writeOFF(filename, vtx, tri, colors);
    return ret ? 1 : 0;
  }
  else {
    bool ret = igl::writeOFF(filename, vtx, tri);
    return ret ? 1 : 0;
  }
}

int MedialAxisRepresentation::LibIGLInterface::saveObjMesh(const char *filename, const TriMeshGeo &meshIn, const std::vector<Vec3d> &normals, const std::vector<Vec2d> &texs)
{
  ES::MXd vtx;
  ES::MXi tri;
  BasicUtilities::triMeshGeoToMatrices(meshIn, vtx, tri);

  ES::MXd ns;
  if (normals.size()) {
    ns.resize(vtx.rows(), 3);
    for (int i = 0; i < (int)normals.size(); i++) {
      ns.row(i) = ES::V3d(normals[i][0], normals[i][1], normals[i][2]);
    }
  }

  ES::MXd ts;
  if (texs.size()) {
    ts.resize(vtx.rows(), 2);
    for (int i = 0; i < (int)texs.size(); i++) {
      ts.row(i) = ES::V2d(texs[i][0], texs[i][1]);
    }
  }

  bool ret = igl::writeOBJ(filename, vtx, tri, ns, tri, ts, tri);
  return ret ? 1 : 0;
}

void MedialAxisRepresentation::LibIGLInterface::harmonic(const TriMeshGeo &meshIn, int order, VegaFEM::EigenSupport::SpMatD &K)
{
  Eigen::SparseMatrix<double> sp_col;
  ES::MXd vtx;
  ES::MXi tri;
  BasicUtilities::triMeshGeoToMatrices(meshIn, vtx, tri);

  igl::harmonic(vtx, tri, order, sp_col);
  K = sp_col;
}

void MedialAxisRepresentation::LibIGLInterface::harmonic(const VegaFEM::EigenSupport::MXd &vtx, const VegaFEM::EigenSupport::MXi &tet, int order, VegaFEM::EigenSupport::SpMatD &K)
{
  Eigen::SparseMatrix<double> sp_col;

  igl::harmonic(vtx, tet, order, sp_col);
  K = sp_col;
}

void MedialAxisRepresentation::LibIGLInterface::graph_laplacian_matrix(const VegaFEM::EigenSupport::MXi &F, VegaFEM::EigenSupport::SpMatD &K)
{
  Eigen::SparseMatrix<double> A;
  igl::adjacency_matrix(F, A);
  Eigen::SparseVector<double> Asum;
  igl::sum(A, 1, Asum);
  Eigen::SparseMatrix<double> Adiag;
  igl::diag(Asum, Adiag);
  Eigen::SparseMatrix<double> U = A - Adiag;
  K = U;
}

void MedialAxisRepresentation::LibIGLInterface::graph_laplacian_matrix(const VegaFEM::EigenSupport::SpMatD &Adj, VegaFEM::EigenSupport::SpMatD &K)
{
  Eigen::SparseMatrix<double> A = Adj;
  Eigen::SparseVector<double> Asum;
  igl::sum(A, 1, Asum);
  Eigen::SparseMatrix<double> Adiag;
  igl::diag(Asum, Adiag);
  Eigen::SparseMatrix<double> U = A - Adiag;
  K = U;
}

void MedialAxisRepresentation::LibIGLInterface::cotangent(const TriMeshGeo &meshIn, VegaFEM::EigenSupport::SpMatD &L)
{
  Eigen::SparseMatrix<double> sp_col;
  ES::MXd vtx;
  ES::MXi tri;
  BasicUtilities::triMeshGeoToMatrices(meshIn, vtx, tri);

  igl::cotmatrix(vtx, tri, sp_col);
  L = sp_col;
}

void MedialAxisRepresentation::LibIGLInterface::mass(const TriMeshGeo &meshIn, VegaFEM::EigenSupport::SpMatD &M)
{
  Eigen::SparseMatrix<double> sp_col;
  ES::MXd vtx;
  ES::MXi tri;
  BasicUtilities::triMeshGeoToMatrices(meshIn, vtx, tri);

  igl::massmatrix(vtx, tri, igl::MASSMATRIX_TYPE_DEFAULT, sp_col);
  M = sp_col;
}

void MedialAxisRepresentation::LibIGLInterface::diag(const VegaFEM::EigenSupport::VXd &v, VegaFEM::EigenSupport::SpMatD &M)
{
  Eigen::SparseMatrix<double> sp_col;
  igl::diag(v, sp_col);
  M = sp_col;
}

void MedialAxisRepresentation::LibIGLInterface::boundaryLoop(const TriMeshGeo &meshIn, std::vector<int> &boundary)
{
  ES::MXd vtx;
  ES::MXi tri;
  BasicUtilities::triMeshGeoToMatrices(meshIn, vtx, tri);

  igl::boundary_loop(tri, boundary);
}

void MedialAxisRepresentation::LibIGLInterface::boundaryLoops(const TriMeshGeo &meshIn, std::vector<std::vector<int>> &boundary)
{
  ES::MXd vtx;
  ES::MXi tri;
  BasicUtilities::triMeshGeoToMatrices(meshIn, vtx, tri);

  igl::boundary_loop(tri, boundary);
}

void MedialAxisRepresentation::LibIGLInterface::harmonicParametrization(const TriMeshGeo &meshIn, TriMeshGeo &meshOut)
{
  ES::MXd V;
  ES::MXi F;
  BasicUtilities::triMeshGeoToMatrices(meshIn, V, F);

  // Compute the initial solution for ARAP (harmonic parametrization)
  ES::VXi bnd;
  igl::boundary_loop(F, bnd);

  ES::MXd bnd_uv;
  igl::map_vertices_to_circle(V, bnd, bnd_uv);

  ES::MXd V_uv;
  igl::harmonic(V, F, bnd, bnd_uv, 1, V_uv);

  meshOut = meshIn;
  for (int vi = 0; vi < meshOut.numVertices(); vi++) {
    meshOut.pos(vi) = Vec3d(V_uv(vi, 0), V_uv(vi, 1), 0);
  }
}

void MedialAxisRepresentation::LibIGLInterface::arapParametrization(const TriMeshGeo &meshIn, const std::vector<std::tuple<int, double, double>> &boundaryConditions, TriMeshGeo &meshOut)
{
  ES::MXd V;
  ES::MXi F;
  BasicUtilities::triMeshGeoToMatrices(meshIn, V, F);

  ES::MXd initial_guess;
  if (meshOut.numVertices() == meshIn.numVertices()) {
    initial_guess.resize(V.rows(), 2);
    for (int vi = 0; vi < meshOut.numVertices(); vi++) {
      initial_guess.row(vi) = ES::V2d(meshOut.pos(vi)[0], meshOut.pos(vi)[1]);
    }
  }
  else {
    // Compute the initial solution for ARAP (harmonic parametrization)
    ES::VXi bnd;
    igl::boundary_loop(F, bnd);

    bool hasFullBoundaryInput = true;
    for (int i = 0; i < (int)bnd.size(); i++) {
      bool found = false;
      for (const auto &pr : boundaryConditions) {
        if (std::get<0>(pr) == bnd[i]) {
          found = true;
          break;
        }
      }

      if (found == false) {
        hasFullBoundaryInput = false;
        break;
      }
    }

    ES::MXd bnd_uv;
    if (hasFullBoundaryInput) {
      bnd_uv.resize(bnd.size(), 2);
      for (int i = 0; i < (int)bnd.size(); i++) {
        bool found = false;
        for (const auto &pr : boundaryConditions) {
          if (std::get<0>(pr) == bnd[i]) {
            bnd_uv.row(i) = ES::V2d(std::get<1>(pr), std::get<2>(pr));
            found = true;
            break;
          }
        }

        if (found == false) {
          abort();
        }
      }
    }
    else {
      igl::map_vertices_to_circle(V, bnd, bnd_uv);
    }

    igl::harmonic(V, F, bnd, bnd_uv, 1, initial_guess);
  }

  // Add dynamic regularization to avoid to specify boundary conditions
  igl::ARAPData arap_data;
  arap_data.with_dynamics = true;

  Eigen::VectorXi b;
  Eigen::MatrixXd bc;

  if (boundaryConditions.size()) {
    b.resize(boundaryConditions.size());
    bc.resize(boundaryConditions.size(), 2);
    for (int inc = 0; const auto &pr : boundaryConditions) {
      b[inc] = std::get<0>(pr);
      bc.row(inc) = ES::V2d(std::get<1>(pr), std::get<2>(pr));
      inc++;
    }
  }
  else {
    b.setZero(0);
    bc.setZero(0, 0);
  }

  // Initialize ARAP
  arap_data.max_iter = 100;
  // 2 means that we're going to *solve* in 2d
  arap_precomputation(V, F, 2, b, arap_data);

  // Solve arap using the harmonic map as initial guess
  ES::MXd V_uv = initial_guess;

  arap_solve(bc, arap_data, V_uv);

  meshOut = meshIn;
  for (int vi = 0; vi < meshOut.numVertices(); vi++) {
    meshOut.pos(vi) = Vec3d(V_uv(vi, 0), V_uv(vi, 1), 0);
  }
}

void MedialAxisRepresentation::LibIGLInterface::windingNumber(const VegaFEM::EigenSupport::MXd &vtx, const VegaFEM::EigenSupport::MXi &pieces,
  const VegaFEM::EigenSupport::MXd &queryPt, VegaFEM::EigenSupport::VXd &outValues)
{
  igl::winding_number(vtx, pieces, queryPt, outValues);
}

void MedialAxisRepresentation::LibIGLInterface::gaussianCurvature(const TriMeshGeo &meshIn, VegaFEM::EigenSupport::VXd &K)
{
  ES::MXd V;
  ES::MXi F;
  BasicUtilities::triMeshGeoToMatrices(meshIn, V, F);

  igl::gaussian_curvature(V, F, K);
  Eigen::SparseMatrix<double> M, Minv;
  igl::massmatrix(V, F, igl::MASSMATRIX_TYPE_DEFAULT, M);
  igl::invert_diag(M, Minv);
  K = (Minv * K).eval();
}

void MedialAxisRepresentation::LibIGLInterface::computeVertexNormal(const TriMeshGeo &meshIn, std::vector<Vec3d> &normals)
{
  ES::MXd V;
  ES::MXi F;
  BasicUtilities::triMeshGeoToMatrices(meshIn, V, F);

  ES::MXd N;
  igl::per_vertex_normals(V, F, igl::PER_VERTEX_NORMALS_WEIGHTING_TYPE_ANGLE, N);

  normals.clear();
  for (Eigen::Index vi = 0; vi < N.rows(); vi++) {
    normals.emplace_back(N(vi, 0), N(vi, 1), N(vi, 2));
  }
}

void MedialAxisRepresentation::LibIGLInterface::meanCuravtures(const TriMeshGeo &meshIn, VegaFEM::EigenSupport::VXd &h)
{
  ES::MXd V;
  ES::MXi F;
  BasicUtilities::triMeshGeoToMatrices(meshIn, V, F);

  ES::MXd HN;
  Eigen::SparseMatrix<double> L, M, Minv;
  igl::cotmatrix(V, F, L);
  igl::massmatrix(V, F, igl::MASSMATRIX_TYPE_VORONOI, M);
  igl::invert_diag(M, Minv);
  HN = -Minv * (L * V);
  h = HN.rowwise().norm();  // up to sign
}

void MedialAxisRepresentation::LibIGLInterface::bbwWeights(
  const VegaFEM::EigenSupport::MXd &vtx, const VegaFEM::EigenSupport::MXi &tets,
  const VegaFEM::EigenSupport::MXd &vtxHandles,
  VegaFEM::EigenSupport::MXd &weights)
{
  const ES::MXd &V = vtx;
  const ES::MXi &T = tets;

  // List of boundary indices (aka fixed value indices into VV)
  ES::VXi b;

  // List of boundary conditions of each weight function
  ES::MXd bc;

  ES::VXi P(vtxHandles.rows());
  for (int vi = 0; vi < (int)vtxHandles.rows(); vi++) {
    P(vi) = vi;
  }

  igl::boundary_conditions(V, T, vtxHandles, P, ES::MXi(), ES::MXi(), b, bc);

  // compute BBW weights matrix
  igl::BBWData bbw_data;
  // only a few iterations for sake of demo
  bbw_data.active_set_params.max_iter = 100;
  bbw_data.verbosity = 2;
  bbw_data.partition_unity = true;

  if (!igl::bbw(V, T, b, bc, bbw_data, weights)) {
    std::cerr << "Cannot compute bbw weights!" << std::endl;
    return;
  }
}

#if defined(__GNUC__) || defined(__GNUG__)
#  pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#endif
