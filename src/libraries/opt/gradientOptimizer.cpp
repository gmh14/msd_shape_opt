#include "gradientOptimizer.h"
#include "linearElasticity.h"
#include "rfunc.h"
#include "libiglInterface.h"
#include "logger.h"

#include "basicAlgorithms.h"
#include "createTriMesh.h"
#include "predicates.h"

#include <unsupported/Eigen/SparseExtra>

#include <tbb/parallel_for.h>
#include <tbb/enumerable_thread_specific.h>

#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/intersections.h>

#include <fmt/format.h>

#include <numeric>

namespace ES = VegaFEM::EigenSupport;

namespace MedialAxisRepresentation
{
double eps_funcH = 1e-2;
double theta_min = 1e-2;
double theta_scale = 1.0 - theta_min;

double funcH(double v)
{
  // a (-e)3 + b (-e)2 + c(-e) + d = 0
  // a e3 + b e2 + c e + d = 1
  // 3 a (-e)2 + 2b (-e) + c = 0
  // 3 a e2 + 2b e + c = 0

  if (v < -eps_funcH) {
    return 0;
  }
  else if (v > eps_funcH) {
    return 1;
  }
  else {
    double a = -1.0 / (eps_funcH * eps_funcH * eps_funcH * 4.0);
    double b = 0.0;
    double c = 3.0 / (4 * eps_funcH);
    double d = 0.5;

    return a * v * v * v + b * v * v + c * v + d;
  }
}

double gradH(double v)
{
  if (v < -eps_funcH) {
    return 0;
  }
  else if (v > eps_funcH) {
    return 0;
  }
  else {
    double a = -1.0 / (eps_funcH * eps_funcH * eps_funcH * 4.0);
    double b = 0.0;
    double c = 3.0 / (4 * eps_funcH);
    double d = 0.5;

    return 3 * a * v * v + 2 * b * v + c;
  }
}

void computeTheta(const std::vector<VegaFEM::EigenSupport::V3d> &gridVertices, const std::vector<std::array<int, 8>> voxels,
  ES::VXd &reducedTheta, int nSphereVtx, int nCylinderVtx,
  const std::vector<std::shared_ptr<VertexRFunction>> &vtxR,
  const std::vector<std::shared_ptr<EdgeRFunction>> &edgeR,
  std::shared_ptr<const RFunction> rFunc, ES::VXd &theta)
{
  int cOffset = (nSphereVtx + 6) * (int)vtxR.size();

  tbb::enumerable_thread_specific<ES::VXd> reducedTheta_tls(reducedTheta);

  // for (int ei = 0; ei < (int)voxels.size(); ei++) {
  tbb::parallel_for(0, (int)voxels.size(), [&](int ei) {
    ES::VXd &reducedThetaCopy = reducedTheta_tls.local();
    ES::V3d center(0, 0, 0);
    for (int j = 0; j < 8; j++) {
      center += gridVertices[voxels[ei][j]];
    }
    center /= 8.0;

    for (int vi = 0; vi < (int)vtxR.size(); vi++) {
      reducedThetaCopy.segment<3>(vi * (nSphereVtx + 6)) = center;
    }

    for (int ei = 0; ei < (int)edgeR.size(); ei++) {
      reducedThetaCopy.segment<3>(cOffset + ei * (nCylinderVtx + 9)) = center;
    }

    double rval = rFunc->func(reducedThetaCopy.data());
    theta[ei] = funcH(rval) * theta_scale + theta_min;
  });
}

void compute_dr(const std::vector<VegaFEM::EigenSupport::V3d> &gridVertices, const std::vector<std::array<int, 8>> voxels,
  ES::VXd &reducedTheta, int nSphereVtx, int nCylinderVtx,
  const std::vector<std::shared_ptr<VertexRFunction>> &vtxR,
  const std::vector<std::shared_ptr<EdgeRFunction>> &edgeR,
  std::shared_ptr<const RFunction> rFunc, const ES::VXd &dEdtheta, ES::VXd &dE_dr, ES::VXd &dtheta_dr_local, ES::VXd &theta)
{
  int cOffset = (nSphereVtx + 6) * (int)vtxR.size();
  dE_dr.setZero();

  tbb::enumerable_thread_specific<ES::VXd> dE_dr_tls(ES::VXd::Zero(dE_dr.size()));
  tbb::enumerable_thread_specific<ES::VXd> reducedTheta_tls(reducedTheta);

  // for (int ei = 0; ei < (int)voxels.size(); ei++) {
  tbb::parallel_for(0, (int)voxels.size(), [&](int ei) {
    ES::VXd &reducedThetaCopy = reducedTheta_tls.local();
    thread_local ES::VXd dtheta_dr_local_copy;

    if (dtheta_dr_local_copy.size() == 0) {
      dtheta_dr_local_copy.resize(reducedThetaCopy.size());
    }

    ES::V3d center(0, 0, 0);
    for (int j = 0; j < 8; j++) {
      center += gridVertices[voxels[ei][j]];
    }
    center /= 8.0;

    for (int vi = 0; vi < (int)vtxR.size(); vi++) {
      reducedThetaCopy.segment<3>(vi * (nSphereVtx + 6)) = center;
    }

    for (int ei = 0; ei < (int)edgeR.size(); ei++) {
      reducedThetaCopy.segment<3>(cOffset + ei * (nCylinderVtx + 9)) = center;
    }

    double f = rFunc->func(reducedThetaCopy.data());
    double H = funcH(f);
    double theta_local = H * theta_scale + theta_min;

    // dtheta/dr = dtheta/dH dH/df df/dr
    rFunc->gradient(reducedThetaCopy.data(), dtheta_dr_local_copy.data());
    if (dtheta_dr_local_copy.hasNaN()) {
      std::cout << "has nan." << std::endl;
    }

    dtheta_dr_local_copy *= gradH(f) * theta_scale;

    auto &buf = dE_dr_tls.local();
    tbb::this_task_arena::isolate([&] {
      buf += dtheta_dr_local_copy * dEdtheta[ei];
    });

    theta[ei] = theta_local;
  });

  dE_dr.setZero();
  for (auto it = dE_dr_tls.begin(); it != dE_dr_tls.end(); ++it) {
    dE_dr += *it;
  }
}

void staticSim(const std::vector<VegaFEM::EigenSupport::V3d> &gridVertices, const std::vector<std::array<int, 8>> voxels,
  const std::vector<int> &fixedDOFs, const ES::VXd &fextIn, const ES::VXd &theta,
  const ES::SpMatI &Kmap, const std::vector<int> &b2s, const std::vector<int> &s2b,
  ES::SpMatD &K, ES::SpMatD &K11, ES::SymSolver &solver, ES::VXd &u)
{
  computeK(gridVertices, voxels, theta, K);

  int n3 = (int)gridVertices.size() * 3;

#if 0
  std::vector<int> gatheredDOFs;
  gatheredDOFs.reserve(gridVertices.size() * 3);
  for (int ei = 0; ei < (int)voxels.size(); ei++) {
    if (theta[ei] > 0.1) {
      for (int j = 0; j < 8; j++) {
        for (int dofi = 0; dofi < 3; dofi++) {
          gatheredDOFs.emplace_back(voxels[ei][j] * 3 + dofi);
        }
      }
    }
  }

  sortAndDeduplicateWithErase(gatheredDOFs);

  std::vector<int> gatheredDOFsAfterRemovalFixed;
  gatheredDOFsAfterRemovalFixed.reserve(gridVertices.size() * 3);
  std::set_difference(gatheredDOFs.begin(), gatheredDOFs.end(), fixedDOFs.begin(), fixedDOFs.end(),
    std::back_inserter(gatheredDOFsAfterRemovalFixed));

  std::vector<int> allDOFs(gridVertices.size() * 3);
  std::iota(allDOFs.begin(), allDOFs.end(), 0);

  std::vector<int> removedDOFs;
  removedDOFs.reserve(gridVertices.size() * 3);
  std::set_difference(allDOFs.begin(), allDOFs.end(), gatheredDOFsAfterRemovalFixed.begin(), gatheredDOFsAfterRemovalFixed.end(),
    std::back_inserter(removedDOFs));

  ES::SpMatD K11;
  ES::RemoveRowsCols(K, removedDOFs, K11);

  std::vector<int> b2s, s2b;
  ES::RemoveRows(n3, removedDOFs, b2s, s2b);

  ES::VXd fext1(n3 - removedDOFs.size());
  for (int i = 0; i < (int)fextIn.size(); i++) {
    if (b2s[i] < 0)
      continue;

    fext1[b2s[i]] = fextIn[i];
  }

  ES::SymSolver Ksolver;
  Ksolver.analyzePattern(K11);
  Ksolver.factorize(K11);
  u = Ksolver.solve(fext1);
#else
  ES::VXd fext1(n3 - fixedDOFs.size());
  for (int i = 0; i < (int)fextIn.size(); i++) {
    if (b2s[i] < 0)
      continue;

    fext1[b2s[i]] = fextIn[i];
  }

  ES::TransferBigToSmall(K, K11, Kmap, 1);
  // [msd_shape_opt] disabled debug dump: it wrote the whole stiffness matrix (~100 MB of text) at every solve
  // Eigen::saveMarket(K11, "K11.mat");

  solver.factorize(K11);
  ES::VXd u1 = solver.solve(fext1);

  u.setZero();
  for (int i = 0; i < (int)u1.size(); i++) {
    u[s2b[i]] = u1[i];
  }

#endif
}

void createVoxelMesh(const std::vector<VegaFEM::EigenSupport::V3d> &gridVertices, const std::vector<std::array<int, 8>> voxels,
  const ES::VXd &theta, const char *filename)
{
  auto writeBoxMesh = [](const Vec3d &bmin, const Vec3d &bmax, int &baseVID, std::ofstream &ofile) {
    Vec3d positions[] = {
      bmin,
      {bmax[0], bmin[1], bmin[2]},
      {bmax[0], bmax[1], bmin[2]},
      {bmin[0], bmax[1], bmin[2]},
      {bmin[0], bmin[1], bmax[2]},
      {bmax[0], bmin[1], bmax[2]},
      bmax,
      {bmin[0], bmax[1], bmax[2]}
    };

    Vec4i faces[] = {
      {0, 3, 2, 1},
      {4, 5, 6, 7},
      {0, 1, 5, 4},
      {3, 7, 6, 2},
      {1, 2, 6, 5},
      {0, 4, 7, 3}
    };

    for (int i = 0; i < 8; i++) {
      ofile << "v " << positions[i][0] << ' ' << positions[i][1] << ' ' << positions[i][2] << '\n';
      ;
    }

    for (int i = 0; i < 6; i++) {
      ofile << "f " << (faces[i][0] + 1 + baseVID) << ' ' << (faces[i][1] + 1 + baseVID) << ' ' << (faces[i][2] + 1 + baseVID) << ' ' << (faces[i][3] + 1 + baseVID) << '\n';
    }
    baseVID += 8;
  };

  std::ofstream ofile(filename);
  int baseVID = 0;
  for (int ei = 0; ei < (int)voxels.size(); ei++) {
    if (theta[ei] > 0.5) {
      writeBoxMesh(Vec3d(gridVertices[voxels[ei][0]].data()), Vec3d(gridVertices[voxels[ei][7]].data()), baseVID, ofile);
    }
  }

  ofile.close();
}

}  // namespace MedialAxisRepresentation

void MedialAxisRepresentation::optimizeContinuous(
  const std::vector<VegaFEM::EigenSupport::V3d> &gridVertices, const std::vector<std::array<int, 8>> voxels,
  const std::vector<VegaFEM::EigenSupport::V3d> &vertices, const std::vector<std::array<int, 2>> &edges,
  const VegaFEM::EigenSupport::VXd &fextIn, const std::vector<int> &fixedVertices, double v0, int method,
  const OptimizeContinuousOptions &options)
{
  // argmin E(u1, theta) = 1/2 u1T K11 u1 + 1/2 w (S theta - v0)^2
  // st. K11 u1 = f1

  // K11 du1/dtheta + dK11/dtheta u1 = 0
  // du1/dtheta = - K11^-1 dK11/dtheta

  // dE/dtheta = dE/du du/dtheta + dE/theta
  //           = (K11 u1)^T (-K11^-1 dK11/dtheta) + 1/2 u1T dK11/dtheta u1 + w ST (S theta - v0)
  //           = -u1T dK11/dtheta u1 + 1/2 u1T dK11/dtheta u1 + w (S theta - v0)^T S
  //           = -1/2 u1T dK11/dtheta u1  + w (S theta - v0)^T S

  double defaultR = options.initialRadius;  // [msd_shape_opt] originally hard-coded to 0.04 (= the default option)
  double cellSize = (gridVertices[voxels[0][0]] - gridVertices[voxels[0][1]]).norm();
  double totalCount = (double)voxels.size();
  double w = cellSize * cellSize * cellSize * 10;

  int n = (int)gridVertices.size();
  int n3 = n * 3;
  int nele = (int)voxels.size();

  ES::V3d bmin(1e100, 1e100, 1e100);
  ES::V3d bmax(-1e100, -1e100, -1e100);
  for (int i = 0; i < (int)gridVertices.size(); i++) {
    bmin = bmin.cwiseMin(gridVertices[i]);
    bmax = bmax.cwiseMax(gridVertices[i]);
  }
  ES::V3d bbcenter = (bmin + bmax) * 0.5;
  bmin = (bmin - bbcenter) * 0.999 + bbcenter;
  bmax = (bmax - bbcenter) * 0.999 + bbcenter;

  ES::VXd ucur(n3);
  ucur.setZero();

  ES::VXd theta(nele);
  theta.setOnes(nele);

  // build R func
  std::vector<std::shared_ptr<VertexRFunction>> vtxR;
  std::vector<std::shared_ptr<EdgeRFunction>> edgeR;
  // std::vector<std::shared_ptr<UnionRFunction>> unionR;

  TriMeshGeo templateSphere, templateCylinder;
  if (LibIGLInterface::loadObjMesh("s.obj", templateSphere) != 0) {
    exit(1);
  }

  for (int vi = 0; vi < templateSphere.numVertices(); vi++) {
    templateSphere.pos(vi) /= len(templateSphere.pos(vi));
  }

  if (LibIGLInterface::loadObjMesh("c1.obj", templateCylinder) != 0) {
    exit(1);
  }

  for (int i = 0; i < (int)vertices.size(); i++) {
    vtxR.emplace_back(std::make_shared<VertexRFunction>(templateSphere));
  }

  for (int i = 0; i < (int)edges.size(); i++) {
    edgeR.emplace_back(std::make_shared<EdgeRFunction>(templateCylinder));
  }

  std::vector<std::shared_ptr<RFunction>> primitiveFunc;
  for (auto v : vtxR)
    primitiveFunc.emplace_back(v);

  for (auto e : edgeR)
    primitiveFunc.emplace_back(e);

  int start = 0;
  int end = (int)primitiveFunc.size();

  while (start + 1 < end) {
    for (int i = start; i < end; i += 2) {
      if (i < end && i + 1 < end) {
        primitiveFunc.emplace_back(std::make_shared<UnionRFunction>(primitiveFunc[i], primitiveFunc[i + 1]));
      }
      else {
        primitiveFunc.emplace_back(primitiveFunc[i]);
      }
    }
    start = end;
    end = (int)primitiveFunc.size();
  }

  // unionR.emplace_back(std::make_shared<UnionRFunction>(vtxR[0], vtxR[1]));
  // for (int vi = 2; vi < (int)vertices.size(); vi++) {
  //   unionR.emplace_back(std::make_shared<UnionRFunction>(unionR.back(), vtxR[vi]));
  // }

  // for (int ei = 0; ei < (int)edges.size(); ei++) {
  //   unionR.emplace_back(std::make_shared<UnionRFunction>(unionR.back(), edgeR[ei]));
  // }

  int nSphereDOFs = (int)vtxR.size() * (templateSphere.numVertices() + 6);
  int nCylinderDOFs = (int)edgeR.size() * (templateCylinder.numVertices() + 9);
  ES::VXd reducedTheta(nSphereDOFs + nCylinderDOFs);
  reducedTheta.setZero();

  for (int vi = 0; vi < (int)vtxR.size(); vi++) {
    reducedTheta.segment<3>(vi * (templateSphere.numVertices() + 6) + 3) = vertices[vi];
    for (int k = 0; k < templateSphere.numVertices(); k++) {
      reducedTheta(vi * (templateSphere.numVertices() + 6) + 6 + k) = defaultR;
    }
  }

  for (int ei = 0; ei < (int)edgeR.size(); ei++) {
    reducedTheta.segment<3>(nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 3) = vertices[edges[ei][0]];
    reducedTheta.segment<3>(nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 6) = vertices[edges[ei][1]];
    for (int k = 0; k < templateCylinder.numVertices(); k++) {
      reducedTheta(nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 9 + k) = defaultR;
    }
  }

  computeTheta(gridVertices, voxels, reducedTheta, templateSphere.numVertices(), templateCylinder.numVertices(),
    vtxR, edgeR, primitiveFunc.back(), theta);

  createVoxelMesh(gridVertices, voxels, theta, "initial-sample.obj");

  // compute K
  std::vector<ES::M24d> elementK(voxels.size());
  ES::SpMatD KAll;
  // for (int ei = 0; ei < (int)voxels.size(); ei++) {
  tbb::parallel_for(0, (int)voxels.size(), [&](int ei) {
    computeElementK(gridVertices, voxels, ei, elementK[ei]);
    if (elementK[ei].hasNaN()) {
      exit(1);
    }
  });

  SPDLOG_LOGGER_INFO(Logger::lgr(), "done ele K computation.");

  KAll = computeK(gridVertices, voxels, theta);

  SPDLOG_LOGGER_INFO(Logger::lgr(), "done sp K computation.");

  std::vector<int> fixedDOFs;
  for (int vid : fixedVertices) {
    fixedDOFs.emplace_back(vid * 3);
    fixedDOFs.emplace_back(vid * 3 + 1);
    fixedDOFs.emplace_back(vid * 3 + 2);
  }

  ES::SpMatD K11;
  ES::SpMatI Kmap;
  ES::RemoveRowsCols(KAll, fixedDOFs, K11);
  ES::RemoveRowsCols(KAll, K11, fixedDOFs, Kmap);

  std::vector<int> b2s, s2b;
  ES::RemoveRows(n3, fixedDOFs, b2s, s2b);

  ES::SymSolver Ksolver;
  Ksolver.analyzePattern(K11);
  Ksolver.factorize(K11);

  staticSim(gridVertices, voxels, fixedDOFs, fextIn, theta,
    Kmap, b2s, s2b, KAll, K11, Ksolver, ucur);

  SPDLOG_LOGGER_INFO(Logger::lgr(), "done sim init.");

  // compute S
  ES::SpMatD S(1, nele);
  std::vector<ES::TripletD> entries;
  for (int j = 0; j < nele; j++) {
    entries.emplace_back(0, j, 1.0);
  }
  S.setFromTriplets(entries.begin(), entries.end());

  entries.clear();

  for (int vi = 0; vi < (int)vertices.size(); vi++) {
    int posRowOffset = vi * (templateSphere.numVertices() + 6) + 3;
    int posColOffset = vi * 3;

    for (int r = 0; r < 3; r++) {
      entries.emplace_back(posRowOffset + r, posColOffset + r, 1.0);
    }

    int radiusRowOffset = vi * (templateSphere.numVertices() + 6) + 6;
    int radiusColOffset = (int)vertices.size() * 3 + vi * templateSphere.numVertices();

    for (int r = 0; r < templateSphere.numVertices(); r++) {
      entries.emplace_back(radiusRowOffset + r, radiusColOffset + r, 1.0);
    }
  }

  for (int ei = 0; ei < (int)edges.size(); ei++) {
    int pos0RowOffset = nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 3;
    int pos0ColOffset = edges[ei][0] * 3;

    for (int r = 0; r < 3; r++) {
      entries.emplace_back(pos0RowOffset + r, pos0ColOffset + r, 1.0);
    }

    int pos1RowOffset = nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 6;
    int pos1ColOffset = edges[ei][1] * 3;

    for (int r = 0; r < 3; r++) {
      entries.emplace_back(pos1RowOffset + r, pos1ColOffset + r, 1.0);
    }

    int radiusRowOffset = nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 9;
    int radiusColOffset = (int)vertices.size() * 3 + (int)vertices.size() * templateSphere.numVertices() + ei * templateCylinder.numVertices();

    for (int r = 0; r < templateCylinder.numVertices(); r++) {
      entries.emplace_back(radiusRowOffset + r, radiusColOffset + r, 1.0);
    }
  }
  ES::SpMatD assembleM(primitiveFunc.back()->getNumDOFs(), (int)vertices.size() * 3 + (int)vertices.size() * templateSphere.numVertices() + (int)edges.size() * templateCylinder.numVertices());
  assembleM.setFromTriplets(entries.begin(), entries.end());

  // another subspace
  int useOneMoreSubspace = 1;

  entries.clear();
  ES::MXd UCylinder(templateCylinder.numVertices(), 2);
  ALOG((int)UCylinder.rows() == 14);

  UCylinder(0, 0) = 1, UCylinder(0, 1) = 0;
  UCylinder(1, 0) = 1, UCylinder(1, 1) = 0;
  UCylinder(2, 0) = 1, UCylinder(2, 1) = 0;
  UCylinder(3, 0) = 1, UCylinder(3, 1) = 0;
  UCylinder(4, 0) = 0.5, UCylinder(4, 1) = 0.5;
  UCylinder(5, 0) = 0.5, UCylinder(5, 1) = 0.5;
  UCylinder(6, 0) = 0.5, UCylinder(6, 1) = 0.5;
  UCylinder(7, 0) = 0.5, UCylinder(7, 1) = 0.5;
  UCylinder(8, 0) = 0, UCylinder(8, 1) = 1;
  UCylinder(9, 0) = 0, UCylinder(9, 1) = 1;
  UCylinder(10, 0) = 0, UCylinder(10, 1) = 1;
  UCylinder(11, 0) = 0, UCylinder(11, 1) = 1;
  UCylinder(12, 0) = 1, UCylinder(12, 1) = 0;
  UCylinder(13, 0) = 0, UCylinder(13, 1) = 1;

  for (int vi = 0; vi < (int)vertices.size(); vi++) {
    int posRowOffset = vi * 3;
    int posColOffset = vi * 3;

    for (int r = 0; r < 3; r++) {
      entries.emplace_back(posRowOffset + r, posColOffset + r, 1.0);
    }

    int radiusRowOffset = (int)vertices.size() * 3 + vi * templateSphere.numVertices();
    int radiusColOffset = (int)vertices.size() * 3 + vi;

    for (int r = 0; r < templateSphere.numVertices(); r++) {
      entries.emplace_back(radiusRowOffset + r, radiusColOffset, 1.0);
    }
  }

  for (int ei = 0; ei < (int)edges.size(); ei++) {
    int radiusColOffset = (int)vertices.size() * 3 + (int)vertices.size() + ei * 2;
    int radiusRowOffset = (int)vertices.size() * 3 + (int)vertices.size() * templateSphere.numVertices() + ei * templateCylinder.numVertices();

    for (int r = 0; r < templateCylinder.numVertices(); r++) {
      for (int c = 0; c < 2; c++) {
        entries.emplace_back(radiusRowOffset + r, radiusColOffset + c, UCylinder(r, c));
      }
    }
  }
  ES::SpMatD assembleM1((int)vertices.size() * 3 + (int)vertices.size() * templateSphere.numVertices() + (int)edges.size() * templateCylinder.numVertices(),
    (int)vertices.size() * 3 + (int)vertices.size() + (int)edges.size() * 2);
  assembleM1.setFromTriplets(entries.begin(), entries.end());

  SPDLOG_LOGGER_INFO(Logger::lgr(), "done init all.");

  // Eigen::saveMarket(assembleM, "m.mat");

  std::ofstream oEnergyFile("energy.txt");

  // [msd_shape_opt] (additive) bookkeeping used only to report the progress to the caller
  double lastCompliance = 0, lastVolumePenalty = 0, lastVolumeFraction = 0;
  double lastGradNorm = 0, lastAlpha = 0, lastMaxChange = 0;
  int lastLineSearchIters = 0;
  auto reportIteration = [&](int iter) {
    if (!options.iterationCallback)
      return;

    OptimizeContinuousIterationInfo info;
    info.iteration = iter;
    info.theta = &theta;
    info.reducedTheta = &reducedTheta;
    info.numSphereTemplateVertices = templateSphere.numVertices();
    info.numCylinderTemplateVertices = templateCylinder.numVertices();
    info.compliance = lastCompliance;
    info.volumePenalty = lastVolumePenalty;
    info.volumeFraction = lastVolumeFraction;
    info.energy = lastCompliance + lastVolumePenalty;
    info.gradientNorm = lastGradNorm;
    info.stepSize = lastAlpha;
    info.maxChange = lastMaxChange;
    info.lineSearchIterations = lastLineSearchIters;
    options.iterationCallback(info);
  };

  // opt
  for (int giter = 0; giter < options.maxIterations; giter++) {  // [msd_shape_opt] originally: giter < 200
    std::cout << "Iter: " << giter << std::endl;
    // compute gradient
    ES::VXd dEdtheta(nele);
    for (int ei = 0; ei < nele; ei++) {
      Eigen::Matrix<double, 24, 1> ulocal;
      for (int j = 0; j < 8; j++) {
        if (std::binary_search(fixedVertices.begin(), fixedVertices.end(), voxels[ei][j])) {
          ulocal.segment<3>(j * 3).setZero();
        }
        else {
          ulocal.segment<3>(j * 3) = ucur.segment<3>(voxels[ei][j] * 3);
        }
      }

      dEdtheta[ei] = (elementK[ei] * ulocal).dot(ulocal);
    }
    dEdtheta *= -0.5;

    double t0 = std::max((theta.sum() - theta_min * totalCount) / theta_scale - v0 * totalCount, 0.0);
    dEdtheta += ES::VXd::Ones(nele) * w * t0 / theta_scale;

    ES::VXd dE_dr(reducedTheta.size());
    dE_dr.setZero();

    ES::VXd dtheta_dr_local(reducedTheta.size());
    compute_dr(gridVertices, voxels, reducedTheta, templateSphere.numVertices(), templateCylinder.numVertices(),
      vtxR, edgeR, primitiveFunc.back(), dEdtheta, dE_dr, dtheta_dr_local, theta);

    auto computeE = [&](int writeFile = 0) {
      computeTheta(gridVertices, voxels, reducedTheta,
        templateSphere.numVertices(), templateCylinder.numVertices(),
        vtxR, edgeR, primitiveFunc.back(), theta);

      staticSim(gridVertices, voxels, fixedDOFs, fextIn, theta,
        Kmap, b2s, s2b, KAll, K11, Ksolver, ucur);

      ES::VXd Ku(ucur.size());
      ES::Mv(KAll, ucur, Ku);
      double fillCount = (theta.sum() - theta_min * totalCount) / theta_scale;
      double vdiff = std::max(fillCount - v0 * totalCount, 0.0);
      double term1 = 0.5 * Ku.dot(ucur);
      double term2 = w * vdiff * vdiff * 0.5;
      std::cout << "       d: " << fillCount / totalCount;
      std::cout << "       E: " << term1 << ',' << term2 << std::endl;

      oEnergyFile << fillCount / totalCount << ',' << term1 << ',' << term2 << ',' << (term1 + term2) << std::endl;

      lastCompliance = term1, lastVolumePenalty = term2, lastVolumeFraction = fillCount / totalCount;  // [msd_shape_opt]

      return term1 + term2;
    };

    if (giter == 0) {
      computeE(1);
      reportIteration(0);  // [msd_shape_opt]
    }

    if (0) {
      // filter positions
      for (int vi = 0; vi < vtxR.size(); vi++) {
        dE_dr.segment<3>(vi * (templateSphere.numVertices() + 6)).setZero();
        dE_dr.segment<3>(vi * (templateSphere.numVertices() + 6) + 3).setZero();
      }

      for (int ei = 0; ei < (int)edgeR.size(); ei++) {
        dE_dr.segment<3>(nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 0).setZero();
        dE_dr.segment<3>(nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 3).setZero();
        dE_dr.segment<3>(nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 6).setZero();
      }

      std::cout << "||grad||=" << dE_dr.norm() << "; ||grad||_max=" << dE_dr.cwiseAbs().maxCoeff() << std::endl;

      std::vector<std::pair<double, int>> entryLists;
      for (int vi = 0; vi < dE_dr.size(); vi++) {
        entryLists.emplace_back(std::abs(dE_dr[vi]), vi);
      }
      std::sort(entryLists.begin(), entryLists.end());

      double E0 = computeE();
      ES::VXd rCopy = reducedTheta;
      double min_alpha = 1;
      for (int dofi = 0; dofi < (int)dE_dr.size(); dofi++) {
        if (dE_dr[dofi] > 1e-6 && rCopy[dofi] > 1e-6) {
          min_alpha = std::min(min_alpha, rCopy[dofi] / dE_dr[dofi]);
        }
      }

      double alpha = min_alpha * 0.9;
      std::cout << "max alpha: " << alpha << std::endl;

      for (int liter = 0; liter < 15; liter++) {
        reducedTheta = rCopy - alpha * dE_dr;
        double E1 = computeE();

        std::cout << "    ls " << liter << ": E=" << E1 << "; E0=" << E0 << "; alpha=" << alpha << std::endl;

        if (E1 > E0) {
          alpha *= 0.5;
        }
        else {
          break;
        }
      }

      reducedTheta = rCopy - dE_dr * alpha;
      E0 = computeE();

      double maxChange = alpha * dE_dr.cwiseAbs().maxCoeff();
      if (maxChange < 1e-6) {
        break;
      }
    }
    else if (method) {
      ES::VXd dE_dr_clean = assembleM.transpose() * dE_dr;
      ES::VXd dE_dq = assembleM1.transpose() * dE_dr_clean;
      std::cout << "||grad||=" << dE_dq.norm() << "; ||grad||_max=" << dE_dq.cwiseAbs().maxCoeff() << std::endl;
      lastGradNorm = dE_dq.norm();  // [msd_shape_opt]

      double E0 = computeE();
      ES::VXd rInitialClean = assembleM.transpose() * reducedTheta;
      for (int vi = 0; vi < (int)vertices.size(); vi++) {
        int posRowOffset = vi * (templateSphere.numVertices() + 6) + 3;
        int posColOffset = vi * 3;

        rInitialClean.segment<3>(posColOffset) = reducedTheta.segment<3>(posRowOffset);
      }

      ES::VXd qInitialClean = assembleM1.transpose() * rInitialClean;
      for (int vi = 0; vi < (int)vertices.size(); vi++) {
        int radiusRowOffset = (int)vertices.size() * 3 + vi * templateSphere.numVertices();
        int radiusColOffset = (int)vertices.size() * 3 + vi;

        qInitialClean(radiusColOffset) = rInitialClean(radiusRowOffset);
      }

      for (int ei = 0; ei < (int)edges.size(); ei++) {
        int radiusColOffset = (int)vertices.size() * 3 + (int)vertices.size() + ei * 2;
        int radiusRowOffset = (int)vertices.size() * 3 + (int)vertices.size() * templateSphere.numVertices() + ei * templateCylinder.numVertices();

        qInitialClean(radiusColOffset) = rInitialClean(radiusRowOffset);
        qInitialClean(radiusColOffset + 1) = rInitialClean(radiusRowOffset + 8);
      }

      double min_alpha = 1;
      for (int dofi = (int)vertices.size() * 3; dofi < (int)dE_dq.size(); dofi++) {
        if (dE_dq[dofi] > 1e-6 && qInitialClean[dofi] > 1e-6) {
          min_alpha = std::min(min_alpha, qInitialClean[dofi] / dE_dq[dofi]);
        }
      }

      // dE_dr_clean.segment<3>(0).setZero();
      // dE_dr_clean.segment<3>(3 * 3).setZero();
      // dE_dr_clean.segment<3>(2 * 3).setZero();

      // dE_dr_clean.head(vertices.size() * 3).setZero();

      double min_alpha_pos = 1.0;
      min_alpha_pos = 1.0;
      for (int dofi = 0; dofi < (int)vertices.size() * 3; dofi++) {
        if (std::abs(dE_dq[dofi]) > 1e-6) {
          min_alpha_pos = std::min(min_alpha_pos, cellSize / std::abs(dE_dq[dofi]) * 4);
        }
      }

      std::cout << min_alpha << ',' << min_alpha_pos << std::endl;

      double alpha = std::min(min_alpha * 0.75, min_alpha_pos);
      // double alpha = min_alpha * 0.75;
      std::cout << "max alpha: " << alpha << std::endl;

      for (int liter = 0; liter < 15; liter++) {
        lastLineSearchIters = liter + 1;  // [msd_shape_opt]
        ES::VXd qClean = qInitialClean - alpha * dE_dq;
        ES::VXd rClean = assembleM1 * qClean;
        reducedTheta = assembleM * rClean;

        double E1 = computeE();

        std::cout << "    ls " << liter << ": E=" << E1 << "; E0=" << E0 << "; alpha=" << alpha << std::endl;

        if (E1 > E0) {
          alpha *= 0.5;
        }
        else {
          break;
        }
      }

      ES::VXd qClean = qInitialClean - alpha * dE_dq;
      ES::VXd rClean = assembleM1 * qClean;
      reducedTheta = assembleM * rClean;

      TriMeshGeo skMesh;
      for (int ei = 0; ei < (int)edges.size(); ei++) {
        ES::V3d p0 = reducedTheta.segment<3>(nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 3);
        ES::V3d p1 = reducedTheta.segment<3>(nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 6);
        skMesh.addMesh(createSingleTriangleMesh(Vec3d(p0.data()), Vec3d(p1.data()), Vec3d(p0.data()) + Vec3d(1e-6)));
      }
      skMesh.save(fmt::format("iter{:04d}-sk.obj", giter));

      E0 = computeE(1);

      createVoxelMesh(gridVertices, voxels, theta, fmt::format("iter{:04d}-sample.obj", giter).c_str());

      double maxChange = alpha * dE_dr.cwiseAbs().maxCoeff();
      std::cout << "max change: " << maxChange << std::endl;

      lastAlpha = alpha, lastMaxChange = maxChange;  // [msd_shape_opt]
      reportIteration(giter + 1);                    // [msd_shape_opt]

      if (maxChange < 1e-5) {
        break;
      }
    }
    else {
      ES::VXd dE_dr_clean = assembleM.transpose() * dE_dr;
      std::cout << "r ||grad||=" << dE_dr_clean.norm() << "; ||grad||_max=" << dE_dr_clean.cwiseAbs().maxCoeff() << std::endl;
      lastGradNorm = dE_dr_clean.norm();  // [msd_shape_opt]

      // std::vector<std::pair<double, int>> entryLists;
      // for (int vi = 0; vi < dE_dr_clean.size(); vi++) {
      //   entryLists.emplace_back(std::abs(dE_dr_clean[vi]), vi);
      // }
      // std::sort(entryLists.begin(), entryLists.end());

      double E0 = computeE();
      ES::VXd rInitialClean = assembleM.transpose() * reducedTheta;
      for (int vi = 0; vi < (int)vertices.size(); vi++) {
        int posRowOffset = vi * (templateSphere.numVertices() + 6) + 3;
        int posColOffset = vi * 3;

        rInitialClean.segment<3>(posColOffset) = reducedTheta.segment<3>(posRowOffset);
      }

      double min_alpha = 1;
      for (int dofi = (int)vertices.size() * 3; dofi < (int)dE_dr_clean.size(); dofi++) {
        if (dE_dr_clean[dofi] > 1e-6 && rInitialClean[dofi] > 1e-6) {
          min_alpha = std::min(min_alpha, rInitialClean[dofi] / dE_dr_clean[dofi]);
        }
      }

      // dE_dr_clean.segment<3>(0).setZero();
      // dE_dr_clean.segment<3>(3 * 3).setZero();
      // dE_dr_clean.segment<3>(2 * 3).setZero();

      // dE_dr_clean.head(vertices.size() * 3).setZero();

      double min_alpha_pos = 1.0;
#if 0
      using IK = CGAL::Exact_predicates_inexact_constructions_kernel;
      IK::Iso_cuboid_3 bb(IK::Point_3(bmin[0], bmin[1], bmin[2]), IK::Point_3(bmax[0], bmax[1], bmax[2]));

      for (int vi = 0; vi < (int)vertices.size(); vi++) {
        ES::V3d dir = -dE_dr_clean.segment<3>(vi * 3);
        if (dir.norm() > 1e-5) {
          dir.normalize();

          IK::Point_3 p0(rInitialClean[vi * 3], rInitialClean[vi * 3 + 1], rInitialClean[vi * 3 + 2]);
          IK::Vector_3 d(dir[0], dir[1], dir[2]);
          IK::Ray_3 ray(p0, d);

          auto ret = CGAL::intersection(ray, bb);
          ES::V3d maxAllowedDiff;
          ES::V3d grad_d = -dE_dr_clean.segment<3>(vi * 3);
          if (ret) {
            if (IK::Point_3 *p = boost::get<IK::Point_3>(&*ret)) {
              ES::V3d p1((*p)[0], (*p)[1], (*p)[2]);
              maxAllowedDiff = p1 - rInitialClean.segment<3>(vi * 3);
            }
            else if (IK::Segment_3 *s = boost::get<IK::Segment_3>(&*ret)) {
              IK::Vector_3 diff1 = s->vertex(1) - p0;
              IK::Vector_3 diff2 = s->vertex(0) - p0;

              if (diff1.squared_length() < diff2.squared_length()) {
                std::cout << diff1.squared_length() << std::endl;
                maxAllowedDiff = ES::V3d(diff2[0], diff2[1], diff2[2]);
              }
              else {
                std::cout << diff2.squared_length() << std::endl;
                maxAllowedDiff = ES::V3d(diff1[0], diff1[1], diff1[2]);
              }
            }
            else {
              throw std::runtime_error("Impossible");
            }

            for (int j = 0; j < 3; j++) {
              if (std::abs(d[j]) > 1e-6) {
                min_alpha_pos = std::min(min_alpha_pos, maxAllowedDiff[j] / grad_d[j]);
              }
            }
          }
          else {
          }
        }
      }
#endif

      double min_alpha_edge = 1.0;
      for (int ei = 0; ei < (int)edges.size(); ei++) {
        ES::V3d xi = rInitialClean.segment<3>(edges[ei][0] * 3);
        ES::V3d xj = rInitialClean.segment<3>(edges[ei][1] * 3);

        ES::V3d di = -dE_dr_clean.segment<3>(edges[ei][0] * 3);
        ES::V3d dj = -dE_dr_clean.segment<3>(edges[ei][1] * 3);

        ES::V3d xdiff = xi - xj;
        ES::V3d ddiff = di - dj;

        double a = ddiff.dot(ddiff);
        double b = ddiff.dot(xdiff) * 2.0;
        double c = xdiff.dot(xdiff) - cellSize * cellSize;

        double delta = b * b - 4 * a * c;
        if (delta < 0)
          continue;

        double root0 = (-b + std::sqrt(delta)) / (2 * a);
        double root1 = (-b - std::sqrt(delta)) / (2 * a);

        min_alpha_edge = std::min({ min_alpha_edge, root0, root1 });
      }

      min_alpha_pos = 1.0;
      for (int dofi = 0; dofi < (int)vertices.size() * 3; dofi++) {
        if (std::abs(dE_dr_clean[dofi]) > 1e-6) {
          min_alpha_pos = std::min(min_alpha_pos, cellSize / std::abs(dE_dr_clean[dofi]) * 4);
        }
      }

      std::cout << min_alpha << ',' << min_alpha_edge << ',' << min_alpha_pos << std::endl;

      double alpha = std::min(min_alpha * 0.75, min_alpha_pos);
      // double alpha = min_alpha * 0.75;
      std::cout << "max alpha: " << alpha << std::endl;

      for (int liter = 0; liter < 15; liter++) {
        lastLineSearchIters = liter + 1;  // [msd_shape_opt]
        ES::VXd rClean = rInitialClean - alpha * dE_dr_clean;
        reducedTheta = assembleM * rClean;
        //  ES::VXd reducedTheta0 = reducedTheta;

        // for (int vi = 0; vi < (int)vertices.size(); vi++) {
        //   ES::V3d pdiff =
        //     reducedTheta.segment<3>(vi * (templateSphere.numVertices() + 6) + 3) -
        //     reducedTheta0.segment<3>(vi * (templateSphere.numVertices() + 6) + 3);

        //  std::cout << vi << ": ";
        //  std::cout << pdiff.transpose() << std::endl;
        //}

        // for (int ei = 0; ei < (int)edges.size(); ei++) {
        //   ES::V3d pdiff =
        //     reducedTheta.segment<3>(nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 3) -
        //     reducedTheta0.segment<3>(nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 3);

        //  std::cout << ei << ": ";
        //  std::cout << pdiff.transpose() << std::endl;

        //  pdiff =
        //    reducedTheta.segment<3>(nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 6) -
        //    reducedTheta0.segment<3>(nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 6);

        //  std::cout << ei << ": ";
        //  std::cout << pdiff.transpose() << std::endl;
        //}

        double E1 = computeE();

        std::cout << "    ls " << liter << ": E=" << E1 << "; E0=" << E0 << "; alpha=" << alpha << std::endl;

        if (E1 > E0) {
          alpha *= 0.5;
        }
        else {
          break;
        }
      }

      ES::VXd rClean = rInitialClean - alpha * dE_dr_clean;
      reducedTheta = assembleM * rClean;

      TriMeshGeo skMesh;
      for (int ei = 0; ei < (int)edges.size(); ei++) {
        ES::V3d p0 = reducedTheta.segment<3>(nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 3);
        ES::V3d p1 = reducedTheta.segment<3>(nSphereDOFs + ei * (templateCylinder.numVertices() + 9) + 6);
        skMesh.addMesh(createSingleTriangleMesh(Vec3d(p0.data()), Vec3d(p1.data()), Vec3d(p0.data()) + Vec3d(1e-6)));
      }
      skMesh.save(fmt::format("iter{:04d}-sk.obj", giter));

      E0 = computeE(1);

      createVoxelMesh(gridVertices, voxels, theta, fmt::format("iter{:04d}-sample.obj", giter).c_str());

      double maxChange = alpha * dE_dr.cwiseAbs().maxCoeff();
      std::cout << "max change: " << maxChange << std::endl;

      lastAlpha = alpha, lastMaxChange = maxChange;  // [msd_shape_opt]
      reportIteration(giter + 1);                    // [msd_shape_opt]

      if (maxChange < 1e-5) {
        break;
      }
    }
  }
}