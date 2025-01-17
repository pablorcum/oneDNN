/*******************************************************************************
* Copyright 2016-2021 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef CPU_REORDER_SIMPLE_REORDER_HPP
#define CPU_REORDER_SIMPLE_REORDER_HPP

#include <assert.h>

#include "common/bfloat16.hpp"
#include "common/c_types_map.hpp"
#include "common/dnnl_thread.hpp"
#include "common/math_utils.hpp"
#include "common/primitive.hpp"
#include "common/primitive_attr.hpp"
#include "common/tag_traits.hpp"
#include "common/type_helpers.hpp"
#include "common/utils.hpp"

#include "cpu/cpu_primitive.hpp"
#include "cpu/reorder/cpu_reorder_pd.hpp"

#include "cpu/simple_q10n.hpp"

namespace dnnl {
namespace impl {
namespace cpu {

using bd = block_dim_t;
using ib = inner_blk_t;

template <impl::data_type_t type>
using data_t = typename prec_traits<type>::type;

template <impl::data_type_t type_i, impl::data_type_t type_o>
using _qz_a1b0 = qz_a1b0<data_t<type_i>, data_t<type_o>>;

template <impl::data_type_t type_i, impl::data_type_t type_o>
using _qz = qz<data_t<type_i>, data_t<type_o>>;

namespace fmt_order {
const bool keep = true;
const bool reverse = false;
const bool any = keep;
} // namespace fmt_order

namespace spec {
struct direct_copy {};
struct direct_copy_except_dim_0 {};
struct reference {};
} // namespace spec

#define SIMPLE_REORDER_TEMPL_DECL \
    impl::data_type_t type_i, impl::format_tag_t tag_i, \
            impl::data_type_t type_o, impl::format_tag_t tag_o, \
            bool order_keep
#define SIMPLE_REORDER_TEMPL_CALL type_i, tag_i, type_o, tag_o, order_keep

#define DECLARE_COMMON_PARAMS() \
    auto input = CTX_IN_MEM(const data_t<type_i> *, DNNL_ARG_FROM); \
    auto output = CTX_OUT_MEM(data_t<type_o> *, DNNL_ARG_TO); \
    const auto &scratchpad = ctx.get_scratchpad_grantor(); \
    MAYBE_UNUSED(scratchpad); \
    const auto input_d = ctx.memory_mdw(DNNL_ARG_FROM, pd->src_md()); \
    const auto output_d = ctx.memory_mdw(DNNL_ARG_TO, pd->dst_md()); \
    const float alpha = pd->alpha(); \
    MAYBE_UNUSED(alpha); \
    const float beta = pd->beta(); \
    MAYBE_UNUSED(beta);

#define GET_SCRATCHPAD_SIZE_ZERO() \
    static size_t get_scratchpad_size(const memory_desc_wrapper &input_d, \
            const memory_desc_wrapper &output_d) { \
        return 0; \
    }

/* specific reorders: common template */
template <SIMPLE_REORDER_TEMPL_DECL, typename spec = void>
struct simple_reorder_impl {};

namespace {
inline bool simple_fmt_check(bool order_keep, impl::format_tag_t tag_i,
        impl::format_tag_t tag_o, const memory_desc_wrapper &input_d,
        const memory_desc_wrapper &output_d) {
    if (input_d.has_runtime_dims_or_strides()) return false;
    return input_d.matches_tag(order_keep ? tag_i : tag_o)
            && output_d.matches_tag(order_keep ? tag_o : tag_i);
}
inline bool simple_po_check(const primitive_attr_t *attr) {
    const auto &po = attr->post_ops_;
    return po.len() == 0 || (po.len() == 1 && po.entry_[0].is_sum(false));
}
inline bool simple_attr_check(const primitive_attr_t *attr,
        bool many_scales_support, bool sum_support) {
    using smask_t = primitive_attr_t::skip_mask_t;
    smask_t skip_mask = smask_t::oscale;
    if (sum_support) skip_mask = skip_mask | smask_t::post_ops;
    if (!attr->has_default_values(skip_mask)) return false;
    if (!attr->defined()) return false;
    if (sum_support) simple_po_check(attr);
    if (many_scales_support) return true;
    return attr->output_scales_.mask_ == 0;
}
} // namespace

/* generic and direct-copy reorders */

template <SIMPLE_REORDER_TEMPL_DECL>
struct simple_reorder_impl<SIMPLE_REORDER_TEMPL_CALL,
        typename utils::enable_if<tag_i == format_tag::any
                        && tag_o == format_tag::any
                        && order_keep == fmt_order::any,
                spec::direct_copy>::type> {
    static bool is_applicable(const memory_desc_wrapper &input_d,
            const memory_desc_wrapper &output_d, const primitive_attr_t *attr) {
        /* FIXME: is the formula correct? */
        return !input_d.has_runtime_dims_or_strides()
                && input_d.similar_to(output_d, true, false, 0)
                && input_d.is_dense() && output_d.is_dense()
                && simple_attr_check(attr, false, true);
    }

    GET_SCRATCHPAD_SIZE_ZERO();

    static status_t execute(const cpu_reorder_pd_t *pd, const exec_ctx_t &ctx) {
        DECLARE_COMMON_PARAMS();

        assert(input_d.is_dense());

        input += input_d.blk_off(0);
        output += output_d.blk_off(0);

        const size_t nelems = input_d.nelems();

        constexpr int block_size = 16;
        const auto num_blocks = nelems / block_size;
        const auto rem_elems = nelems % block_size;

        parallel(0, [&](const int ithr, const int nthr) {
            size_t start {0}, end {0};
            balance211(num_blocks, nthr, ithr, start, end);
            start = start * block_size;
            end = end * block_size;

            if (alpha == 1.0 && beta == 0.0) {
                PRAGMA_OMP_SIMD()
                for (size_t e = start; e < end; ++e) {
                    output[e] = qz_a1b0<data_t<type_i>, data_t<type_o>>()(
                            input[e]);
                }
            } else if (alpha == 1.0) {
                PRAGMA_OMP_SIMD()
                for (size_t e = start; e < end; ++e) {
                    output[e] = qz_a1<data_t<type_i>, data_t<type_o>>()(
                            input[e], output[e], beta);
                }
            } else if (beta == 0.0) {
                PRAGMA_OMP_SIMD()
                for (size_t e = start; e < end; ++e) {
                    output[e] = qz_b0<data_t<type_i>, data_t<type_o>>()(
                            input[e], alpha);
                }
            } else {
                PRAGMA_OMP_SIMD()
                for (size_t e = start; e < end; ++e) {
                    output[e] = qz<data_t<type_i>, data_t<type_o>>()(
                            input[e], output[e], alpha, beta);
                }
            }

            if (rem_elems != 0 && ithr == nthr - 1) {
                if (alpha == 1.0 && beta == 0.0) {
                    PRAGMA_OMP_SIMD()
                    for (size_t e = nelems - rem_elems; e < nelems; ++e) {
                        output[e] = qz_a1b0<data_t<type_i>, data_t<type_o>>()(
                                input[e]);
                    }
                } else if (alpha == 1.0) {
                    PRAGMA_OMP_SIMD()
                    for (size_t e = nelems - rem_elems; e < nelems; ++e) {
                        output[e] = qz_a1<data_t<type_i>, data_t<type_o>>()(
                                input[e], output[e], beta);
                    }
                } else if (beta == 0.0) {
                    PRAGMA_OMP_SIMD()
                    for (size_t e = nelems - rem_elems; e < nelems; ++e) {
                        output[e] = qz_b0<data_t<type_i>, data_t<type_o>>()(
                                input[e], alpha);
                    }
                } else {
                    PRAGMA_OMP_SIMD()
                    for (size_t e = nelems - rem_elems; e < nelems; ++e) {
                        output[e] = qz<data_t<type_i>, data_t<type_o>>()(
                                input[e], output[e], alpha, beta);
                    }
                }
            }
        });
        return status::success;
    }
};

template <SIMPLE_REORDER_TEMPL_DECL>
struct simple_reorder_impl<SIMPLE_REORDER_TEMPL_CALL,
        typename utils::enable_if<tag_i == format_tag::any
                        && tag_o == format_tag::any
                        && order_keep == fmt_order::any,
                spec::direct_copy_except_dim_0>::type> {
    static bool is_applicable(const memory_desc_wrapper &input_d,
            const memory_desc_wrapper &output_d, const primitive_attr_t *attr) {
        auto is_dense_no_0 = [](const memory_desc_wrapper &data_d) {
            return nelems_no_dim_0(data_d) == _size_no_dim_0(data_d);
        };
        /* FIXME: is the formula correct? */
        return !input_d.has_runtime_dims_or_strides()
                && input_d.similar_to(output_d, true, false, 1)
                && is_dense_no_0(input_d) && is_dense_no_0(output_d)
                && simple_attr_check(attr, false, true);
    }

    GET_SCRATCHPAD_SIZE_ZERO();

    static status_t execute(const cpu_reorder_pd_t *pd, const exec_ctx_t &ctx) {
        DECLARE_COMMON_PARAMS();
        using namespace utils;

        input += input_d.blk_off(0);
        output += output_d.blk_off(0);

        const int N = input_d.dims()[0];
        const dim_t is = input_d.blocking_desc().strides[0];
        const dim_t os = output_d.blocking_desc().strides[0];
        const dim_t nelems_no_d0 = nelems_no_dim_0(input_d);
        const dim_t work_amount = N * nelems_no_d0;

        if (alpha == 1.0 && beta == 0.0) {
            parallel(0, [&](const int ithr, const int nthr) {
                dim_t n {0}, dim1_s {0};
                dim_t start {0}, end {0};
                balance211(work_amount, nthr, ithr, start, end);
                nd_iterator_init(start, n, N, dim1_s, nelems_no_d0);
                while (start < end) {
                    dim_t work_rem = end - start;
                    dim_t dim1_e = dim1_s + work_rem > nelems_no_d0
                            ? nelems_no_d0
                            : dim1_s + work_rem;
                    PRAGMA_OMP_SIMD()
                    for (dim_t e = dim1_s; e < dim1_e; ++e) {
                        output[os * n + e]
                                = _qz_a1b0<type_i, type_o>()(input[is * n + e]);
                    }
                    nd_iterator_jump(start, end, n, N, dim1_s, nelems_no_d0);
                }
            });
        } else {
            parallel(0, [&](const int ithr, const int nthr) {
                dim_t n {0}, dim1_s {0};
                dim_t start {0}, end {0};
                balance211(work_amount, nthr, ithr, start, end);
                nd_iterator_init(start, n, N, dim1_s, nelems_no_d0);
                while (start < end) {
                    dim_t work_rem = end - start;
                    dim_t dim1_e = dim1_s + work_rem > nelems_no_d0
                            ? nelems_no_d0
                            : dim1_s + work_rem;
                    PRAGMA_OMP_SIMD()
                    for (dim_t e = dim1_s; e < dim1_e; ++e) {
                        output[os * n + e]
                                = _qz<type_i, type_o>()(input[is * n + e],
                                        output[os * n + e], alpha, beta);
                    }
                    nd_iterator_jump(start, end, n, N, dim1_s, nelems_no_d0);
                }
            });
        }

        return status::success;
    }

private:
    static dim_t nelems_no_dim_0(const memory_desc_wrapper &data_d) {
        const int ndims = data_d.ndims();
        if (ndims <= 1) return 1;
        return utils::array_product(data_d.dims() + 1, data_d.ndims() - 1);
    }

    static dim_t _size_no_dim_0(const memory_desc_wrapper &data_d) {
        dims_t blocks;
        data_d.compute_blocks(blocks);

        const auto &blk = data_d.blocking_desc();

        dim_t blk_size = 1;
        for (int iblk = 0; iblk < blk.inner_nblks; ++iblk)
            blk_size *= blk.inner_blks[iblk];

        dim_t max_size = blk_size;
        for (int d = 1; d < data_d.ndims(); ++d) {
            max_size = nstl::max(max_size,
                    data_d.padded_dims()[d] / blocks[d] * blk.strides[d]);
        }

        return max_size;
    }
};

template <SIMPLE_REORDER_TEMPL_DECL>
struct simple_reorder_impl<SIMPLE_REORDER_TEMPL_CALL,
        typename utils::enable_if<tag_i == format_tag::any
                        && tag_o == format_tag::any
                        && order_keep == fmt_order::any,
                spec::reference>::type> {
    static bool is_applicable(const memory_desc_wrapper &input_d,
            const memory_desc_wrapper &output_d, const primitive_attr_t *attr) {
        /* supported smask: 0x0...011..10...0,
         * i.e. 1 should be contiguous */
        int smask = attr ? attr->output_scales_.mask_ : 0;
        for (; smask > 0 && !(smask & 0x1); smask >>= 1)
            ;
        for (; smask > 0 && smask & 0x1; smask >>= 1)
            ;
        return input_d.is_blocking_desc() && output_d.is_blocking_desc()
                && !output_d.is_additional_buffer()
                && !input_d.is_additional_buffer() && smask == 0
                && attr->has_default_values(
                        dnnl_primitive_attr::skip_mask_t::oscale_runtime
                        | dnnl_primitive_attr::skip_mask_t::zero_points_runtime
                        | dnnl_primitive_attr::skip_mask_t::post_ops)
                && simple_po_check(attr);
    }

    GET_SCRATCHPAD_SIZE_ZERO();

    static status_t execute(
            const cpu_reorder_pd_t *pd_object, const exec_ctx_t &ctx) {
        // DEFINE_SCALES_BUFFER and DEFINE_ZERO_POINT_VALUE macro use pd() to
        // query properties, hence wrapping the primitive descriptor into a
        // function.
        auto pd = [pd_object]() { return pd_object; };

        auto input = CTX_IN_MEM(const data_t<type_i> *, DNNL_ARG_FROM);
        auto output = CTX_OUT_MEM(data_t<type_o> *, DNNL_ARG_TO);

        const float beta = pd()->beta();
        DEFINE_SCALES_BUFFER(scales);
        DEFINE_ZERO_POINT_VALUE(i0, DNNL_ARG_FROM);
        DEFINE_ZERO_POINT_VALUE(o0, DNNL_ARG_TO);

        const auto input_d = ctx.memory_mdw(DNNL_ARG_FROM, pd()->src_md());
        const auto output_d = ctx.memory_mdw(DNNL_ARG_TO, pd()->dst_md());

        const size_t nelems = input_d.nelems();

        // This kernel is used also for tensors with multiple inner
        // blocks for which generic zero padding must be used.
        // TODO: apply zero padding inside parallel_nd()
        ctx.zero_pad_output(DNNL_ARG_TO);

        int ndims_start = 0, ndims_mask = 0;
        int smask = pd()->attr()->output_scales_.mask_;
        for (; smask > 0 && !(smask & 0x1); smask >>= 1)
            ++ndims_start;
        for (; smask > 0 && smask & 0x1; smask >>= 1)
            ++ndims_mask;
        assert(smask == 0);

        const ptrdiff_t D_start
                = utils::array_product(input_d.dims(), ndims_start);
        const ptrdiff_t D_mask = utils::array_product(
                input_d.dims() + ndims_start, ndims_mask);
        const ptrdiff_t D_rest = nelems / D_start / D_mask;

        parallel_nd(D_start, D_mask, D_rest,
                [&](ptrdiff_t ds, ptrdiff_t dm, ptrdiff_t dr) {
                    const float scale = scales[dm];

                    const size_t e = (ds * D_mask + dm) * D_rest + dr;
                    const auto &i = input[input_d.off_l(e)];
                    auto &o = output[output_d.off_l(e)];

                    float f = scale * ((float)i - i0) + o0;
                    o = _qz<data_type::f32, type_o>()(f, o, 1.f, beta);
                });

        return status::success;
    }
};

/* high level class declaration */

template <SIMPLE_REORDER_TEMPL_DECL, typename spec = void>
struct simple_reorder_t : public primitive_t {
    struct pd_t : public cpu_reorder_pd_t {
        using cpu_reorder_pd_t::cpu_reorder_pd_t;

        DECLARE_COMMON_PD_T("simple:any", simple_reorder_t);

    private:
        static status_t create(reorder_pd_t **reorder_pd, engine_t *engine,
                const primitive_attr_t *attr, engine_t *src_engine,
                const memory_desc_t *src_md, engine_t *dst_engine,
                const memory_desc_t *dst_md) {
            bool args_ok = true && src_md->data_type == type_i
                    && dst_md->data_type == type_o
                    && attr->has_default_values(
                            dnnl_primitive_attr::skip_mask_t::oscale_runtime
                            | dnnl_primitive_attr::skip_mask_t::zero_points
                            | dnnl_primitive_attr::skip_mask_t::
                                    zero_points_runtime
                            | dnnl_primitive_attr::skip_mask_t::post_ops)
                    && simple_reorder_impl<SIMPLE_REORDER_TEMPL_CALL,
                            spec>::is_applicable(src_md, dst_md, attr);
            if (!args_ok) return status::invalid_arguments;

            auto _pd = new pd_t(attr, src_engine->kind(), src_md,
                    dst_engine->kind(), dst_md);
            if (_pd == nullptr) return status::out_of_memory;
            if (_pd->init(engine, src_engine, dst_engine) != status::success) {
                delete _pd;
                return status::unimplemented;
            }

            const size_t scratchpad_sz_
                    = simple_reorder_impl<SIMPLE_REORDER_TEMPL_CALL,
                            spec>::get_scratchpad_size(src_md, dst_md);
            auto scratchpad = _pd->scratchpad_registry().registrar();
            scratchpad.book(memory_tracking::names::key_reorder_space,
                    scratchpad_sz_, 1, 16);
            _pd->init_scratchpad_md();
            return safe_ptr_assign(*reorder_pd, _pd);
        }
        friend dnnl::impl::impl_list_item_t;
    };

    simple_reorder_t(const pd_t *apd) : primitive_t(apd) {}

    status_t execute(const exec_ctx_t &ctx) const override {
        return simple_reorder_impl<SIMPLE_REORDER_TEMPL_CALL, spec>::execute(
                pd(), ctx);
    }

private:
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }
};

#undef SIMPLE_REORDER_TEMPL_DECL
#undef SIMPLE_REORDER_TEMPL_CALL

} // namespace cpu
} // namespace impl
} // namespace dnnl

#endif

// vim: et ts=4 sw=4 cindent cino+=l0,\:4,N-s
