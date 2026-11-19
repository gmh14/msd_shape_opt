// [msd_shape_opt] Writes the two template primitive meshes that optimizeContinuous() loads from the
// current working directory:
//
//   s.obj  : template of a medial-vertex primitive ("sphere"), 8 vertices  (|r_k| = 8 in Sec. 6.2.1)
//   c1.obj : template of a medial-edge primitive ("cylinder"), 14 vertices (3 rings x 4 + 2 cap vertices)
//
// The two files were never committed to the original repository, so they are reconstructed from the
// constraints that the code imposes on them (see README.md, "Template primitives"):
//
//  * VertexRFunction: the vertex primitive is {c + r_i * d_i}, d_i = s.obj vertex i normalized to unit
//    length (gradientOptimizer.cpp normalizes them right after loading), so any closed star-shaped
//    8-vertex mesh works. We use VegaFEM's createCubeMesh, whose corners (+-1,+-1,+-1) are exactly the
//    kind of non-unit vertices that the normalization step implies.
//
//  * EdgeRFunction: vertex i (i < n-2) is placed at R * (x_i * |c1 - c0|, y_i * r_i, z_i * r_i) + c0,
//    i.e. x in [0, 1] is the axial parameter and (y, z) is a unit cross-section direction; vertex n-2 is
//    the cap beyond c1 and vertex n-1 the cap beyond c0. ALOG(UCylinder.rows() == 14) and the cone
//    subspace UCylinder used for the medial-axis baseline (rows 0-3 & 12 -> radius A, rows 4-7 -> mean,
//    rows 8-11 & 13 -> radius B) match VegaFEM's createCylinderMesh(r, h, 4, 2) vertex numbering exactly:
//    ring 0 = rows 0-3, ring 1 = rows 4-7, ring 2 = rows 8-11, lower cap = 12 (fan to ring 0) and
//    upper cap = 13 (fan to ring 2). For the mesh to stay closed and star-shaped, ring 0 must therefore lie
//    at the c1 end (x = 1, next to vertex 12) and ring 2 at the c0 end (x = 0, next to vertex 13).
//
//   usage: makeTemplates [output_dir]

#include "../common/primitiveTemplates.h"

#include <filesystem>
#include <iostream>

int main(int argc, char *argv[])
{
  std::filesystem::path outDir = (argc > 1) ? std::filesystem::path(argv[1]) : std::filesystem::current_path();
  std::filesystem::create_directories(outDir);

  TriMeshGeo sphere = MSDShapeOpt::makeSphereTemplate();
  TriMeshGeo cylinder = MSDShapeOpt::makeCylinderTemplate();

  if (sphere.numVertices() != 8 || cylinder.numVertices() != 14) {
    std::cerr << "Unexpected template sizes: " << sphere.numVertices() << ", " << cylinder.numVertices() << std::endl;
    return 1;
  }

  std::filesystem::path sphereFile = outDir / "s.obj";
  std::filesystem::path cylinderFile = outDir / "c1.obj";
  if (!sphere.save(sphereFile.string()) || !cylinder.save(cylinderFile.string())) {
    std::cerr << "Cannot write the templates to " << outDir << std::endl;
    return 1;
  }

  std::cout << "Wrote " << sphereFile << " (" << sphere.numVertices() << " vertices, " << sphere.numTriangles() << " triangles)\n";
  std::cout << "Wrote " << cylinderFile << " (" << cylinder.numVertices() << " vertices, " << cylinder.numTriangles() << " triangles)\n";
  return 0;
}
