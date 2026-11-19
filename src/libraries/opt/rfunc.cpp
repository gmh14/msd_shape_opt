#include "rfunc.h"
#include "logger.h"

#include "automaticDifferentiation_autodiff.h"
#include "EigenVegaTypeConverter.h"
#include "predicates.h"
#include "geometryQuery.h"

#include <cstring>

using namespace MedialAxisRepresentation;
namespace ES = VegaFEM::EigenSupport;

constexpr double minRadius = 0.02;

VertexRFunction::VertexRFunction(const TriMeshGeo &m):
  meshRest(m)
{
}

double VertexRFunction::func(const double *x) const
{
  thread_local TriMeshGeo meshCur = meshRest;

  Vec3d center(x[3], x[4], x[5]);
  Vec3d queryPt(x[0], x[1], x[2]);

  Vec3d segStart = center;
  Vec3d diff = queryPt - center;
  double r0 = len(diff);

  if (r0 > 1e-6) {
    diff /= r0;

    Vec3d segEnd = segStart + diff * 5;
    double segw[2], triw[3];
    for (int vi = 0; vi < meshCur.numVertices(); vi++) {
      double r = std::max(x[6 + vi], minRadius);
      meshCur.pos(vi) = center + meshRest.pos(vi) * r;
    }
    // meshCur.save("hh.obj");

    int triID = -1;
    for (int ti = 0; ti < meshCur.numTriangles(); ti++) {
      if (intersectSegTri(segStart.data(), segEnd.data(),
            meshCur.pos(ti, 0).data(), meshCur.pos(ti, 1).data(), meshCur.pos(ti, 2).data(), segw, triw)) {
        triID = ti;
        break;
      }
    }
    ALOG(triID >= 0);

    double r = len(segw[0] * segStart + segw[1] * segEnd - center);

    return r - r0;
  }
  else {
    return 0.1;
  }
}

using dl2 = VegaFEM::NonlinearOptimization::AutomaticDifferentiation_autodiff::dual2nd;
using dv3 = VegaFEM::NonlinearOptimization::AutomaticDifferentiation_autodiff::dualV3d;
dl2 compute_r(dl2 r[3], const ES::V3d d[3], const ES::V3d &d0)
{
  // p1 = c + d1 r1
  // p2 = c + d2 r2
  // p3 = c + d3 r3
  dv3 p0 = d[0] * r[0];
  dv3 p1 = d[1] * r[1];
  dv3 p2 = d[2] * r[2];

  dv3 e1 = p1 - p0;
  dv3 e2 = p2 - p0;
  dv3 n = e1.cross(e2);

  // nT (d0 t + c - (d1 r1 + c)) = 0
  // nT (d0 t - d1 r1) = 0
  // t = nT d1 r1 / nT d0

  dl2 a = n.dot(d[0]) * r[0];
  dl2 b = n.dot(d0);

  dl2 t = a / b;
  return t;
}

void VertexRFunction::gradient(const double *x, double *grad) const
{
  thread_local TriMeshGeo meshCur = meshRest;

  Vec3d center(x[3], x[4], x[5]);
  Vec3d queryPt(x[0], x[1], x[2]);

  Vec3d segStart = center;
  Vec3d diff = queryPt - center;
  double r0 = len(diff);
  if (r0 > 1e-6) {
    diff /= r0;

    Vec3d segEnd = segStart + diff * 5;
    double segw[2], triw[3];

    for (int vi = 0; vi < meshCur.numVertices(); vi++) {
      double r = std::max(x[6 + vi], minRadius);
      meshCur.pos(vi) = center + meshRest.pos(vi) * r;
    }

    int triID = -1;
    for (int ti = 0; ti < meshCur.numTriangles(); ti++) {
      if (intersectSegTri(segStart.data(), segEnd.data(),
            meshCur.pos(ti, 0).data(), meshCur.pos(ti, 1).data(), meshCur.pos(ti, 2).data(), segw, triw)) {
        triID = ti;
        break;
      }
    }
    ALOG(triID >= 0);

    std::memset(grad, 0, sizeof(double) * getNumDOFs());
    ES::VXd xcopy = ES::Mp<const ES::VXd>(x, getNumDOFs());

    // fd for the first 6 dof
    double eps = 1e-7;
    for (int i = 0; i < 6; i++) {
      xcopy[i] = x[i] + eps;
      double val1 = func(xcopy.data());

      xcopy[i] = x[i] - eps;
      double val2 = func(xcopy.data());

      xcopy[i] = x[i];

      grad[i] = (val1 - val2) / (2.0 * eps);
    }

    for (int i = 0; i < 3; i++) {
      int dofi = meshRest.triVtxID(triID, i) + 6;
      xcopy[dofi] = x[dofi] + eps;
      double val1 = func(xcopy.data());

      xcopy[dofi] = x[dofi] - eps;
      double val2 = func(xcopy.data());

      xcopy[dofi] = x[dofi];
      grad[dofi] = (val1 - val2) / (2.0 * eps);
    }

    /*
    * ES::V3d rs{
      x[6 + meshCur.triVtxID(triID, 0)],
      x[6 + meshCur.triVtxID(triID, 1)],
      x[6 + meshCur.triVtxID(triID, 2)]
    };

    ES::V3d ds[3] = {
      ES::toESV3d(meshCur.pos(triID, 0)).normalized(),
      ES::toESV3d(meshCur.pos(triID, 1)).normalized(),
      ES::toESV3d(meshCur.pos(triID, 2)).normalized(),
    };

    ES::V3d d0 = ES::toESV3d(diff);
    ES::V3d grad3;
    VegaFEM::NonlinearOptimization::AutomaticDifferentiation_autodiff::computeGradient<3>(compute_r, rs, grad3, ds, d0);

    grad[6 + meshCur.triVtxID(triID, 0)] = grad3[0];
    grad[6 + meshCur.triVtxID(triID, 1)] = grad3[1];
    grad[6 + meshCur.triVtxID(triID, 2)] = grad3[2];
    */
  }
  else {
    std::memset(grad, 0, sizeof(double) * getNumDOFs());
  }
}

EdgeRFunction::EdgeRFunction(const TriMeshGeo &mesh):
  meshRest(mesh)
{
}

double EdgeRFunction::func(const double *x) const
{
  thread_local TriMeshGeo meshCur = meshRest;

  Vec3d c[2] = {
    Vec3d(x[3], x[4], x[5]),
    Vec3d(x[6], x[7], x[8]),
  };

  Vec3d queryPt(x[0], x[1], x[2]);

  auto computeProjPt = [&c](const Vec3d &p) -> Vec3d {
    // (c[1] - c[0])^T ((c[1] - c[0]) t + c[0] - q) = 0
    // double t = dot(p - c[0], c[1] - c[0]) / dot(c[1] - c[0], c[1] - c[0]);
    // Vec3d projQueryPt = (c[1] - c[0]) * t + c[0];

    // return std::tuple(projQueryPt, t);
    return getClosestPointToLineSegment(p, c[0], c[1]);
  };

  Vec3d projQueryPt = computeProjPt(queryPt);
  Vec3d segStart = projQueryPt;
  Vec3d diff = queryPt - projQueryPt;
  double r0 = len(diff);
  if (r0 > 1e-6) {
    diff /= r0;

    Vec3d segEnd = segStart + diff * 5;
    double segw[2], triw[3];

    // compute R
    ES::V3d d0(1, 0, 0);
    ES::V3d d1 = ES::toESV3d(c[1] - c[0]).normalized();
    ES::M3d R;
    if (std::abs(std::abs(d0.dot(d1)) - 1) < 1e-5) {
      R.setIdentity();
    }
    else {
      ES::V3d rotAxis = d0.cross(d1).normalized();
      double rotAngle = std::acos(std::clamp(d0.dot(d1), -1.0, 1.0));

      Eigen::AngleAxisd aad(rotAngle, rotAxis);
      R = aad.toRotationMatrix();
    }

    double s = len(c[1] - c[0]);
    for (int vi = 0; vi < meshCur.numVertices() - 2; vi++) {
      double r = std::max(x[9 + vi], minRadius);
      ES::V3d p0(meshRest.pos(vi)[0] * s, meshRest.pos(vi)[1] * r, meshRest.pos(vi)[2] * r);
      ES::V3d p1 = R * p0 + ES::toESV3d(c[0]);
      meshCur.pos(vi) = Vec3d(p1.data());
    }

    {
      int vi = meshCur.numVertices() - 2;
      double r = std::max(x[9 + vi], minRadius);
      ES::V3d p0(s + r, 0, 0);
      ES::V3d p1 = R * p0 + ES::toESV3d(c[0]);
      meshCur.pos(vi) = Vec3d(p1.data());
    }

    {
      int vi = meshCur.numVertices() - 1;
      double r = std::max(x[9 + vi], minRadius);

      ES::V3d p0(-r, 0, 0);
      ES::V3d p1 = R * p0 + ES::toESV3d(c[0]);
      meshCur.pos(vi) = Vec3d(p1.data());
    }

    // meshCur.save("hh.obj");

    int triID = -1;
    for (int ti = 0; ti < meshCur.numTriangles(); ti++) {
      if (intersectSegTri(segStart.data(), segEnd.data(),
            meshCur.pos(ti, 0).data(), meshCur.pos(ti, 1).data(), meshCur.pos(ti, 2).data(), segw, triw)) {
        triID = ti;
        break;
      }
    }

    if (triID < 0) {
      return -0.02;
    }

    double r = len(segw[0] * segStart + segw[1] * segEnd - projQueryPt);

    return r - r0;
  }
  else {
    return 0.02;
  }
}

void EdgeRFunction::gradient(const double *x, double *grad) const
{
  thread_local TriMeshGeo meshCur = meshRest;

  Vec3d c[2] = {
    Vec3d(x[3], x[4], x[5]),
    Vec3d(x[6], x[7], x[8]),
  };

  Vec3d queryPt(x[0], x[1], x[2]);

  auto computeProjPt = [&c](const Vec3d &p) -> Vec3d {
    // (c[1] - c[0])^T ((c[1] - c[0]) t + c[0] - q) = 0
    // double t = dot(p - c[0], c[1] - c[0]) / dot(c[1] - c[0], c[1] - c[0]);
    // Vec3d projQueryPt = (c[1] - c[0]) * t + c[0];

    // return std::tuple(projQueryPt, t);
    return getClosestPointToLineSegment(p, c[0], c[1]);
  };

  Vec3d projQueryPt = computeProjPt(queryPt);
  Vec3d segStart = projQueryPt;
  Vec3d diff = queryPt - projQueryPt;
  double r0 = len(diff);
  if (r0 > 1e-6) {
    diff /= r0;

    Vec3d segEnd = segStart + diff * 5;
    double segw[2], triw[3];

    // compute R
    ES::V3d d0(1, 0, 0);
    ES::V3d d1 = ES::toESV3d(c[1] - c[0]).normalized();
    ES::M3d R;
    if (std::abs(std::abs(d0.dot(d1)) - 1) < 1e-5) {
      R.setIdentity();
    }
    else {
      ES::V3d rotAxis = d0.cross(d1).normalized();
      double rotAngle = std::acos(std::clamp(d0.dot(d1), -1.0, 1.0));

      Eigen::AngleAxisd aad(rotAngle, rotAxis);
      R = aad.toRotationMatrix();
    }

    double s = len(c[1] - c[0]);
    for (int vi = 0; vi < meshCur.numVertices() - 2; vi++) {
      double r = std::max(x[9 + vi], minRadius);
      ES::V3d p0(meshRest.pos(vi)[0] * s, meshRest.pos(vi)[1] * r, meshRest.pos(vi)[2] * r);
      ES::V3d p1 = R * p0 + ES::toESV3d(c[0]);
      meshCur.pos(vi) = Vec3d(p1.data());
    }

    {
      int vi = meshCur.numVertices() - 2;
      double r = std::max(x[9 + vi], minRadius);
      ES::V3d p0(s + r, 0, 0);
      ES::V3d p1 = R * p0 + ES::toESV3d(c[0]);
      meshCur.pos(vi) = Vec3d(p1.data());
    }

    {
      int vi = meshCur.numVertices() - 1;
      double r = std::max(x[9 + vi], minRadius);
      ES::V3d p0(-r, 0, 0);
      ES::V3d p1 = R * p0 + ES::toESV3d(c[0]);
      meshCur.pos(vi) = Vec3d(p1.data());
    }

    int triID = -1;
    for (int ti = 0; ti < meshCur.numTriangles(); ti++) {
      if (intersectSegTri(segStart.data(), segEnd.data(),
            meshCur.pos(ti, 0).data(), meshCur.pos(ti, 1).data(), meshCur.pos(ti, 2).data(), segw, triw)) {
        triID = ti;
        break;
      }
    }

    std::memset(grad, 0, sizeof(double) * getNumDOFs());
    if (triID < 0) {
      return;
    }

    ES::VXd xcopy = ES::Mp<const ES::VXd>(x, getNumDOFs());
    // fd for the first 6 dof
    double eps = 1e-7;
    for (int i = 0; i < 9; i++) {
      xcopy[i] = x[i] + eps;
      double val1 = func(xcopy.data());

      xcopy[i] = x[i] - eps;
      double val2 = func(xcopy.data());

      xcopy[i] = x[i];

      grad[i] = (val1 - val2) / (2.0 * eps);
    }

    for (int i = 0; i < 3; i++) {
      int dofi = meshRest.triVtxID(triID, i) + 9;
      xcopy[dofi] = x[dofi] + eps;
      double val1 = func(xcopy.data());

      xcopy[dofi] = x[dofi] - eps;
      double val2 = func(xcopy.data());

      xcopy[dofi] = x[dofi];
      grad[dofi] = (val1 - val2) / (2.0 * eps);
    }
  }
  else {
    std::memset(grad, 0, sizeof(double) * getNumDOFs());
  }
}

UnionRFunction::UnionRFunction(std::shared_ptr<const RFunction> f1_, std::shared_ptr<const RFunction> f2_):
  f1(f1_), f2(f2_)
{
}

double UnionRFunction::func(const double *x) const
{
  double v1 = f1->func(x);
  double v2 = f2->func(x + f1->getNumDOFs());

  return v1 + v2 + std::sqrt(v1 * v1 + v2 * v2);
}

void UnionRFunction::gradient(const double *x, double *grad) const
{
  double v1 = f1->func(x);
  double v2 = f2->func(x + f1->getNumDOFs());

  double s = std::sqrt(v1 * v1 + v2 * v2);
  s = std::max(s, 1e-8);

  double dfdv1 = 1 + v1 / s;
  double dfdv2 = 1 + v2 / s;

  std::memset(grad, 0, sizeof(double) * getNumDOFs());

  f1->gradient(x, grad);
  for (int i = 0; i < f1->getNumDOFs(); i++) {
    grad[i] *= dfdv1;
  }

  f2->gradient(x + f1->getNumDOFs(), grad + f1->getNumDOFs());
  for (int i = 0; i < f2->getNumDOFs(); i++) {
    grad[i + f1->getNumDOFs()] *= dfdv2;
  }
}