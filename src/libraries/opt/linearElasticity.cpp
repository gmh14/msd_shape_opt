#include "linearElasticity.h"
#include "materials/linear_material.hpp"

#include "logger.h"

#include <tbb/parallel_for.h>
#include <tbb/spin_mutex.h>

namespace ES = VegaFEM::EigenSupport;

namespace MedialAxisRepresentation
{
Eigen::Matrix<double, 8, 8> GaussIntegrationFactor()
{
  // \int_{-1}^{1} f(x) dx \approx f(-1/sqrt(3)) + f(1/sqrt(3)).
  Eigen::Matrix<double, 8, 8> X0_coeff = Eigen::Matrix<double, 8, 8>::Zero();
  const double inv_sqrt_three = 1.0 / sqrt(3);
  const double r = (1.0 - inv_sqrt_three) / 2.0;
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 2; ++j)
      for (int k = 0; k < 2; ++k) {
        const double rx = (i == 0) ? r : (1 - r);
        const double ry = (j == 0) ? r : (1 - r);
        const double rz = (k == 0) ? r : (1 - r);
        Eigen::Matrix<double, 8, 1> coeff = Eigen::Matrix<double, 8, 1>::Zero();
        coeff << (1 - rx) * (1 - ry) * (1 - rz),
          (1 - rx) * (1 - ry) * rz,
          (1 - rx) * ry * (1 - rz),
          (1 - rx) * ry * rz,
          rx * (1 - ry) * (1 - rz),
          rx * (1 - ry) * rz,
          rx * ry * (1 - rz),
          rx * ry * rz;
        X0_coeff.col(i * 4 + j * 2 + k) = coeff;
      }
  return X0_coeff;
}

// return dphi (the deformation gradient) for a given voxel:
// undeformed_vertex is a point in material space.
// undeformed_cube are the vertices of an undeformed voxel
// deformed_cube are the vertices of a the deformed voxel.
Eigen::Matrix<double, 3, 3> DeformationGradient(
  const ES::V3d &undeformed_vertex,
  const Eigen::Matrix<double, 3, 8> &undeformed_cube,
  const Eigen::Matrix<double, 3, 8> &deformed_cube)
{
  // Rename variables.
  const ES::V3d &X = undeformed_vertex;
  const Eigen::Matrix<double, 3, 8> &X0 = undeformed_cube;
  const Eigen::Matrix<double, 3, 8> &x0 = deformed_cube;
  const double dx = X0(0, 4) - X0(0, 0);
  const double inv_dx = 1.0 / dx;
  const ES::V3d offset = X - X0.col(0);
  const double rx = offset.x() / dx;
  const double ry = offset.y() / dx;
  const double rz = offset.z() / dx;
  Eigen::Matrix<double, 3, 3> F = Eigen::Matrix<double, 3, 3>::Zero();
  const double x_factor[2] = { 1 - rx, rx };
  const double y_factor[2] = { 1 - ry, ry };
  const double z_factor[2] = { 1 - rz, rz };
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 2; ++j)
      for (int k = 0; k < 2; ++k) {
        F.col(0) += x0.col(4 * i + 2 * j + k) * (i == 0 ? -inv_dx : inv_dx) * y_factor[j] * z_factor[k];
        F.col(1) += x0.col(4 * i + 2 * j + k) * x_factor[i] * (j == 0 ? -inv_dx : inv_dx) * z_factor[k];
        F.col(2) += x0.col(4 * i + 2 * j + k) * x_factor[i] * y_factor[j] * (k == 0 ? -inv_dx : inv_dx);
      }
  return F;
}

// return dphi/dx for a given voxel:
// undeformed_vertex is a point in material space.
// undeformed_cube are the vertices of an undeformed voxel
// deformed_cube are the vertices of a the deformed voxel.
Eigen::Matrix<double, 9, 24> DeformationGradientPartialx(
  const ES::V3d &undeformed_vertex,
  const Eigen::Matrix<double, 3, 8> &undeformed_cube,
  const Eigen::Matrix<double, 3, 8> &deformed_cube)
{
  const ES::V3d &X = undeformed_vertex;
  const Eigen::Matrix<double, 3, 8> &X0 = undeformed_cube;
  Eigen::Matrix<double, 9, 24> Jacobian = Eigen::Matrix<double, 9, 24>::Zero();
  const double dx = X0(0, 4) - X0(0, 0);
  const double inv_dx = 1.0 / dx;
  const ES::V3d offset = X - X0.col(0);
  const double rx = offset.x() / dx;
  const double ry = offset.y() / dx;
  const double rz = offset.z() / dx;
  Eigen::Matrix<double, 3, 3> F = Eigen::Matrix<double, 3, 3>::Zero();
  const double x_factor[2] = { 1 - rx, rx };
  const double y_factor[2] = { 1 - ry, ry };
  const double z_factor[2] = { 1 - rz, rz };
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 2; ++j)
      for (int k = 0; k < 2; ++k) {
        const int index = 4 * i + 2 * j + k;
        const double scale_first_column = (i == 0 ? -inv_dx : inv_dx) * y_factor[j] * z_factor[k];
        Jacobian(0, 3 * index) += scale_first_column;
        Jacobian(1, 3 * index + 1) += scale_first_column;
        Jacobian(2, 3 * index + 2) += scale_first_column;
        const double scale_second_column = x_factor[i] * (j == 0 ? -inv_dx : inv_dx) * z_factor[k];
        Jacobian(3, 3 * index) += scale_second_column;
        Jacobian(4, 3 * index + 1) += scale_second_column;
        Jacobian(5, 3 * index + 2) += scale_second_column;
        const double scale_third_column = x_factor[i] * y_factor[j] * (k == 0 ? -inv_dx : inv_dx);
        Jacobian(6, 3 * index) += scale_third_column;
        Jacobian(7, 3 * index + 1) += scale_third_column;
        Jacobian(8, 3 * index + 2) += scale_third_column;
      }
  return Jacobian;
}

}  // namespace MedialAxisRepresentation

void MedialAxisRepresentation::computeElementK(const std::vector<VegaFEM::EigenSupport::V3d> &gridVertices, const std::vector<std::array<int, 8>> voxels, int ei, VegaFEM::EigenSupport::M24d &Kele)
{
  static Eigen::Matrix<double, 8, 8> X0_coeff = GaussIntegrationFactor();

  Eigen::Matrix<int, 8, 1> hex_id;
  hex_id << voxels[ei][0], voxels[ei][1], voxels[ei][2], voxels[ei][3],
    voxels[ei][4], voxels[ei][5], voxels[ei][6], voxels[ei][7];

  materials::LinearElasticityMaterial<3, double> material(1e6, 0.45);
  Eigen::Matrix<double, 3, 8> X0, x0;
  X0.setZero();
  x0.setZero();

  for (int j = 0; j < 8; ++j) {
    X0.col(j) = gridVertices[hex_id(j)];
    x0.col(j) = gridVertices[hex_id(j)];
  }

  Eigen::Matrix<double, 24, 24> dfdx0 = Eigen::Matrix<double, 24, 24>::Zero();
  Eigen::Matrix<double, 24, 1> df = Eigen::Matrix<double, 24, 1>::Zero();

  double hex_size = (gridVertices[hex_id[0]] - gridVertices[hex_id[1]]).norm();
  double hex_vol = hex_size * hex_size * hex_size;

  for (int j = 0; j < 8; ++j) {
    ES::V3d X = X0 * X0_coeff.col(j);

    const Eigen::Matrix<double, 9, 24> dFdxi =
      DeformationGradientPartialx(X, X0, x0);
    const Eigen::Matrix<double, 3, 3> F = DeformationGradient(X, X0, x0);
    const Eigen::Matrix<double, 9, 9> dPdF = material.StressDifferential(F);

    // dP = dPdF * dFdxi * dxi.
    dfdx0 += hex_vol / 8 * dFdxi.transpose() * dPdF * dFdxi;
  }

  Kele = dfdx0;
}

VegaFEM::EigenSupport::SpMatD MedialAxisRepresentation::computeK(const std::vector<VegaFEM::EigenSupport::V3d> &gridVertices, const std::vector<std::array<int, 8>> voxels,
  const VegaFEM::EigenSupport::VXd &w)
{
  // std::vector<Eigen::Triplet<T>> triplet_list;
  int vertex_num = static_cast<int>(gridVertices.size());
  int hex_num = static_cast<int>(voxels.size());

  // tbb::concurrent_vector<typename Eigen::Triplet<T>> triplet_list;
  // triplet_list.reserve(hex_num * 64);
  std::vector<ES::TripletD> triplet_list(hex_num * 24 * 24);

  // for (int i = 0; i < hex_num; ++i) {
  tbb::parallel_for(0, hex_num, [&](int ei) {
    ES::M24d Klocal;
    computeElementK(gridVertices, voxels, ei, Klocal);

    int offset = ei * 24 * 24;

    for (int j = 0; j < 8; ++j) {
      for (int k = 0; k < 8; ++k) {
        for (int s = 0; s < 3; ++s) {
          for (int t = 0; t < 3; ++t) {
            // triplet_list.emplace_back(3 * hex_id(j) + s, 3 * hex_id(k) + t, -dfdx0(3 * j + s, 3 * k + t));
            triplet_list[offset + j * 8 * 3 * 3 + k * 3 * 3 + s * 3 + t] = ES::TripletD(3 * voxels[ei][j] + s, 3 * voxels[ei][k] + t, w[ei] * Klocal(3 * j + s, 3 * k + t));
          }
        }
      }
    }
  });

  ES::SpMatD K(vertex_num * 3, vertex_num * 3);
  K.setFromTriplets(triplet_list.begin(), triplet_list.end());

  return K;
}

void MedialAxisRepresentation::computeK(const std::vector<VegaFEM::EigenSupport::V3d> &gridVertices, const std::vector<std::array<int, 8>> voxels,
  const VegaFEM::EigenSupport::VXd &w, VegaFEM::EigenSupport::SpMatD &K)
{
  // std::vector<Eigen::Triplet<T>> triplet_list;
  int vertex_num = static_cast<int>(gridVertices.size());
  int hex_num = static_cast<int>(voxels.size());

  // tbb::concurrent_vector<typename Eigen::Triplet<T>> triplet_list;
  // triplet_list.reserve(hex_num * 64);
  // std::vector<ES::TripletD> triplet_list(hex_num * 24 * 24);
  static std::vector<tbb::spin_mutex> locks;

  if (locks.size() == 0ull) {
    locks = std::vector<tbb::spin_mutex>(gridVertices.size() * 3);
  }

  memset(K.valuePtr(), 0, sizeof(double) * K.nonZeros());

  // for (int i = 0; i < hex_num; ++i) {
  tbb::parallel_for(0, hex_num, [&](int ei) {
    ES::M24d Klocal;
    computeElementK(gridVertices, voxels, ei, Klocal);

    int offset = ei * 24 * 24;

    for (int j = 0; j < 8; ++j) {
      for (int k = 0; k < 8; ++k) {
        for (int s = 0; s < 3; ++s) {
          for (int t = 0; t < 3; ++t) {
            int row = 3 * voxels[ei][j] + s;
            int col = 3 * voxels[ei][k] + t;
            double val = w[ei] * Klocal(3 * j + s, 3 * k + t);

            const auto beg = K.innerIndexPtr() + K.outerIndexPtr()[row];
            const auto end = K.innerIndexPtr() + K.outerIndexPtr()[row + 1];
            auto it = std::lower_bound(beg, end, col);
            ALOG(it != end && *it == col);

            std::ptrdiff_t offset = it - K.innerIndexPtr();

            locks[row].lock();

            K.valuePtr()[offset] += val;

            locks[row].unlock();
          }
        }
      }
    }
  });
}