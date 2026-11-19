// Tao Du
// taodu@csail.mit.edu
// Sept 22, 2016
#pragma once
#include "material.hpp"

namespace materials {

    template<int dim, typename T>
    class LinearElasticityMaterial : public Material<dim, T> {
    public:
        using xyz = typename Material<dim, T>::MatrixDimT;

        LinearElasticityMaterial(const T young_modulus,
                                 const T poisson_ratio) : Material<dim, T>(young_modulus, poisson_ratio) {}

        LinearElasticityMaterial(const LinearElasticityMaterial<dim, T>& material)  : Material<dim, T>(material) {}

        ~LinearElasticityMaterial() {}


        const T EnergyDensity(const typename Material<dim, T>::MatrixDimT& F) const {
            const typename Material<dim, T>::MatrixDimT small_strain_tensor =
                    0.5 * (F + F.transpose()) - Material<dim, T>::MatrixDimT::Identity();
            const T trace = small_strain_tensor.trace();
            return Material<dim, T>::mu() * small_strain_tensor.array().square().sum()
                   + Material<dim, T>::lambda() / 2.0 * trace * trace;
        }

        const typename Material<dim, T>::MatrixDimT StressTensor(
                const typename Material<dim, T>::MatrixDimT& F) const {
            const typename Material<dim, T>::MatrixDimT I = Material<dim, T>::MatrixDimT::Identity();
            return Material<dim, T>::mu() * (F + F.transpose() - 2 * I) +
                   Material<dim, T>::lambda() * (F.trace() - dim) * I;
        }

        // given dF, compute dP
        const typename Material<dim, T>::MatrixDimT StressDifferential(
                const typename Material<dim, T>::MatrixDimT& F,
                const typename Material<dim, T>::MatrixDimT& dF) const {

            const typename Material<dim, T>::MatrixDimT I = Material<dim, T>::MatrixDimT::Identity();
            return Material<dim, T>::mu() * (dF + dF.transpose()) +
                   Material<dim, T>::lambda() * dF.trace() * I;

        };

         // compute the dPdF for linear material
         // Hint:
         //     1. even though we pass in a F in the arguments list,it’s not actually needed for linear materials here.
         //     2. output P is a 3×3 matrix in 3 dimensions. F is also a 3×3matrix. There are 81 partial derivatives. Thus the output is a 9x9 matrix.
        const typename Material<dim, T>::MatrixDim2T StressDifferential(
                const typename Material<dim, T>::MatrixDimT& F) const {

            const T mu = Material<dim, T>::mu();
            const T lambda = Material<dim, T>::lambda();
            typename Material<dim, T>::MatrixDim2T K =
                    Material<dim, T>::MatrixDim2T::Identity() * mu;
            for (int i = 0; i < dim; ++i) {
                for (int j = 0; j < dim; ++j) {
                    K(i + dim * j, j + dim * i) += mu;
                    K((dim + 1) * i, (dim + 1) * j) += lambda;
                }
            }
            return K;

        }

    private:
        LinearElasticityMaterial<dim, T>& operator=(
                const LinearElasticityMaterial<dim, T>&);



    };

}