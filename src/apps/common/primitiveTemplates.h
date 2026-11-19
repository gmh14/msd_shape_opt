#pragma once

// [msd_shape_opt] Template primitive meshes loaded by optimizeContinuous() from the working directory
// (s.obj and c1.obj). See src/apps/makeTemplates/makeTemplates.cpp and README.md for how they were
// reconstructed from the constraints imposed by rfunc.cpp / gradientOptimizer.cpp.

#include "createTriMesh.h"
#include "triMeshGeo.h"

namespace MSDShapeOpt
{
// vertex primitive: 8-vertex cube with corners at (+-1, +-1, +-1);
// gradientOptimizer.cpp normalizes the vertices onto the unit sphere right after loading s.obj
inline TriMeshGeo makeSphereTemplate()
{
  return createCubeMesh(Vec3d(0.0, 0.0, 0.0), 2.0);
}

// edge primitive: createCylinderMesh(1, 1, 4 slices, 2 stacks) (3 rings x 4 vertices + 2 cap vertices),
// moved rigidly into EdgeRFunction's frame: x = axial parameter in [0, 1], (y, z) = unit cross-section
// direction; ring 0 and cap vertex 12 at the c1 end (x = 1), ring 2 and cap vertex 13 at the c0 end (x = 0).
inline TriMeshGeo makeCylinderTemplate()
{
  TriMeshGeo cylinder = createCylinderMesh(1.0, 1.0, 4, 2);
  for (int vi = 0; vi < cylinder.numVertices(); vi++) {
    Vec3d p = cylinder.pos(vi);
    cylinder.pos(vi) = Vec3d(0.5 - p[1], p[0], p[2]);
  }
  return cylinder;
}
}  // namespace MSDShapeOpt
