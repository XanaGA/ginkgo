/*******************************<GINKGO LICENSE>******************************
Copyright (c) 2017-2023, the Ginkgo authors
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************<GINKGO LICENSE>*******************************/

#include "core/matrix/ell_kernels.hpp"


#include <array>


#include <omp.h>


#include <ginkgo/core/base/exception_helpers.hpp>
#include <ginkgo/core/base/math.hpp>
#include <ginkgo/core/matrix/csr.hpp>
#include <ginkgo/core/matrix/dense.hpp>


#include "accessor/reduced_row_major.hpp"
#include "core/base/mixed_precision_types.hpp"

#include <immintrin.h>


namespace gko {
namespace kernels {
namespace omp {
/**
 * @brief The ELL matrix format namespace.
 *
 * @ingroup ell
 */
namespace ell {


template <int num_rhs, typename InputValueType, typename MatrixValueType,
          typename OutputValueType, typename IndexType, typename OutFn>
void spmv_small_rhs(std::shared_ptr<const OmpExecutor> exec,
                    const matrix::Ell<MatrixValueType, IndexType>* a,
                    const matrix::Dense<InputValueType>* b,
                    matrix::Dense<OutputValueType>* c, OutFn out)
{
    GKO_ASSERT(b->get_size()[1] == num_rhs);
    using arithmetic_type =
        highest_precision<InputValueType, OutputValueType, MatrixValueType>;
    using a_accessor =
        gko::acc::reduced_row_major<1, arithmetic_type, const MatrixValueType>;
    using b_accessor =
        gko::acc::reduced_row_major<2, arithmetic_type, const InputValueType>;

    constexpr int vect_size = 4;
    const auto num_stored_elements_per_row =
        a->get_num_stored_elements_per_row();
    const auto stride = a->get_stride();
    const auto a_vals = gko::acc::range<a_accessor>(
        std::array<acc::size_type, 1>{
            static_cast<acc::size_type>(num_stored_elements_per_row * stride)},
        a->get_const_values());
    const auto b_vals = gko::acc::range<b_accessor>(
        std::array<acc::size_type, 2>{
            {static_cast<acc::size_type>(b->get_size()[0]),
             static_cast<acc::size_type>(b->get_size()[1])}},
        b->get_const_values(),
        std::array<acc::size_type, 1>{
            {static_cast<acc::size_type>(b->get_stride())}});
    const IndexType* __restrict col_ptr = a->get_const_col_idxs();
    // const IndexType* col_ptr = a->get_const_col_idxs();

#pragma omp parallel for
    for (size_type first_row = 0;
         first_row < a->get_size()[0] - (vect_size - 1);
         first_row += vect_size) {
        std::array<arithmetic_type, vect_size> values;
        IndexType cols[vect_size];
        std::array<arithmetic_type, vect_size * num_rhs> partial_sum;
        partial_sum.fill(zero<arithmetic_type>());

        for (size_type i = 0; i < num_stored_elements_per_row; i++) {
#pragma unroll
            for (size_type next = 0; next < vect_size; next++) {
                values[next] = a_vals((first_row + next) + i * stride);
                cols[next] = col_ptr[(first_row + next) + i * stride];
            }
#pragma unroll
            for (size_type next = 0; next < vect_size; next++) {
#pragma unroll
                for (size_type j = 0; j < num_rhs; j++) {
                    partial_sum[next * num_rhs + j] +=
                        values[next] * b_vals(cols[next], j);
                }
            }
        }

#pragma unroll
        for (size_type next = 0; next < vect_size; next++) {
#pragma unroll
            for (size_type j = 0; j < num_rhs; j++) {
                // std::cout << "First row: " << first_row << "\n";
                // std::cout << "Next row: " <<  next << "\n";
                // std::cout << "First + next: " << first_row + next << "\n";
                // std::cout << j << "\n";
                [&] {
                    c->at((first_row + next), j) = out(
                        first_row + next, j, partial_sum[next * num_rhs + j]);
                }();
                // std::cout << "-----------------------" << "\n" ;
            }
        }
    }


    size_type rest = a->get_size()[0] % vect_size;
    // std::cout << "Rest: " << rest << "\n";
    if (rest != 0) {
        size_type last = vect_size * (size_type)(a->get_size()[0] / vect_size);
        // std::cout << "Last: " <<  last << "\n";

        for (size_type row = last; row < a->get_size()[0]; row++) {
            std::array<arithmetic_type, num_rhs> partial_sum;
            partial_sum.fill(zero<arithmetic_type>());
            for (size_type i = 0; i < num_stored_elements_per_row; i++) {
                arithmetic_type val = a_vals(row + i * stride);
                auto col = col_ptr[row + i * stride];
#pragma unroll
                for (size_type j = 0; j < num_rhs; j++) {
                    partial_sum[j] += val * b_vals(col, j);
                }
            }
#pragma unroll
            for (size_type j = 0; j < num_rhs; j++) {
                [&] { c->at(row, j) = out(row, j, partial_sum[j]); }();
            }
        }
    }
}


template <int num_rhs>
void spmv_small_rhs_vect(std::shared_ptr<const OmpExecutor> exec,
                         const matrix::Ell<double, int>* a,
                         const matrix::Dense<double>* b,
                         matrix::Dense<double>* c)
{
    GKO_ASSERT(b->get_size()[1] == num_rhs);
    using arithmetic_type = highest_precision<double, double, double>;
    using a_accessor =
        gko::acc::reduced_row_major<1, arithmetic_type, const double>;
    using b_accessor =
        gko::acc::reduced_row_major<2, arithmetic_type, const double>;

    // num_rhs = 1;
    constexpr int vect_size = 8;
    const auto num_stored_elements_per_row =
        a->get_num_stored_elements_per_row();
    const auto stride = a->get_stride();
    const auto a_vals = a->get_const_values();
    const auto b_vals = b->get_const_values();
    const int* __restrict col_ptr = a->get_const_col_idxs();
    // const int* col_ptr = a->get_const_col_idxs();

#pragma omp parallel for
    for (size_type first_row = 0;
         first_row < a->get_size()[0] - (vect_size - 1);
         first_row += vect_size) {
        __m512d a_values_vect;

        int b_values[vect_size];
        __m512d b_values_vect;

        // std::cout << "Before zero fill" << "\n";
        __m512d partial_sum_vect;
        partial_sum_vect = _mm512_setzero_pd();

        double partial_sum[vect_size];

        // std::cout << "Before for" << "\n";
        for (size_type i = 0; i < num_stored_elements_per_row; i++) {
            // std::cout << "Before a_values" << "\n";
            a_values_vect = _mm512_loadu_pd(&a_vals[first_row + i * stride]);
            // std::cout << "Before b_values" << "\n";
            __m256i col_idxs_vect =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(
                    &col_ptr[first_row + i * stride]));
            // std::cout << "------------------------------ " << "\n";
            // for (size_type next = 0; next < vect_size; next++) {
            //     b_values_vect[next] =
            //         b_vals[col_ptr[first_row + next + i * stride]];
            // }
            b_values_vect =
                _mm512_i32gather_pd(col_idxs_vect, b_vals, sizeof(double));

            // for (size_type next = 0; next < vect_size; next++) {
            //     std::cout << "INDEX: " << (first_row+next) + i * stride <<
            //     "\n"; std::cout << "a_vals: " << a_vals[(first_row+next) + i
            //     * stride] << "\n";
            //     // a_values_vect[next] = a_vals[(first_row+next) + i *
            //     stride]; std::cout << "a_values_vect: " <<
            //     a_values_vect[next] << "\n";

            //     std::cout << "col_ptr[]: " << col_ptr[(first_row + next) + i
            //     * stride] << "\n";
            //     // b_values_vect[next] = b_values[col_ptr[(first_row + next)
            //     + i * stride]]; std::cout << "b_values_vect: " <<
            //     b_values_vect[next] << "\n";
            // }

            // a_values_vect = _mm512_set_pd(a_vals(first_row + i * stride),
            //                                 a_vals((first_row+1) + i *
            //                                 stride), a_vals((first_row+2) + i
            //                                 * stride), a_vals((first_row+3) +
            //                                 i * stride), a_vals((first_row+4)
            //                                 + i * stride),
            //                                 a_vals((first_row+5) + i *
            //                                 stride), a_vals((first_row+6) + i
            //                                 * stride), a_vals((first_row+7) +
            //                                 i * stride));

            // std::cout << "INDEX: " << first_row + i * stride << "\n";
            // b_values_vect = _mm512_set_pd(b_values[col_ptr[first_row + i *
            // stride]],
            //                                 b_values[col_ptr[(first_row + 1)
            //                                 + i * stride]],
            //                                 b_values[col_ptr[(first_row + 2)
            //                                 + i * stride]],
            //                                 b_values[col_ptr[(first_row + 3)
            //                                 + i * stride]],
            //                                 b_values[col_ptr[(first_row + 4)
            //                                 + i * stride]],
            //                                 b_values[col_ptr[(first_row + 5)
            //                                 + i * stride]],
            //                                 b_values[col_ptr[(first_row + 6)
            //                                 + i * stride]],
            //                                 b_values[col_ptr[(first_row + 7)
            //                                 + i * stride]]);

            partial_sum_vect =
                _mm512_fmadd_pd(a_values_vect, b_values_vect, partial_sum_vect);
            // std::cout << "-------------------------------\n";
            // std::cout << "a_values_vect["<< 7 <<"]: " << a_values_vect[7] <<
            // "\n"; std::cout << "b_values_vect["<< 7 <<"]: " <<
            // b_values_vect[7] << "\n"; std::cout << "partial_sum_vect["<< 0
            // <<"]: " << partial_sum_vect[0] << "\n";
            for (size_type next = 0; next < vect_size; next++) {
                // std::cout << "partial_sum_vect["<< next <<"]: " <<
                // partial_sum_vect[next] << "\n";
            }
        }

        // std::cout << "Before store" << "\n";
        // _mm512_storeu_pd(partial_sum, partial_sum_vect);

        // std::cout << "Before c_at" << "\n";
#pragma unroll
        for (size_type next = 0; next < vect_size; next++) {
            [&] { c->at((first_row + next), 0) = partial_sum_vect[next]; }();
        }
    }


    size_type rest = a->get_size()[0] % vect_size;
    // std::cout << "Rest: " << rest << "\n";
    if (rest != 0) {
        size_type last = vect_size * (size_type)(a->get_size()[0] / vect_size);
        // std::cout << "Last: " <<  last << "\n";

        for (size_type row = last; row < a->get_size()[0]; row++) {
            std::array<arithmetic_type, num_rhs> partial_sum;
            partial_sum.fill(zero<arithmetic_type>());
            for (size_type i = 0; i < num_stored_elements_per_row; i++) {
                arithmetic_type val = a_vals[row + i * stride];
                auto col = col_ptr[row + i * stride];
#pragma unroll
                for (size_type j = 0; j < num_rhs; j++) {
                    partial_sum[j] += val * b_vals[col];
                }
            }
#pragma unroll
            for (size_type j = 0; j < num_rhs; j++) {
                [&] { c->at(row, j) = partial_sum[j]; }();
            }
        }
    }
}


template <int block_size, typename InputValueType, typename MatrixValueType,
          typename OutputValueType, typename IndexType, typename OutFn>
void spmv_blocked(std::shared_ptr<const OmpExecutor> exec,
                  const matrix::Ell<MatrixValueType, IndexType>* a,
                  const matrix::Dense<InputValueType>* b,
                  matrix::Dense<OutputValueType>* c, OutFn out)
{
    GKO_ASSERT(b->get_size()[1] > block_size);
    using arithmetic_type =
        highest_precision<InputValueType, OutputValueType, MatrixValueType>;
    using a_accessor =
        gko::acc::reduced_row_major<1, arithmetic_type, const MatrixValueType>;
    using b_accessor =
        gko::acc::reduced_row_major<2, arithmetic_type, const InputValueType>;

    const auto num_stored_elements_per_row =
        a->get_num_stored_elements_per_row();
    const auto stride = a->get_stride();
    const auto a_vals = gko::acc::range<a_accessor>(
        std::array<acc::size_type, 1>{
            static_cast<acc::size_type>(num_stored_elements_per_row * stride)},
        a->get_const_values());
    const auto b_vals = gko::acc::range<b_accessor>(
        std::array<acc::size_type, 2>{
            {static_cast<acc::size_type>(b->get_size()[0]),
             static_cast<acc::size_type>(b->get_size()[1])}},
        b->get_const_values(),
        std::array<acc::size_type, 1>{
            {static_cast<acc::size_type>(b->get_stride())}});

    const auto num_rhs = b->get_size()[1];
    const auto rounded_rhs = num_rhs / block_size * block_size;

#pragma omp parallel for
    for (size_type row = 0; row < a->get_size()[0]; row++) {
        std::array<arithmetic_type, block_size> partial_sum;
        for (size_type rhs_base = 0; rhs_base < rounded_rhs;
             rhs_base += block_size) {
            partial_sum.fill(zero<arithmetic_type>());
            for (size_type i = 0; i < num_stored_elements_per_row; i++) {
                arithmetic_type val = a_vals(row + i * stride);
                auto col = a->col_at(row, i);
                if (col != invalid_index<IndexType>()) {
#pragma unroll
                    for (size_type j = 0; j < block_size; j++) {
                        partial_sum[j] += val * b_vals(col, j + rhs_base);
                    }
                }
            }
#pragma unroll
            for (size_type j = 0; j < block_size; j++) {
                const auto col = j + rhs_base;
                [&] { c->at(row, col) = out(row, col, partial_sum[j]); }();
            }
        }
        partial_sum.fill(zero<arithmetic_type>());
        for (size_type i = 0; i < num_stored_elements_per_row; i++) {
            arithmetic_type val = a_vals(row + i * stride);
            auto col = a->col_at(row, i);
            if (col != invalid_index<IndexType>()) {
                for (size_type j = rounded_rhs; j < num_rhs; j++) {
                    partial_sum[j - rounded_rhs] += val * b_vals(col, j);
                }
            }
        }
        for (size_type j = rounded_rhs; j < num_rhs; j++) {
            [&] {
                c->at(row, j) = out(row, j, partial_sum[j - rounded_rhs]);
            }();
        }
    }
}


template <typename InputValueType, typename MatrixValueType,
          typename OutputValueType, typename IndexType>
void spmv(std::shared_ptr<const OmpExecutor> exec,
          const matrix::Ell<MatrixValueType, IndexType>* a,
          const matrix::Dense<InputValueType>* b,
          matrix::Dense<OutputValueType>* c)
{
    const auto num_rhs = b->get_size()[1];
    if (num_rhs <= 0) {
        return;
    }
    auto out = [](auto, auto, auto value) { return value; };
    if (num_rhs == 1) {
        if (std::is_same<InputValueType, double>::value &&
            std::is_same<MatrixValueType, double>::value &&
            std::is_same<OutputValueType, double>::value &&
            std::is_same<IndexType, int>::value) {
            spmv_small_rhs_vect<1>(
                exec, reinterpret_cast<const matrix::Ell<double, int>*>(a),
                reinterpret_cast<const matrix::Dense<double>*>(b),
                reinterpret_cast<matrix::Dense<double>*>(c));
        } else {
            spmv_small_rhs<1>(exec, a, b, c, out);
        }
        return;
    }
    if (num_rhs == 2) {
        spmv_small_rhs<2>(exec, a, b, c, out);
        return;
    }
    if (num_rhs == 3) {
        spmv_small_rhs<3>(exec, a, b, c, out);
        return;
    }
    if (num_rhs == 4) {
        spmv_small_rhs<4>(exec, a, b, c, out);
        return;
    }
    spmv_blocked<4>(exec, a, b, c, out);
}

GKO_INSTANTIATE_FOR_EACH_MIXED_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_ELL_SPMV_KERNEL);


template <typename InputValueType, typename MatrixValueType,
          typename OutputValueType, typename IndexType>
void advanced_spmv(std::shared_ptr<const OmpExecutor> exec,
                   const matrix::Dense<MatrixValueType>* alpha,
                   const matrix::Ell<MatrixValueType, IndexType>* a,
                   const matrix::Dense<InputValueType>* b,
                   const matrix::Dense<OutputValueType>* beta,
                   matrix::Dense<OutputValueType>* c)
{
    using arithmetic_type =
        highest_precision<InputValueType, OutputValueType, MatrixValueType>;
    const auto num_rhs = b->get_size()[1];
    if (num_rhs <= 0) {
        return;
    }
    const auto alpha_val = arithmetic_type{alpha->at(0, 0)};
    const auto beta_val = arithmetic_type{beta->at(0, 0)};
    auto out = [&](auto i, auto j, auto value) {
        return alpha_val * value + beta_val * arithmetic_type{c->at(i, j)};
    };
    if (num_rhs == 1) {
        spmv_small_rhs<1>(exec, a, b, c, out);
        return;
    }
    if (num_rhs == 2) {
        spmv_small_rhs<2>(exec, a, b, c, out);
        return;
    }
    if (num_rhs == 3) {
        spmv_small_rhs<3>(exec, a, b, c, out);
        return;
    }
    if (num_rhs == 4) {
        spmv_small_rhs<4>(exec, a, b, c, out);
        return;
    }
    spmv_blocked<4>(exec, a, b, c, out);
}

GKO_INSTANTIATE_FOR_EACH_MIXED_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_ELL_ADVANCED_SPMV_KERNEL);


}  // namespace ell
}  // namespace omp
}  // namespace kernels
}  // namespace gko
