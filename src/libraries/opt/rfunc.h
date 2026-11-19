#pragma once

#include "EigenSupport.h"
#include "triMeshGeo.h"
#include "boundingVolumeTree.h"

#include <memory>

namespace MedialAxisRepresentation
{
class RFunction
{
public:
  virtual double func(const double *x) const = 0;
  virtual void gradient(const double *x, double *grad) const = 0;
  virtual int getNumDOFs() const = 0;
};

class VertexRFunction : public RFunction
{
public:
  VertexRFunction(const TriMeshGeo &mesh);

  // x = [x, p, r]
  virtual double func(const double *x) const override;
  virtual void gradient(const double *x, double *grad) const override;
  virtual int getNumDOFs() const override { return meshRest.numVertices() + 6; }

protected:
  TriMeshGeo meshRest;
};

class EdgeRFunction : public RFunction
{
public:
  EdgeRFunction(const TriMeshGeo &mesh);

  // x = [x, p, r]
  virtual double func(const double *x) const override;
  virtual void gradient(const double *x, double *grad) const override;
  virtual int getNumDOFs() const override { return meshRest.numVertices() + 9; }

protected:
  TriMeshGeo meshRest, meshCur;
};

class UnionRFunction : public RFunction
{
public:
  UnionRFunction(std::shared_ptr<const RFunction> f1, std::shared_ptr<const RFunction> f2);

  virtual double func(const double *x) const override;
  virtual void gradient(const double *x, double *grad) const override;
  virtual int getNumDOFs() const override { return f1->getNumDOFs() + f2->getNumDOFs(); }

protected:
  std::shared_ptr<const RFunction> f1, f2;
};

}  // namespace MedialAxisRepresentation