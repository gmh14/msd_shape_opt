#pragma once

#include "EigenSupport.h"

#include <vector>
#include <array>
#include <functional>

namespace MedialAxisRepresentation
{
// [msd_shape_opt] (additive) state of the optimization reported after the initial evaluation
// (iteration = 0) and after every accepted gradient-descent step (iteration = 1, 2, ...).
struct OptimizeContinuousIterationInfo
{
  int iteration = 0;
  const VegaFEM::EigenSupport::VXd *theta = nullptr;         // per-voxel density in [theta_min, 1]
  const VegaFEM::EigenSupport::VXd *reducedTheta = nullptr;  // all primitive parameters (see rfunc.h for the layout)
  int numSphereTemplateVertices = 0;                         // #vertices of the template sphere (s.obj)
  int numCylinderTemplateVertices = 0;                       // #vertices of the template cylinder (c1.obj)
  double compliance = 0;                                     // 1/2 u^T K u
  double volumePenalty = 0;                                  // w/2 max(V - V0, 0)^2
  double volumeFraction = 0;                                 // fraction of filled voxels
  double energy = 0;                                         // compliance + volumePenalty
  double gradientNorm = 0;                                   // norm of the (reduced) gradient used for the step
  double stepSize = 0;                                       // accepted line-search step
  double maxChange = 0;                                      // convergence measure of the original code
  int lineSearchIterations = 0;
};

// [msd_shape_opt] (additive) options; the default values are exactly the values that were
// hard-coded in the original implementation.
struct OptimizeContinuousOptions
{
  int maxIterations = 200;
  double initialRadius = 0.04;
  std::function<void(const OptimizeContinuousIterationInfo &)> iterationCallback;
};

void optimizeContinuous(
  const std::vector<VegaFEM::EigenSupport::V3d> &gridVertices, const std::vector<std::array<int, 8>> voxels,
  const std::vector<VegaFEM::EigenSupport::V3d> &spheres, const std::vector<std::array<int, 2>> &edges,
  const VegaFEM::EigenSupport::VXd &fextIn, const std::vector<int> &fixedVertices, double v0, int method = 0,
  const OptimizeContinuousOptions &options = OptimizeContinuousOptions());

}