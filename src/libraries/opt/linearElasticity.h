#pragma once

#include "EigenSupport.h"

#include <vector>
#include <array>

namespace MedialAxisRepresentation
{
void computeElementK(const std::vector<VegaFEM::EigenSupport::V3d> &gridVertices, const std::vector<std::array<int, 8>> voxels, int ei,
  VegaFEM::EigenSupport::M24d &Kele);

VegaFEM::EigenSupport::SpMatD computeK(const std::vector<VegaFEM::EigenSupport::V3d> &gridVertices, const std::vector<std::array<int, 8>> voxels,
  const VegaFEM::EigenSupport::VXd &w);

void computeK(const std::vector<VegaFEM::EigenSupport::V3d> &gridVertices, const std::vector<std::array<int, 8>> voxels,
  const VegaFEM::EigenSupport::VXd &w, VegaFEM::EigenSupport::SpMatD &K);
}  // namespace MedialAxisRepresentation