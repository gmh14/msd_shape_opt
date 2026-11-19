#pragma once

#include "EigenSupport.h"
#include "minivector.h"
#include "mat4d.h"

#if __has_include("vegafem/sparseMatrix/sparseMatrix.h")
#define HAS_VEGA_SPARSE
class SparseMatrix;
#endif

namespace VegaFEM
{
namespace EigenSupport
{
#if defined(HAS_VEGA_SPARSE)
// create sparse matrix
void CreateSparseMatrix(const SparseMatrix *Asrc, SpMatD &Adst);
SpMatD CreateSparseMatrix(const SparseMatrix *Asrc);
SparseMatrix *CreateSparseMatrix(const SpMatD &A);

void UpdateSparseMatrix(SpMatD &Adst, const SparseMatrix *Asrc);
void UpdateSparseMatrix(SparseMatrix *Adst, const SpMatD &Asrc);
#endif

Vec2d toVec2d(const V2d &p);
Vec3d toVec3d(const V3d &p);
Vec4d toVec4d(const V4d &p);
Mat3d toMat3d(const M3d &m);
Mat4d toMat4d(const M4d &m);

V2d toESV2d(const Vec2d &p);
V3d toESV3d(const Vec3d &p);
V4d toESV4d(const Vec4d &p);
M3d toESM3d(const Mat3d &m);
M4d toESM4d(const Mat4d &m);

inline Vec2d toVec2d(const V2d &p)
{
  return Vec2d(p[0], p[1]);
}

inline Vec3d toVec3d(const V3d &p)
{
  return Vec3d(p[0], p[1], p[2]);
}

inline Vec3i toVec3i(const V3i &p)
{
  return Vec3i(p[0], p[1], p[2]);
}

inline Vec4d toVec4d(const V4d &p)
{
  return Vec4d(p[0], p[1], p[2], p[3]);
}

inline Mat3d toMat3d(const M3d &m)
{
  return Mat3d(
    m(0, 0), m(0, 1), m(0, 2),
    m(1, 0), m(1, 1), m(1, 2),
    m(2, 0), m(2, 1), m(2, 2));
}

inline Mat4d toMat4d(const M4d &m)
{
  return Mat4d(
    m(0, 0), m(0, 1), m(0, 2), m(0, 3),
    m(1, 0), m(1, 1), m(1, 2), m(1, 3),
    m(2, 0), m(2, 1), m(2, 2), m(2, 3),
    m(3, 0), m(3, 1), m(3, 2), m(3, 3));
}

inline V2d toESV2d(const Vec2d &p)
{
  return V2d(p[0], p[1]);
}

inline V3d toESV3d(const Vec3d &p)
{
  return V3d(p[0], p[1], p[2]);
}

inline V4d toESV4d(const Vec4d &p)
{
  return V4d(p[0], p[1], p[2], p[3]);
}

inline M3d toESM3d(const Mat3d &m)
{
  M3d ret;
  ret << m[0][0], m[0][1], m[0][2],
    m[1][0], m[1][1], m[1][2],
    m[2][0], m[2][1], m[2][2];

  return ret;
}

inline M4d toESM4d(const Mat4d &m)
{
  M4d ret;
  ret << m[0][0], m[0][1], m[0][2], m[0][3],
    m[1][0], m[1][1], m[1][2], m[1][3],
    m[2][0], m[2][1], m[2][2], m[2][3],
    m[3][0], m[3][1], m[3][2], m[3][3];

  return ret;
}

}  // namespace EigenSupport
}  // namespace VegaFEM