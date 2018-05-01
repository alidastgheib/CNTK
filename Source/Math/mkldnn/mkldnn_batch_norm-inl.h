/*******************************************************************************
 * Copyright 2016 Intel Corporation
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
 *
 * \file mkl_batch_norm-inl.h
 * \brief
 * \author lingyan.guo@intel.com
 *         zhenlin.luo@intel.com
 *
 *******************************************************************************/
#ifndef CNTK_OPERATOR_MKL_DNN_MKLDNN_BATCH_NORM_INL_H_
#define CNTK_OPERATOR_MKL_DNN_MKLDNN_BATCH_NORM_INL_H_

#include <string>
#include <algorithm>
#include <vector>
#include <iostream>
#include <math.h>
#include "mkl_memory.h"
#include "mkldnn_memory-inl.h"
#include "mkl_conv-common-inl.h"
#include "mkldnn_base-inl.h"

#pragma warning(push)
#pragma warning(disable : 4244) // possible loss of data
#ifdef USE_MKLDNN
#define MKL_DNN_BN_MIN_EPSILON (1e-5)
namespace Microsoft
{
namespace MSR
{
namespace CNTK
{

template <typename DType>
class MKLDNNBatchNormOp : public MKLDNNLayer<DType>
{
    static int s_id_gen;
    int m_id;

public:
    using Mat = Matrix<DType>;
    explicit MKLDNNBatchNormOp(TensorShape inOutT, ImageLayoutKind imageLayout, bool relu = false)
        : MKLDNNLayer<DType>(),
          fwd_top_data(NULL),
          fwd_bottom_data(NULL),
          fwd_inference_pd(NULL),
          fwd_training_pd(NULL),
          bwd_top_diff(NULL),
          bwd_bottom_diff(NULL),
          bwd_scaleshift_pd(NULL),
          m_inOutT(inOutT),
          m_imageLayout(imageLayout),
          m_eps_(MKL_DNN_BN_MIN_EPSILON),
          accu_grad(-1)
    {
        m_id = s_id_gen++;
        m_relu = relu;
    }
    virtual ~MKLDNNBatchNormOp() {}
    std::string getName()
    {
        std::string name = "MKLDNNBatchNormOp_";
        name = name + std::to_string(m_id);
        return name;
    }

private:
    void LayerSetUp(int batchsize, double epsilon = MKL_DNN_BN_MIN_EPSILON)
    {
        ImageDimensions inT(m_inOutT, m_imageLayout);
        m_eps_ = max(epsilon, MKL_DNN_BN_MIN_EPSILON);
        m_channels_ = (int) inT.c();
        m_height_ = (int) inT.h();
        m_width_ = (int) inT.w();
        m_num_ = batchsize;
        int32_t n = this->m_num_;
        int32_t iw = this->m_width_;
        int32_t ih = this->m_height_;
        int32_t ic = this->m_channels_;
        mkldnn::memory::data_type mpcsn = mkldnn::memory::data_type::f32;
        mkldnn::engine cpu_engine = CpuEngine::Instance().get_engine();
        fwd_usr_input_md.reset(new mkldnn::memory::desc({{n, ic, ih, iw}}, mpcsn, mkldnn::memory::format::nchw));
        fwd_usr_mpd.reset(new mkldnn::memory::primitive_desc(*fwd_usr_input_md, cpu_engine));
    }
    void initFwd(const Mat& in)
    {
        void* bottom_data = const_cast<DType*>(mkl_prv_data<DType>(in));
        // ---- Initialize memory descriptors -------------
        std::shared_ptr<mkldnn::memory::desc> input_md, scaleshift_md;
        std::shared_ptr<mkldnn::memory::primitive_desc> usr_mpd(NULL), prv_mpd(NULL);
        if (bottom_data != nullptr)
        {
            std::shared_ptr<MKLDNNData<DType>> mem_descr = get_mkldnn_prv_descriptor<DType>(in);
            assert(mem_descr != NULL);
            fwd_bottom_data = mem_descr;
            input_md.reset(new mkldnn::memory::desc(mem_descr->prv_memory_pd()->desc()));
            usr_mpd = mem_descr->usr_memory_pd();
            prv_mpd = mem_descr->prv_memory_pd();
        }
        else
        {
            input_md = fwd_usr_input_md;
            usr_mpd = fwd_usr_mpd;
            fwd_bottom_data.reset(new MKLDNNData<DType>(usr_mpd, prv_mpd));
            fwd_bottom_data->name = "fwd_bottom_data   @ " + this->getName();
        }

        mkldnn::engine cpu_engine = CpuEngine::Instance().get_engine();
        // ---- Initialize BatchNorm primitive descriptor -------------
        mkldnn::batch_normalization_forward::desc BatchNormFwdInference_desc(
            mkldnn::prop_kind::forward_scoring, *input_md, m_eps_,
            mkldnn::batch_normalization_flag::use_global_stats | mkldnn::batch_normalization_flag::use_scale_shift);
        mkldnn::batch_normalization_forward::desc BatchNormFwdTraining_desc(
            mkldnn::prop_kind::forward_training, *input_md, m_eps_, mkldnn::batch_normalization_flag::use_scale_shift);

        if (m_relu)
        {
            attr_t attr = attr_t(mkldnn::round_mode::round_nearest, 1.0, attr_t::scale_t::policy_t::COMMON);
            attr.pops.entry[0].kind = attr_t::post_ops_t::kind_t::RELU;
            attr.pops.entry[0].eltwise.alpha = 0.0;
            attr.pops.entry[0].eltwise.beta = 0.0;
            attr.pops.entry[0].eltwise.scale = 1.0;
            attr.pops.len = 1;
            attr.mkldnn_attr_create();
            fwd_inference_pd.reset(
                new mkldnn::batch_normalization_forward::primitive_desc(BatchNormFwdInference_desc, attr.mkldnn_attr, cpu_engine));
            fwd_training_pd.reset(
                new mkldnn::batch_normalization_forward::primitive_desc(BatchNormFwdTraining_desc, attr.mkldnn_attr, cpu_engine));
        }
        else
        {
            fwd_inference_pd.reset(
            new mkldnn::batch_normalization_forward::primitive_desc(BatchNormFwdInference_desc, cpu_engine));
            fwd_training_pd.reset(
                new mkldnn::batch_normalization_forward::primitive_desc(BatchNormFwdTraining_desc, cpu_engine));
        }

        fwd_top_data.reset(new MKLDNNData<DType>(usr_mpd, prv_mpd));
        fwd_top_data->name = "fwd_top_data   @ " + this->getName();
        weight_memory.reset(new mkldnn::memory(fwd_inference_pd->weights_primitive_desc()));
    }
    void InitBatchNormBwd(const Mat& in)
    {
        int32_t n = this->m_num_;
        int32_t w = this->m_width_;
        int32_t h = this->m_height_;
        int32_t c = this->m_channels_;

        void* src_data = const_cast<DType*>(mkl_prv_data<DType>(in));
        bool src_is_prv = (src_data != NULL);
        mkldnn::engine cpu_engine = CpuEngine::Instance().get_engine();
        mkldnn::memory::data_type mpcsn = mkldnn::memory::data_type::f32;
        // ---- Initialize memory descriptors -------------

        std::shared_ptr<mkldnn::memory::desc> top_diff_md, top_data_md;
        std::shared_ptr<mkldnn::memory::primitive_desc> usr_diff_mpd(NULL), prv_diff_mpd(NULL);
        if (src_is_prv)
        {
            std::shared_ptr<MKLDNNMemoryDescriptor<DType>> mem_descr = get_mkldnn_prv_descriptor<DType>(in);
            top_diff_md.reset(new mkldnn::memory::desc(mem_descr->prv_memory_pd()->desc()));
            usr_diff_mpd = mem_descr->usr_memory_pd();
            prv_diff_mpd = mem_descr->prv_memory_pd();
        }
        else
        {
            top_diff_md.reset(new mkldnn::memory::desc({{n, c, h, w}}, mpcsn, mkldnn::memory::format::nchw));
            usr_diff_mpd.reset(new mkldnn::memory::primitive_desc(*top_diff_md, cpu_engine));
        }
        shared_ptr<mkldnn::memory::desc> output_stats_md;
        if (fwd_output_memory != NULL)
            output_stats_md.reset(new mkldnn::memory::desc(fwd_output_memory->get_primitive_desc().desc()));
        else
            output_stats_md.reset(new mkldnn::memory::desc(*fwd_usr_input_md));
        mkldnn::batch_normalization_backward::desc BatchNormBwd_desc(mkldnn::prop_kind::backward, *top_diff_md,
                                                                     *output_stats_md, m_eps_,
                                                                     mkldnn::batch_normalization_flag::use_scale_shift);
        bwd_scaleshift_pd.reset(
            new mkldnn::batch_normalization_backward::primitive_desc(BatchNormBwd_desc, cpu_engine, *fwd_training_pd));

        diff_weight_memory.reset(new mkldnn::memory(bwd_scaleshift_pd->diff_weights_primitive_desc()));

        bwd_bottom_diff.reset(new MKLDNNData<DType>(usr_diff_mpd, prv_diff_mpd));
        bwd_bottom_diff->name = "bwd_bottom_diff   @ " + this->getName();

        bwd_bottom_diff_ws.reset(new MKLDNNData<DType>(usr_diff_mpd, prv_diff_mpd));
        bwd_bottom_diff_ws->name = "bwd_bottom_diff_ws   @ " + this->getName();

        bwd_top_diff.reset(new MKLDNNData<DType>(usr_diff_mpd, prv_diff_mpd));
        bwd_top_diff->name = "bwd_top_diff   @ " + this->getName();
    }
    void InitReLUBwd(const Mat& src)
    {
        int32_t n = this->m_num_;
        int32_t iw = this->m_width_;
        int32_t ih = this->m_height_;
        int32_t ic = this->m_channels_;
        DType negative_slope = 0;
        void* src_data = const_cast<DType*>(mkl_prv_data<DType>(src));
        bool src_is_prv = (src_data != NULL);
        mkldnn::engine cpu_engine = CpuEngine::Instance().get_engine();
        mkldnn::memory::data_type mpcsn = mkldnn::memory::data_type::f32;
        std::shared_ptr<mkldnn::memory::desc> top_diff_md;
        std::shared_ptr<mkldnn::memory::desc> top_data_md;

        std::shared_ptr<mkldnn::memory::primitive_desc> usr_diff_mpd;
        std::shared_ptr<mkldnn::memory::primitive_desc> prv_diff_mpd;
        if (src_is_prv)
        {
            std::shared_ptr<MKLDNNMemoryDescriptor<DType>> mem_descr = get_mkldnn_prv_descriptor<DType>(src);
            top_diff_md.reset(new mkldnn::memory::desc(mem_descr->prv_memory_pd()->desc()));
            usr_diff_mpd = mem_descr->usr_memory_pd();
            prv_diff_mpd = mem_descr->prv_memory_pd();
        }
        else
        {
            top_diff_md.reset(new mkldnn::memory::desc({ { n, ic, ih, iw } }, mpcsn, mkldnn::memory::format::nchw));
            usr_diff_mpd.reset(new mkldnn::memory::primitive_desc(*top_diff_md, cpu_engine));
        }
        top_data_md = top_diff_md;
        mkldnn::eltwise_forward::desc fwd_training_desc(mkldnn::prop_kind::forward_training, mkldnn::eltwise_relu,
            *top_data_md, negative_slope);
        fwd_relu_training_pd.reset(new mkldnn::relu_forward::primitive_desc(fwd_training_desc, cpu_engine));
        mkldnn::eltwise_backward::desc reluBwd_desc(mkldnn::eltwise_relu, *top_diff_md, *top_data_md, negative_slope);
        bwd_relu_pd.reset(new mkldnn::relu_backward::primitive_desc(reluBwd_desc, cpu_engine, *fwd_relu_training_pd));
        bwd_relu_top_diff.reset(new MKLDNNData<DType>(usr_diff_mpd, prv_diff_mpd));
        bwd_relu_top_diff->name = "bwd_top_diff   @ " + this->getName();
        bwd_relu_dst_data.reset(new MKLDNNData<DType>(usr_diff_mpd, prv_diff_mpd));
        bwd_relu_dst_data->name = "bwd_bottom_data   @ " + this->getName();
    }
public:
    virtual void Forward(const Mat& in, const Mat& scale, const Mat& bias, bool inferenceOnly, double expAvgFactor,
                         double blendFactor, Mat& runMean, Mat& runVariance, Mat& out, double epsilon, Mat& savedMean,
                         Mat& savedInvStdDev)
    {
        DType* in_ptr = mkl_experimental_direct_get(in);
        DType* out_ptr = mkl_experimental_direct_get(out);
        if (blendFactor != 0 && (blendFactor != 1 || expAvgFactor > 0))
            InvalidArgument("MKL batch normalization engine currently supports blendTimeConstant of 0 or 1 only.");
        if (!init_mkldnn_)
        {
            LayerSetUp((int) in.GetNumCols(), epsilon);
            init_mkldnn_ = true;
        }
        if (fwd_inference_pd == NULL)
        {
            initFwd(in);
        }
        std::shared_ptr<mkldnn::memory> mean_memory, var_memory;
        std::shared_ptr<mkldnn::memory> fwd_input_primitive;

        DType* scaleShift_buf = static_cast<DType*>(weight_memory->get_data_handle());
        // use_weight_bias_
        DType* scale_ptr = scale.Data();
        DType* bias_ptr = bias.Data();
        memcpy(scaleShift_buf, scale_ptr, sizeof(DType)*m_channels_);
        memcpy(&scaleShift_buf[m_channels_], bias_ptr, sizeof(DType)*m_channels_);
        fwd_input_primitive = fwd_bottom_data->get_converted_prv(in_ptr, false, in);
        fwd_output_memory = fwd_top_data->create_output_memory(out_ptr, out);

        // ---- Create BatchNorm --------------------
        if (inferenceOnly)
        {
            assert(expAvgFactor == 0 && blendFactor == 1);
            savedMean.Resize(0, 0); // (these are not produced in this case)
            savedInvStdDev.Resize(0, 0);
            mean_memory.reset(new mkldnn::memory(fwd_inference_pd->mean_primitive_desc(), runMean.Data()));
            var_memory.reset(new mkldnn::memory(fwd_inference_pd->variance_primitive_desc(), runVariance.Data()));
            BatchNormFwd.reset(new mkldnn::batch_normalization_forward(
                *fwd_inference_pd, *fwd_input_primitive, (const mkldnn::primitive::at) *mean_memory,
                (const mkldnn::primitive::at) *var_memory, *weight_memory, *fwd_output_memory));
        }
        else
        {
            mean_memory.reset(new mkldnn::memory(fwd_training_pd->mean_primitive_desc(), savedMean.Data()));
            var_memory.reset(new mkldnn::memory(fwd_training_pd->variance_primitive_desc(), savedInvStdDev.Data()));
            BatchNormFwd.reset(new mkldnn::batch_normalization_forward(
                *fwd_training_pd, *fwd_input_primitive, *weight_memory, *fwd_output_memory, *mean_memory, *var_memory));
        }
        BatchNormFwd.submit();

        if (!inferenceOnly)
        {
            size_t mean_size = runMean.GetNumElements();
            int32_t m = this->m_width_ * this->m_height_ * this->m_num_;
            DType bcf = m > 1 ? DType(m) / (m - 1) : 1;
            DType* moving_mean_ptr = reinterpret_cast<DType*>(runMean.Data());
            DType* mean_ptr = reinterpret_cast<DType*>(savedMean.Data());
            DType* moving_var_ptr = reinterpret_cast<DType*>(runVariance.Data());
            DType* var_ptr = reinterpret_cast<DType*>(savedInvStdDev.Data());
            DType momentum = 1.0 - (DType) expAvgFactor;
#pragma omp parallel for
            for (int32_t i = 0; i < mean_size; i++)
            {
                moving_mean_ptr[i] = moving_mean_ptr[i] * momentum + mean_ptr[i] * expAvgFactor;
                moving_var_ptr[i] = moving_var_ptr[i] * momentum + var_ptr[i] * bcf * expAvgFactor;
            }
        }
    }

    virtual void Backward(const Mat& in, const Mat& out, const Mat& srcGrad, Mat& grad, const Mat& scale, double blendFactor,
                          const Mat& savedMean, const Mat& savedInvStdDev, Mat& scaleGrad, Mat& biasGrad,
                          bool accumulateDataGrad)
    {
        UNUSED(blendFactor);
        DType* in_ptr = mkl_experimental_direct_get(in);
        DType* out_ptr = mkl_experimental_direct_get(out);
        DType* srcgrad_ptr = mkl_experimental_direct_get(srcGrad);
        DType* grad_ptr = mkl_experimental_direct_get(grad);
        if (!init_mkldnn_)
        {
            LayerSetUp((int) in.GetNumCols());
            init_mkldnn_ = true;
        }
        if (fwd_inference_pd == NULL)
            initFwd(in);
        if (bwd_scaleshift_pd == NULL)
            InitBatchNormBwd(in);
        if (m_relu)
        {
            if (bwd_relu_pd == NULL)
            {
                InitReLUBwd(out);
            }
            std::shared_ptr<mkldnn::memory> dst_memory, diff_dst_memory, diff_src_memory;
            dst_memory = bwd_relu_dst_data->get_converted_prv(out_ptr, false, out);
            diff_src_memory = bwd_relu_top_diff->get_converted_prv(srcgrad_ptr, false, srcGrad);
            MKLDNNPrimitive<DType> reluBwd;
            reluBwd.reset(new mkldnn::relu_backward(*bwd_relu_pd, *dst_memory, *diff_src_memory, *diff_src_memory));
            reluBwd.submit();
        }
        DType* scaleShift_buf = static_cast<DType*>(weight_memory->get_data_handle());
        DType* var_ptr = NULL;
        // use_weight_bias_
        memcpy(scaleShift_buf, scale.Data(), m_channels_ * sizeof(DType));

        // optional: convert InvStd to Variance
        var_ptr = reinterpret_cast<DType*>(savedInvStdDev.Data());
        std::shared_ptr<mkldnn::memory> bwd_input_primitive;
        std::shared_ptr<mkldnn::memory> bwd_diff_dst_memory;
        std::shared_ptr<mkldnn::memory> bwd_bottom_diff_memory;
        std::shared_ptr<mkldnn::memory> bwd_bottom_diff_dst;
        std::shared_ptr<mkldnn::memory> bwd_bottom_diff_ws_memory;
        if (accumulateDataGrad)
        {
            accu_grad.Resize(grad);
            bwd_bottom_diff_dst = bwd_bottom_diff_ws_memory = bwd_bottom_diff_ws->create_output_memory(accu_grad.Data(), accu_grad);
            bwd_bottom_diff_memory = bwd_bottom_diff->get_converted_prv(grad_ptr, true, grad);
            grad_ptr = mkl_experimental_direct_get(grad);
        }
        else
        {
            bwd_bottom_diff_dst = bwd_bottom_diff_memory = bwd_bottom_diff->create_output_memory(grad_ptr, grad);
        }
        bwd_input_primitive = mkldnn_prv_memory<DType>(in);
        if (bwd_input_primitive == nullptr)
        {
            bwd_input_primitive.reset(new mkldnn::memory(*fwd_usr_mpd, in_ptr));
        }
        std::shared_ptr<mkldnn::memory> mean_memory, var_memory;
        mean_memory.reset(new mkldnn::memory(bwd_scaleshift_pd->mean_primitive_desc(), savedMean.Data()));
        var_memory.reset(new mkldnn::memory(bwd_scaleshift_pd->variance_primitive_desc(), var_ptr));

        bwd_diff_dst_memory = bwd_top_diff->get_converted_prv(srcgrad_ptr, true, srcGrad);

        BatchNormBwd.reset(new mkldnn::batch_normalization_backward(
            *bwd_scaleshift_pd, *bwd_input_primitive, *mean_memory, *var_memory, *bwd_diff_dst_memory, *weight_memory,
            *bwd_bottom_diff_dst, *diff_weight_memory));
        BatchNormBwd.submit();

        DType* scaleShiftDiff_buf = reinterpret_cast<DType*>(diff_weight_memory->get_data_handle());

        if (accumulateDataGrad)
        {
            DType * workspace_ptr = mkl_experimental_direct_get(accu_grad);
            grad.MklMem()->template AddTo<DType>(grad_ptr, *accu_grad.MklMem(), workspace_ptr);
        }
        // Store ScaleShift blobs
        DType* diff_scale = scaleGrad.Data();
        DType* diff_shift = biasGrad.Data();
        memcpy(diff_scale, scaleShiftDiff_buf, sizeof(DType)*m_channels_);
        memcpy(diff_shift, &scaleShiftDiff_buf[m_channels_], sizeof(DType)*m_channels_);
    }

private:
    bool init_mkldnn_ = false;
    std::shared_ptr<mkldnn::memory::desc> fwd_usr_input_md;
    std::shared_ptr<mkldnn::memory::primitive_desc> fwd_usr_mpd;

    // Forward
    std::shared_ptr<MKLDNNData<DType>> fwd_top_data, fwd_bottom_data;
    std::shared_ptr<mkldnn::batch_normalization_forward::primitive_desc> fwd_inference_pd;
    std::shared_ptr<mkldnn::batch_normalization_forward::primitive_desc> fwd_training_pd;
    MKLDNNPrimitive<DType> BatchNormFwd;

    // Backward
    std::shared_ptr<mkldnn::batch_normalization_backward::primitive_desc> bwd_scaleshift_pd;
    MKLDNNPrimitive<DType> BatchNormBwd;
    std::shared_ptr<MKLDNNData<DType>> bwd_top_diff;
    std::shared_ptr<MKLDNNData<DType>> bwd_bottom_diff, bwd_bottom_diff_ws;

    std::shared_ptr<mkldnn::memory> weight_memory;
    std::shared_ptr<mkldnn::memory> diff_weight_memory;
    std::shared_ptr<mkldnn::memory> fwd_output_memory;
    std::shared_ptr<mkldnn::relu_forward::primitive_desc> fwd_relu_training_pd;
    std::shared_ptr<mkldnn::relu_backward::primitive_desc> bwd_relu_pd;
    std::shared_ptr<MKLDNNData<DType>> bwd_relu_dst_data, bwd_relu_top_diff;
    // common
    int32_t m_num_, m_width_, m_height_, m_channels_;
    DType m_eps_;
    bool fix_gamma;
    TensorShape m_inOutT;
    ImageLayoutKind m_imageLayout;
    Mat accu_grad;
    bool m_relu;
}; // class MKLDNNBatchNormOp
template <>
int MKLDNNBatchNormOp<float>::s_id_gen = 1;
template <>
int MKLDNNBatchNormOp<double>::s_id_gen = 1;
} // namespace CNTK
} // namespace MSR
} // namespace Microsoft
#endif
#endif // CNTK_OPERATOR_MKL_DNN_MKLDNN_BATCH_NORM_INL_H_
