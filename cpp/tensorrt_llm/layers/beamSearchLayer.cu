/*
 * Copyright (c) 2019-2024, NVIDIA CORPORATION.  All rights reserved.
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
 */

#include "tensorrt_llm/common/memoryUtils.h"
#include "tensorrt_llm/kernels/beamSearchKernels.h"
#include "tensorrt_llm/layers/beamSearchLayer.h"
#include "tensorrt_llm/layers/defaultDecodingParams.h"
#include "tensorrt_llm/layers/layerUtils.h"
#include <limits>

using namespace tensorrt_llm::common;
using namespace tensorrt_llm::kernels;

namespace tensorrt_llm
{
namespace layers
{

template <typename T>
BeamSearchLayer<T>::BeamSearchLayer(
    DecoderDomain const& decoderDomain, cudaStream_t stream, std::shared_ptr<IAllocator> allocator)
    : BaseLayer(decoderDomain, stream, std::move(allocator))
    , mVocabSize(decoderDomain.getVocabSize())
    , mVocabSizePadded(decoderDomain.getVocabSizePadded())
{
    TLLM_LOG_TRACE(__PRETTY_FUNCTION__);
}

template <typename T>
BeamSearchLayer<T>::~BeamSearchLayer()
{
    TLLM_LOG_TRACE(__PRETTY_FUNCTION__);
}

template <typename T>
void BeamSearchLayer<T>::setup(runtime::SizeType32 const batchSize, runtime::SizeType32 const beamWidth,
    runtime::SizeType32 const* batchSlots, std::shared_ptr<BaseSetupParams> baseSetupParams)
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);
    TLLM_CHECK_WITH_INFO(
        beamWidth <= nMaxBeamWidth, std::string("Beam width is larger than the maximum supported (64)."));

    auto setupParams = std::dynamic_pointer_cast<BeamSearchSetupParams>(baseSetupParams);

    mDiversityRateHost.resize(batchSize);
    mLengthPenaltyHost.resize(batchSize);
    mEarlyStoppingHost.resize(batchSize);
    allocateBuffer(batchSize, beamWidth);

    auto constexpr fltMax = std::numeric_limits<float>::max();
    auto constexpr fltMin = std::numeric_limits<float>::lowest();
    auto constexpr fltEpsilon = std::numeric_limits<float>::epsilon();

    FillBuffers const fillBuffers{batchSize, batchSize, mStream};
    fillBuffers(setupParams->beam_search_diversity_rate, DefaultDecodingParams::getBeamSearchDiversity(),
        mDiversityRateHost, mDiversityRateDevice, (int*) nullptr, std::make_pair(-fltEpsilon, fltMax),
        "diveristy rate");
    fillBuffers(setupParams->length_penalty, DefaultDecodingParams::getLengthPenalty(), mLengthPenaltyHost,
        mLengthPenaltyDevice, (int*) nullptr, std::make_pair(fltMin, fltMax), "length penalty");
    fillBuffers(setupParams->early_stopping, DefaultDecodingParams::getEarlyStopping(), mEarlyStoppingHost,
        mEarlyStoppingDevice, (int*) nullptr, std::make_pair(fltMin, fltMax), "early stopping");
    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

__global__ void updateCacheIndirectionKernel(
    int* tgtCI, int const* srcCI, BeamHypotheses bh, int const nMaxAttentionWindow, int const nSinkTokenLength)
{
    // Update indirections from steps `bh.inputLength[indexBatchBeam]` to step `sequence_lengths[indexBatchBeam]`
    int const step = threadIdx.x + blockIdx.x * blockDim.x;
    int const indexBatchBeam = blockIdx.y;
    int const nBS{bh.nBatchSize};
    int const nBM{bh.nBeamWidth};
    int const nMSL{bh.nMaxSeqLen};
    int const indexBatch = indexBatchBeam / nBM;
    int const indexBeam = indexBatchBeam % nBM;
    int const lastStep{bh.sequenceLengths[indexBatchBeam] - 1}; // the sequence_lengths is updated, need to minus 1

    // Return early when the indexBatchBeam or step is out of the bound
    // No update for the indices of context part since KV Cache is shared
    if (indexBatchBeam >= nBM * nBS || step >= nMSL || step < bh.inputLengths[indexBatchBeam]
        || step < (nMSL - nMaxAttentionWindow) || bh.finished[indexBatchBeam].isFinished())
    {
        return;
    }

    // Keep all past tokens by parentIdsPtr
    int const indexBeamSrc = bh.parentIdsPtr[indexBatch][indexBeam * nMSL + lastStep];
    int const stepCirc = (step >= nSinkTokenLength)
        ? nSinkTokenLength + (step - nSinkTokenLength) % (nMaxAttentionWindow - nSinkTokenLength)
        : step;
    // Consider cyclic kv cache for the indir tables
    uint32_t const tgtOffset = indexBatch * nBM * nMaxAttentionWindow + indexBeam * nMaxAttentionWindow + stepCirc;
    uint32_t const srcOffset = indexBatch * nBM * nMaxAttentionWindow + indexBeamSrc * nMaxAttentionWindow + stepCirc;
    tgtCI[tgtOffset] = (step == lastStep) ? indexBeam : srcCI[srcOffset];
}

template <typename T>
void BeamSearchLayer<T>::forward(
    std::shared_ptr<BaseOutputParams> baseOutputs, std::shared_ptr<BaseInputParams> baseInputs)
{
    TLLM_LOG_TRACE("%s", __PRETTY_FUNCTION__);

    auto ip = std::dynamic_pointer_cast<BeamSearchInputParams>(baseInputs);
    auto op = std::dynamic_pointer_cast<BeamSearchOutputParams>(baseOutputs);

    TLLM_CHECK_WITH_INFO(op->beamHypotheses, std::string("Output BeamHypotheses is not set."));
    TLLM_CHECK_WITH_INFO(op->sequence_length->template getPtr<int>() != nullptr || mLengthPenaltyDevice == nullptr,
        std::string("Current sequence lengths must be set for length penalty computation."));
    TLLM_CHECK_WITH_INFO(ip->ite == 0, "Pipeline Parallelism is not supported yet !");

    BeamHypotheses& bh{*op->beamHypotheses};
    // bh's members already initialized in op: *CBA, batchDones
    // bh's members not used in function: outputIds, logProbs, outputIdsUnfinish, parentIdsUnfinish
    bh.nMaxBatchSize = static_cast<std::int32_t>(op->output_ids_ptr.shape[0]);
    bh.nBatchSize = ip->logits.shape[0];
    bh.nBeamWidth = static_cast<std::int32_t>(op->output_ids_ptr.shape[1]);
    bh.nIte = ip->ite;
    bh.nMaxSeqLen = static_cast<std::int32_t>(op->output_ids_ptr.shape[2]);
    bh.nVocabSize = mVocabSizePadded;
    bh.diversityRates = mDiversityRateDevice;
    bh.lengthPenalties = mLengthPenaltyDevice;
    bh.earlyStoppings = mEarlyStoppingDevice;
    bh.inputLengths = ip->input_lengths->template getPtr<int const>();
    bh.endIds = ip->end_ids.template getPtr<int const>();
    bh.logProbsTiled = (op->output_log_probs) ? op->output_log_probs->template getPtr<float>() : nullptr;
    bh.sequenceLengths = op->sequence_length->template getPtr<int>();
    bh.cumLogProbs = op->cum_log_probs->template getPtr<float>();
    bh.finished = reinterpret_cast<FinishedState*>(op->finished->template getPtr<FinishedState::UnderlyingType>());
    bh.outputIdsPtr = op->output_ids_ptr.template getPtr<int*>();
    bh.parentIdsPtr = op->parent_ids_ptr.template getPtr<int*>();

    T const* logits = ip->logits.template getPtr<T>();
    T const* bias = static_cast<T const*>(nullptr);
    TLLM_CHECK_WITH_INFO(mWorkspaceSize >= 2 * bh.nMaxBatchSize * bh.nBeamWidth * bh.nBeamWidth * 2,
        std::string("Workspace size is not enough for topk softmax."));

    invokeTopkSoftMax(logits, bias, mWorkspace, bh, mStream);
    sync_check_cuda_error();

    if (bh.nBeamWidth > 1)
    {
        auto tgtCI = op->tgt_cache_indirection.template getPtr<int>();
        auto srcCI = ip->src_cache_indirection.template getPtr<int const>();
        dim3 const grid(roundUp(bh.nMaxSeqLen, 32), bh.nBatchSize * bh.nBeamWidth);
        updateCacheIndirectionKernel<<<grid, 32, 0, mStream>>>(
            tgtCI, srcCI, bh, ip->max_attention_window, ip->sink_token_length);
        sync_check_cuda_error();
    }
}

template <typename T>
void BeamSearchLayer<T>::allocateBuffer(runtime::SizeType32 const batchSize, runtime::SizeType32 const beamWidth)
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);
    int const nPadBeamWidth = padToNextPowerOfTwo(beamWidth);
    // Unit of mWorkspaceSize is number of elements (not Byte), align to 4 for further optimization
    size_t nTopK = batchSize * nPadBeamWidth * nPadBeamWidth * 2;
    size_t nTempBuffer = batchSize * nPadBeamWidth * nMaxVocabPartForStage1FastKernel * (2 * (nPadBeamWidth * 2) + 2);
    mWorkspaceSize = roundUp(nTopK, 4) * 2 + roundUp(nTempBuffer, 4);
    mWorkspace = mAllocator->reMalloc(mWorkspace, sizeof(float) * mWorkspaceSize, true);
    mDiversityRateDevice = mAllocator->reMalloc(mDiversityRateDevice, sizeof(float) * batchSize, false);
    mLengthPenaltyDevice = mAllocator->reMalloc(mLengthPenaltyDevice, sizeof(float) * batchSize, false);
    mEarlyStoppingDevice = mAllocator->reMalloc(mEarlyStoppingDevice, sizeof(int) * batchSize, false);
    mIsAllocateBuffer = true;
    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template <typename T>
void BeamSearchLayer<T>::freeBuffer()
{
    TLLM_LOG_TRACE("%s start", __PRETTY_FUNCTION__);
    if (mIsAllocateBuffer)
    {
        mAllocator->free((void**) (&mWorkspace));
        mAllocator->free((void**) (&mDiversityRateDevice));
        mAllocator->free((void**) (&mLengthPenaltyDevice));
        mAllocator->free((void**) (&mEarlyStoppingDevice));
        mIsAllocateBuffer = false;
    }
    TLLM_LOG_TRACE("%s stop", __PRETTY_FUNCTION__);
}

template class BeamSearchLayer<float>;
template class BeamSearchLayer<half>;

} // namespace layers
} // namespace tensorrt_llm
