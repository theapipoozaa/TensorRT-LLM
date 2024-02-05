/*
 * Copyright (c) 2022-2024, NVIDIA CORPORATION.  All rights reserved.
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

#pragma once

#include "tensorrt_llm/common/cudaUtils.h"
#include "tensorrt_llm/runtime/common.h"
#include "tensorrt_llm/runtime/iTensor.h"

#include <memory>

namespace tensorrt_llm::runtime
{
class DecodingInput
{
public:
    using TensorPtr = std::shared_ptr<ITensor const>;

    DecodingInput(SizeType maxLength, SizeType maxAttentionWindow, SizeType sinkTokenLength, SizeType maxBatchSize,
        TensorPtr logits, TensorPtr endIds)
        : step{maxLength}
        , maxLength{maxLength}
        , maxAttentionWindow{maxAttentionWindow}
        , sinkTokenLength{sinkTokenLength}
        , maxBatchSize{maxBatchSize}
        , logits{std::move(logits)}
        , endIds{std::move(endIds)}
    {
        TLLM_CHECK_WITH_INFO(static_cast<bool>(this->logits), "Invalid logits tensor");
        TLLM_CHECK_WITH_INFO(static_cast<bool>(this->endIds), "Invalid endIds tensor");
    }

    // mandatory parameters
    SizeType step;
    SizeType maxLength;
    SizeType maxAttentionWindow;
    SizeType sinkTokenLength;
    SizeType maxBatchSize;
    TensorPtr logits; // [batchSize, beamWidth, vocabSizePadded], on gpu
    TensorPtr endIds; // [batchSize * beamWidth], on gpu

    // optional parameters
    TensorPtr finished;            // [maxBatchSize, beamWidth], finished states at current iteration.
                                   // If true for some request, the decoding step of it is skipped, on gpu
    TensorPtr sequenceLimitLength; // [maxBatchSize], on gpu
    TensorPtr embeddingBias;       // [vocabSizePadded], on gpu
    TensorPtr lengths;             // [maxBatchSize, beamWidth], on gpu
    TensorPtr badWordsList;        // [2, badWordsLength] or [batchSize, 2, badWordsLength], on gpu
    TensorPtr stopWordsList;       // [maxBatchSize, 2, stopWordsLength], on gpu
    TensorPtr noRepeatNgramSize;   // [maxBatchSize], on gpu
    TensorPtr
        batchSlots; // [batchSize], optional, address map of the linear batch id to to the seq slots, int32_t, on gpu

    // parameters for beam search
    TensorPtr cacheIndirection; // [maxBatchSize, beamWidth, maxSeqLen] - the k/v cache index for beam search, on gpu
};

} // namespace tensorrt_llm::runtime
