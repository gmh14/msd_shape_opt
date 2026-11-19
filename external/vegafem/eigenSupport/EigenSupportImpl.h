#pragma once

#ifndef _EIGEN_SUPPORT_IMPL_H_
#  define _EIGEN_SUPPORT_IMPL_H_

#  include "EigenDef.h"

#  include <initializer_list>
#  include <vector>
#  include <array>
#  include <stdexcept>
#  include <utility>
#  include <fstream>
#  include <iostream>

extern "C"
{
struct sparse_matrix;
typedef struct sparse_matrix *sparse_matrix_t;
}

namespace VegaFEM
{
namespace EigenSupport
{
// from boost
inline void hashCombine(std::size_t &h, std::size_t k)
{
  const std::size_t m = UINT64_C(0xc6a4a7935bd1e995);
  const int r = 47;

  k *= m;
  k ^= k >> r;
  k *= m;

  h ^= k;
  h *= m;

  // Completely arbitrary number, to prevent 0's
  // from hashing to 0.
  h += 0xe6546b64;
}

struct IntPairHash
{
  std::size_t operator()(const std::pair<int, int> &pr) const
  {
    std::size_t h = 0;
    hashCombine(h, std::hash<int>()(pr.first));
    hashCombine(h, std::hash<int>()(pr.second));

    return h;
  }
};

struct IntPairEqual
{
  bool operator()(const std::pair<int, int> &pr1, const std::pair<int, int> &pr2) const
  {
    return pr1.first == pr2.first && pr1.second == pr2.second;
  }
};

template<int N>
struct IntArrayHash
{
  std::size_t operator()(const std::array<int, N> &val) const
  {
    std::size_t h = 0;
    for (int i = 0; i < N; i++) {
      hashCombine(h, std::hash<int>()(val[i]));
    }

    return h;
  }
};

template<int N>
struct IntArrayEqual
{
  bool operator()(const std::array<int, N> &v1, const std::array<int, N> &v2) const
  {
    return std::memcmp(v1.data(), v2.data(), sizeof(std::array<int, N>)) == 0;
  }
};

template<int N>
struct IntArrayLess
{
  bool operator()(const std::array<int, N> &v1, const std::array<int, N> &v2) const
  {
    return std::memcmp(v1.data(), v2.data(), sizeof(std::array<int, N>)) < 0;
  }
};

using EntryMap = std::unordered_map<std::pair<int, int>, std::ptrdiff_t, IntPairHash, IntPairEqual>;

// ======================================================================
// for dense matrix
VEGA_EXPORT void TensorProduct(M3d &r, const V3d &v1, const V3d &v2);
VEGA_EXPORT M3d TensorProduct(const V3d &v1, const V3d &v2);

template<typename DerivedA, typename DerivedB, typename DerivedC>
void TensorProduct(Eigen::MatrixBase<DerivedC> &r, const Eigen::MatrixBase<DerivedA> &v1, const Eigen::MatrixBase<DerivedB> &v2);

VEGA_EXPORT M3d SkewMatrix(const V3d &v);

// ======================================================================
// for sparse matrix
struct VEGA_EXPORT SparseMatrixMKL
{
  sparse_matrix_t mklHandle;
};

VEGA_EXPORT SpMatD CreateSparseMatrix(ConstRefMatXd A);

VEGA_EXPORT void CreateWeightMatrix(int numRows, int numCols, int numVertices, int numWeightsPerVertex,
  const int *vertexIndices, const int *interpolationVertexIndices, const double *interpolationWeights, int inflate, SpMatD &A);
VEGA_EXPORT SpMatD CreateWeightMatrix(int numRows, int numCols, int numVertices, int numWeightsPerVertex,
  const int *vertexIndices, const int *interpolationVertexIndices, const double *interpolationWeights, int inflate);

VEGA_EXPORT SparseMatrixMKL CreateSparseMatrixMKL(SpMatD &A);
VEGA_EXPORT void DestroySparseMatrixMKL(SparseMatrixMKL &m);

VEGA_EXPORT void Expand3(const SpMatD &A, SpMatD &A3d);
VEGA_EXPORT void ExpandN(const SpMatD &A, SpMatD &A3d, int nd);

template<typename... T>
SpMatD ConcatCol(T &&...Ms);

template<typename... T>
SpMatD ConcatRow(T &&...Ms);

template<typename Iterator>
SpMatD ConcatCol(Iterator beg, Iterator end);

template<typename Iterator>
SpMatD ConcatRow(Iterator beg, Iterator end);

template<typename... T>
void MergeSparseMatrix(SpMatD &Adst, T &&...Ms);

template<typename MatrixType>
void Zero(MatrixType &m);

// IO
VEGA_EXPORT int SaveSparseMatrix(const char *filename, const SpMatD &A);
VEGA_EXPORT int LoadSparseMatrix(const char *filename, SpMatD &A);

// utility
VEGA_EXPORT std::ptrdiff_t FindEntryOffset(const SpMatD &A, int row, int col);
VEGA_EXPORT void BuildEntryMap(const SpMatD &A, EntryMap &entryMap);

// transpose
VEGA_EXPORT void TransposeMapping(const SpMatD &A, const SpMatD &AT, SpMatI &mapping);
VEGA_EXPORT void TransposeTransfer(const SpMatD &A, SpMatD &AT, const SpMatI &mapping, int parallel = 0);

// mapping
VEGA_EXPORT void Small2Big(const SpMatD &Asmall, const SpMatD &Abig, int rowStart, int colStart, SpMatI &mapping);
VEGA_EXPORT void Small2Big(const SpMatD &Asmall, const SpMatD &Abig, const std::vector<int> &dofs, SpMatI &mapping);
VEGA_EXPORT void Small2Big(const SpMatD &Asmall, const SpMatD &Abig, const std::vector<int> &dofsRow, const std::vector<int> &dofsCol, SpMatI &mapping);

VEGA_EXPORT void Big2Small(const SpMatD &Abig, const SpMatD &Asmall, int rowStart, int colStart, SpMatI &mapping, int allowDifferentTopo = 0);
VEGA_EXPORT void Big2Small(const SpMatD &Abig, const SpMatD &Asmall, const std::vector<int> &dofsRow, const std::vector<int> &dofsCol, SpMatI &mapping, int allowDifferentTopo = 0);

VEGA_EXPORT void RemoveRowsCols(const SpMatD &Abig, const SpMatD &Asmall, const std::vector<int> &removedDofs, SpMatI &mapping);
VEGA_EXPORT void RemoveRowsCols(const SpMatD &Abig, const std::vector<int> &removedDofs, SpMatD &mat);
VEGA_EXPORT void RemoveRowsCols(const SpMatD &Abig, const std::vector<int> &removedRowDofs, const std::vector<int> &removedColDofs, SpMatD &mat);

VEGA_EXPORT void SelectRowsCols(const SpMatD &Abig, const std::vector<int> &selectedDofs, SpMatD &mat);
VEGA_EXPORT void SelectRowsCols(const SpMatD &Abig, const std::vector<int> &selectedRowDofs, const std::vector<int> &selectedColDofs, SpMatD &mat);

VEGA_EXPORT void RemoveRows(const SpMatD &Abig, const SpMatD &Asmall, const std::vector<int> &removedDofs, SpMatI &mapping);
VEGA_EXPORT void RemoveRows(int dofAll, const std::vector<int> &removedDofs, std::vector<int> &big2smallMapping, std::vector<int> &small2bigMapping);

VEGA_EXPORT void AddSmallToBig(double coeff, const SpMatD &Asmall, SpMatD &Abig, double beta, const SpMatI &mapping, int parallel = 0);
VEGA_EXPORT void AddBigToSmall(double coeff, const SpMatD &Abig, SpMatD &Asmall, double beta, const SpMatI &mapping, int parallel = 0);

VEGA_EXPORT void TransferSmallToBig(const SpMatD &Asmall, SpMatD &Abig, const SpMatI &mapping, int parallel = 0);
VEGA_EXPORT void TransferSmallToBig(ConstRefVecXd Asmall, RefVecXd Abig, const std::vector<int> &mapping, int parallel = 0);
VEGA_EXPORT void TransferBigToSmall(const SpMatD &Abig, SpMatD &Asmall, const SpMatI &mapping, int parallel = 0);
VEGA_EXPORT void TransferBigToSmall(ConstRefVecXd Abig, RefVecXd Asmall, const std::vector<int> &mapping, int parallel = 0);

// sparse operation
VEGA_EXPORT void Mv(const SpMatD &A, ConstRefVecXd v, RefVecXd b, int transpose = 0);
VEGA_EXPORT void Mv(const SpMatD &A, ConstRefVecXd v, RefVecXd b, double alpha, double beta, int transpose = 0);

VEGA_EXPORT void Mv(SparseMatrixMKL A, ConstRefVecXd v, RefVecXd b, int transpose = 0);
VEGA_EXPORT void Mv(SparseMatrixMKL A, ConstRefVecXd v, RefVecXd b, double alpha, double beta, int transpose = 0);

VEGA_EXPORT void Mm(const SpMatD &A, ConstRefMatXd B, RefMatXd C, int transpose = 0);
VEGA_EXPORT void Mm(const SpMatD &A, ConstRefMatXd B, RefMatXd C, double alpha, double beta, int transpose = 0);

VEGA_EXPORT void Mm(SparseMatrixMKL A, ConstRefMatXd B, RefMatXd C, int transpose = 0);
VEGA_EXPORT void Mm(SparseMatrixMKL A, ConstRefMatXd B, RefMatXd C, double alpha, double beta, int transpose = 0);

VEGA_EXPORT void Mm(const SpMatD &A, const SpMatD &B, SpMatD &C, int transpose = 0);
VEGA_EXPORT void Mm(SparseMatrixMKL A, SparseMatrixMKL B, SparseMatrixMKL C, int transpose = 0);

struct SymbolicMmData;
VEGA_EXPORT void SymbolicMm(const SpMatD &A, const SpMatD &B, SpMatD &C, SymbolicMmData **dat, int transpose = 0);
VEGA_EXPORT void Mm(const SpMatD &A, const SpMatD &B, const SymbolicMmData *dat, SpMatD &C, int transpose = 0);
VEGA_EXPORT void DestroySymbolicMmData(SymbolicMmData *dat);

// In the following three routines,
// when transpose=1 (default), it means doing A^T M A,
// when transpose=0, it means doing A M A^T,
// The routine may not be fast compared to Mm if B is too big
VEGA_EXPORT void ATA(const SpMatD &A, SpMatD &C, int transpose = 1);
// B must be symmetric, and can optionally be represented by upper triangle part
VEGA_EXPORT void ABA(const SpMatD &A, const SpMatD &B, SpMatD &C, int transpose = 1);
VEGA_EXPORT void ABA(const SpMatD &A, const double *W, SpMatD &C, int transpose = 1);

VEGA_EXPORT double vTMv(const SpMatD &A, ConstRefVecXd v, RefVecXd b, int transpose = 0);

VEGA_EXPORT void TestSparseRoutine(int dim1, int dim2);

// dense utilities
VEGA_EXPORT M3Xd ToM3Xd(const double *data, size_t size);
VEGA_EXPORT MX3d ToMX3d(const double *data, size_t size);
VEGA_EXPORT VXd ToVXd(const double *data, size_t size);

template<typename Derived>
V3d RotationToVec3(const Eigen::MatrixBase<Derived> &R);

VEGA_EXPORT void RotationOnlySVD(const M3d &A, M3d *UOut, M3d *VOut, V3d *SOut);

VEGA_EXPORT void RigidFit(const M3Xd &p0, const M3Xd &p1, M3d &R, V3d &t, const double *weights = nullptr);

template<typename Derived>
int readMatrix(const char *filename, Eigen::PlainObjectBase<Derived> &mat, int overwrite = 1);
template<typename Derived>
int writeMatrix(const char *filename, const Eigen::PlainObjectBase<Derived> &mat);
}  // namespace EigenSupport

template<typename Derived>
EigenSupport::V3d EigenSupport::RotationToVec3(const Eigen::MatrixBase<Derived> &R)
{
  Eigen::Quaterniond q;
  q = R;

  double cosThetaHalf = q.w();
  double angle = 2 * acos(cosThetaHalf);

  V3d axis(q.x(), q.y(), q.z());
  axis.normalize();

  return axis * angle;
}

template<typename... T>
void EigenSupport::MergeSparseMatrix(SpMatD &Adst, T &&...Ms)
{
  static_assert(
    std::is_same<
      std::integer_sequence<bool, true, std::is_same<std::decay_t<T>, SpMatD>::value...>,
      std::integer_sequence<bool, std::is_same<std::decay_t<T>, SpMatD>::value..., true>>::value,
    "T should be SpMatD");

  std::array<const SpMatD *, sizeof...(Ms)> matrices = { &Ms... };

  std::vector<TripletD> entries;
  Eigen::Index numRows = 0, numCols = 0;
  for (const SpMatD *mptr : matrices) {
    const SpMatD &m = *mptr;
    if (numRows == 0 && numCols == 0) {
      numRows = m.rows();
      numCols = m.cols();
    }

    if (numRows != m.rows() || numCols != m.cols())
      throw std::domain_error("Different sparse matrix size");

    for (Eigen::Index outeri = 0; outeri < m.outerSize(); outeri++) {
      for (SpMatD::InnerIterator it(m, outeri); it; ++it) {
        entries.emplace_back((int)it.row(), (int)it.col(), 1.0);
      }
    }
  }

  Adst.resize(numRows, numCols);
  Adst.setFromTriplets(entries.begin(), entries.end());
}

template<typename... T>
EigenSupport::SpMatD EigenSupport::ConcatCol(T &&...Ms)
{
  static_assert(
    std::is_same<
      std::integer_sequence<bool, true, std::is_same<std::decay_t<T>, SpMatD>::value...>,
      std::integer_sequence<bool, std::is_same<std::decay_t<T>, SpMatD>::value..., true>>::value,
    "T should be SpMatD");

  std::array<const SpMatD *, sizeof...(Ms)> matrices = { &Ms... };

  std::vector<TripletD> entries;
  Eigen::Index numRows = 0, numCols = 0;
  for (const SpMatD *mptr : matrices) {
    const SpMatD &m = *mptr;
    if (numRows == 0) {
      numRows = m.rows();
    }

    if (numRows != m.rows())
      throw std::domain_error("Different sparse matrix rows");

    for (Eigen::Index outeri = 0; outeri < m.outerSize(); outeri++) {
      for (SpMatD::InnerIterator it(m, outeri); it; ++it) {
        entries.emplace_back((int)it.row(), (int)it.col() + (int)numCols, 1.0);
      }
    }

    numCols += (int)m.cols();
  }

  SpMatD A(numRows, numCols);
  A.setFromTriplets(entries.begin(), entries.end());

  return A;
}

template<typename Iterator>
EigenSupport::SpMatD EigenSupport::ConcatCol(Iterator beg, Iterator end)
{
  std::vector<TripletD> entries;
  Eigen::Index numRows = 0, numCols = 0;
  for (Iterator it = beg; it != end; ++it) {
    const SpMatD &m = *it;
    if (numRows == 0) {
      numRows = m.rows();
    }

    if (numRows != m.rows())
      throw std::invalid_argument("Different sparse matrix rows");

    for (Eigen::Index outeri = 0; outeri < m.outerSize(); outeri++) {
      for (SpMatD::InnerIterator it(m, outeri); it; ++it) {
        entries.emplace_back((int)it.row(), (int)it.col() + (int)numCols, 1.0);
      }
    }

    numCols += (int)m.cols();
  }

  SpMatD A(numRows, numCols);
  A.setFromTriplets(entries.begin(), entries.end());

  return A;
}

template<typename... T>
EigenSupport::SpMatD EigenSupport::ConcatRow(T &&...Ms)
{
  static_assert(
    std::is_same<
      std::integer_sequence<bool, true, std::is_same<std::decay_t<T>, SpMatD>::value...>,
      std::integer_sequence<bool, std::is_same<std::decay_t<T>, SpMatD>::value..., true>>::value,
    "T should be SpMatD");

  std::array<const SpMatD *, sizeof...(Ms)> matrices = { &Ms... };

  std::vector<TripletD> entries;
  Eigen::Index numRows = 0, numCols = 0;
  for (const SpMatD *mptr : matrices) {
    const SpMatD &m = *mptr;
    if (numCols == 0) {
      numCols = m.cols();
    }

    if (numCols != m.cols())
      throw std::domain_error("Different sparse matrix cols");

    for (Eigen::Index outeri = 0; outeri < m.outerSize(); outeri++) {
      for (SpMatD::InnerIterator it(m, outeri); it; ++it) {
        entries.emplace_back((int)it.row() + (int)numRows, (int)it.col(), 1.0);
      }
    }

    numRows += (int)m.rows();
  }

  SpMatD A(numRows, numCols);
  A.setFromTriplets(entries.begin(), entries.end());

  return A;
}

template<typename Iterator>
EigenSupport::SpMatD EigenSupport::ConcatRow(Iterator beg, Iterator end)
{
  std::vector<TripletD> entries;
  Eigen::Index numRows = 0, numCols = 0;
  for (Iterator it = beg; it != end; ++it) {
    const SpMatD &m = *it;
    if (numCols == 0) {
      numCols = m.cols();
    }

    if (numCols != m.cols())
      throw std::domain_error("Different sparse matrix cols");

    for (Eigen::Index outeri = 0; outeri < m.outerSize(); outeri++) {
      for (SpMatD::InnerIterator it(m, outeri); it; ++it) {
        entries.emplace_back((int)it.row() + (int)numRows, (int)it.col(), 1.0);
      }
    }

    numRows += (int)m.rows();
  }

  SpMatD A(numRows, numCols);
  A.setFromTriplets(entries.begin(), entries.end());

  return A;
}

template<>
inline void EigenSupport::Zero(EigenSupport::SpMatD &m)
{
  Eigen::Index nnz = m.nonZeros();
  memset(m.valuePtr(), 0, sizeof(double) * nnz);
}

template<typename MatrixType>
void EigenSupport::Zero(MatrixType &m)
{
  m.setZero();
  abort();
}

template<typename DerivedA, typename DerivedB, typename DerivedC>
void EigenSupport::TensorProduct(Eigen::MatrixBase<DerivedC> &ret, const Eigen::MatrixBase<DerivedA> &v1, const Eigen::MatrixBase<DerivedB> &v2)
{
  if (v1.rows() != v2.rows() || v1.cols() != 1 || v2.cols() != 1 ||
    ret.rows() != v1.rows() || ret.cols() != ret.rows())
    throw std::domain_error("Wrong size of the arguments");

  for (Eigen::Index c = 0; c < ret.cols(); c++) {
    for (Eigen::Index r = 0; r < ret.rows(); r++) {
      ret(r, c) = v1(r, 0) * v2(c, 0);
    }
  }
}

template<typename Derived>
int EigenSupport::readMatrix(const char *filename, Eigen::PlainObjectBase<Derived> &mat, int overwrite)
{
  std::ifstream infile(filename, std::ios_base::binary);
  if (!infile)
    return 1;

  int nRows, nCols, entrySize;
  if (!infile.read((char *)&nRows, sizeof(int))) {
    std::cerr << "Cannot read #rows." << std::endl;
    return 1;
  }

  if (!infile.read((char *)&nCols, sizeof(int))) {
    std::cerr << "Cannot read #cols." << std::endl;
    return 1;
  }

  if (!infile.read((char *)&entrySize, sizeof(int))) {
    std::cerr << "Cannot read entry size." << std::endl;
    return 1;
  }

  if (entrySize != int(sizeof(typename Eigen::PlainObjectBase<Derived>::Scalar))) {
    std::cerr << "entry size mis matched." << std::endl;
  }

  if (overwrite) {
    mat.resize(nRows, nCols);
    if (!infile.read((char *)mat.data(), sizeof(typename Eigen::PlainObjectBase<Derived>::Scalar) * mat.size())) {
      std::cerr << "Cannot read data" << std::endl;
      return 1;
    }

    return 0;
  }
  else {
    if (nRows != int(mat.rows()) || nCols != int(mat.cols())) {
      std::cerr << "#rows or #cols mismatched. File is (" << nRows << ',' << nCols << ") while input is (" << mat.rows() << ',' << mat.cols() << ")" << std::endl;
      return 1;
    }

    if (!infile.read((char *)mat.data(), sizeof(typename Eigen::PlainObjectBase<Derived>::Scalar) * mat.size())) {
      return 1;
    }

    return 0;
  }
}

template<typename Derived>
int EigenSupport::writeMatrix(const char *filename, const Eigen::PlainObjectBase<Derived> &mat)
{
  std::ofstream outfile(filename, std::ios_base::binary);
  if (!outfile)
    return 1;

  int nRows = int(mat.rows()), nCols = int(mat.cols());
  int entrySize = int(sizeof(typename Eigen::PlainObjectBase<Derived>::Scalar));
  if (!outfile.write((const char *)&nRows, sizeof(int))) {
    std::cerr << "Cannot write #rows." << std::endl;
    return 1;
  }

  if (!outfile.write((const char *)&nCols, sizeof(int))) {
    std::cerr << "Cannot write #cols." << std::endl;
    return 1;
  }

  if (!outfile.write((const char *)&entrySize, sizeof(int))) {
    std::cerr << "Cannot write #cols." << std::endl;
    return 1;
  }

  if (!outfile.write((char *)mat.data(), sizeof(typename Eigen::PlainObjectBase<Derived>::Scalar) * mat.size())) {
    return 1;
  }

  return 0;
}

}  // namespace VegaFEM

#endif
