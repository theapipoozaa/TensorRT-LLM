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

#include "tensorrt_llm/common/logger.h"
#include "tensorrt_llm/runtime/bufferManager.h"
#include "tensorrt_llm/runtime/iTensor.h"
#include "tensorrt_llm/runtime/samplingConfig.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

namespace tensorrt_llm::batch_manager
{

enum LlmRequestState_t
{
    REQUEST_STATE_UNKNOWN = 0,
    REQUEST_STATE_CONTEXT_INIT = 1,
    REQUEST_STATE_GENERATION_IN_PROGRESS = 2,
    REQUEST_STATE_GENERATION_COMPLETE = 3
};

template <typename TTensor>
class GenericLlmRequest
{
public:
    using SizeType = runtime::SizeType;
    using TokenIdType = runtime::TokenIdType;
    using RequestIdType = std::uint64_t;
    using VecTokens = std::vector<TokenIdType>;
    using VecLogProbs = std::vector<float>;
    using BeamTokens = std::vector<VecTokens>;
    using TensorPtr = TTensor;

    GenericLlmRequest(RequestIdType requestId, SizeType maxNewTokens, std::shared_ptr<VecTokens> inputTokens,
        runtime::SamplingConfig samplingConfig, bool isStreaming, std::optional<SizeType> endId = std::nullopt,
        std::optional<SizeType> padId = std::nullopt, std::optional<TensorPtr> embeddingBias = std::nullopt,
        std::optional<TensorPtr> badWordsList = std::nullopt, std::optional<TensorPtr> stopWordsList = std::nullopt,
        std::optional<TensorPtr> promptEmbeddingTable = std::nullopt,
        std::optional<SizeType> promptVocabSize = std::nullopt, std::optional<TensorPtr> loraWeights = std::nullopt,
        std::optional<TensorPtr> loraConfig = std::nullopt, bool returnLogProbs = false,
        std::optional<std::shared_ptr<VecTokens>> draftTokens = std::nullopt,
        std::optional<TensorPtr> draftLogits = std::nullopt)
        : mRequestId(requestId)
        , mPromptLen(inputTokens->size())
        , mMaxNewTokens(maxNewTokens)
        , mSamplingConfig(samplingConfig)
        , mState(REQUEST_STATE_CONTEXT_INIT)
        , mIsStreaming(isStreaming)
        , mEndId(endId)
        , mPadId(padId)
        , mSeqSlot(-1)
        , mOrigPromptLen(inputTokens->size())
        , mEmbeddingBias(embeddingBias)
        , mBadWordsList(badWordsList)
        , mStopWordsList(stopWordsList)
        , mPromptEmbeddingTable(promptEmbeddingTable)
        , mPromptVocabSize(promptVocabSize)
        , mLoraWeights(loraWeights)
        , mLoraConfig(loraConfig)
        , mReturnLogProbs(returnLogProbs)
        , mContextChunkSize(std::nullopt)
        , mContextCurrentPosition(0)
        , mLogProbs(samplingConfig.beamWidth)
        , mCumLogProbs(samplingConfig.beamWidth)
        , mDraftTokens(draftTokens.value_or(std::make_shared<VecTokens>()))
        , mDraftLogits(draftLogits)
    {
        mMaxSentTokenPos = mPromptLen - 1;
        // Scatter the input tokens to other beam
        mTokens = BeamTokens(mSamplingConfig.beamWidth, *inputTokens);

        if ((mPromptEmbeddingTable.has_value() && !mPromptVocabSize.has_value())
            || (!mPromptEmbeddingTable.has_value() && mPromptVocabSize.has_value()))
        {
            std::string errStr
                = "Prompt embedding table and prompt vocab size tensors must both be provided for requests with prompt "
                  "tuning enabled.";
            TLLM_LOG_ERROR(errStr);
            throw std::runtime_error(errStr);
        }

        if (draftLogits.has_value() && !draftTokens.has_value())
        {
            std::string errStr = "Draft tokens must be specified when draft logits are given.";
            TLLM_LOG_ERROR(errStr);
            throw std::runtime_error(errStr);
        }
    }

    /// @brief Get total number of tokens for this req (prompt + generated)
    /// @param beam The beam index
    /// @return  The number of tokens
    SizeType getNumTokens(SizeType beam) const
    {
        return mTokens.at(beam).size();
    }

    /// @brief Get max number of tokens across all beams
    /// @return  The number of tokens
    SizeType getMaxBeamNumTokens() const
    {
        SizeType maxTokens = 0;
        for (SizeType beam = 0; beam < mSamplingConfig.beamWidth; ++beam)
        {
            maxTokens = std::max(maxTokens, static_cast<SizeType>(mTokens.at(beam).size()));
        }
        return maxTokens;
    }

    /// @brief Get a token at a given position and beam index
    /// @param beam  The beam index
    /// @param pos The position of the token relative to beginning of the prompt
    /// @return  The token index
    TokenIdType getToken(SizeType beam, SizeType pos) const
    {
        return mTokens.at(beam).at(pos);
    }

    /// @brief Get the tokens at a given beam index
    /// @param beam The beam index
    /// @return A vector of tokens for this beam index, includes the prompt
    VecTokens const& getTokens(SizeType beam) const
    {
        return mTokens.at(beam);
    }

    /// @brief Get all tokens (input+output) for all beams
    /// @return A vector of vector of tokens.
    BeamTokens const& getTokens() const
    {
        return mTokens;
    }

    /// @brief Get the draft tokens
    /// @return shared_ptr to vector of draft tokens
    std::shared_ptr<VecTokens> const& getDraftTokens() const
    {
        return mDraftTokens;
    }

    /// @brief Get the logits for the draft tokens
    /// @return Tensor of draft logits
    std::optional<TensorPtr> getDraftLogits() const
    {
        return mDraftLogits;
    }

    /// @brief Returns true if request has draft tokens
    /// @return flag
    bool hasDraftTokens() const
    {
        return mDraftTokens && mDraftTokens->size() > 0;
    }

    /// @brief Get the maximum number of generated tokens among all rays in beam
    /// @return The number of generated tokens (doesn't include the prompt tokens)
    SizeType getMaxNumGeneratedTokens() const
    {
        return getMaxBeamNumTokens() - mPromptLen;
    }

    /// @brief Add new generated tokens to the vector of tokens
    /// @param token The token to add
    /// @param beam The beam to which to add the new token
    void addNewToken(TokenIdType token, SizeType beam)
    {
        mTokens.at(beam).push_back(token);
    }

    /// @brief Add new generated tokens to the vector of tokens
    /// @param beamTokens A vector containing the tokens to add for each beam index
    ///                   beamTokens is expected to be of size beamWidth
    void addNewTokens(VecTokens const& beamTokens)
    {
        assert(static_cast<size_t>(mSamplingConfig.beamWidth) == beamTokens.size());
        for (std::size_t beam = 0; beam < beamTokens.size(); ++beam)
        {
            const auto outputId = beamTokens[beam];
            mTokens.at(beam).push_back(outputId);
        }
    }

    /// @brief Sets the generated tokens for all beams. Erases all previous generated tokens.
    /// @param generatedBeamTokens The generated tokens for all beams (vector of vector of tokens)
    void setGeneratedTokens(const BeamTokens& generatedBeamTokens)
    {
        assert(generatedBeamTokens.size() == static_cast<size_t>(mSamplingConfig.beamWidth));
        for (std::size_t beam = 0; beam < generatedBeamTokens.size(); ++beam)
        {
            auto& beamTokens = mTokens[beam];
            beamTokens.resize(mPromptLen);
            beamTokens.insert(beamTokens.end(), generatedBeamTokens[beam].begin(), generatedBeamTokens[beam].end());
        }
    }

    /// @brief Pause a request by moving the generated tokens to the prompt
    /// @param maxInputLen The maximum prompt len.
    void pause(SizeType maxInputLen)
    {
        // TODO: For beamWidth > 1, we would need to support swapping to avoid
        // recomputing from the start
        // As a temporary solution, we currently reset the tokens to the prompt
        if (mSamplingConfig.beamWidth > 1)
        {
            for (std::size_t beam = 0; beam < mTokens.size(); ++beam)
            {
                auto& beamTokens = mTokens.at(beam);
                beamTokens.resize(mPromptLen);
                if (mReturnLogProbs)
                {
                    mLogProbs.at(beam).clear();
                }
            }
        }
        else
        {
            SizeType newPromptLen = std::min(maxInputLen, mPromptLen + getMaxNumGeneratedTokens());
            TLLM_LOG_DEBUG("pause: id %lu, mPromptLen %d, newPromptLen %d", mRequestId, mPromptLen, newPromptLen);
            for (std::size_t beam = 0; beam < mTokens.size(); ++beam)
            {
                auto& beamTokens = mTokens.at(beam);
                beamTokens.resize(newPromptLen);

                if (mReturnLogProbs)
                {
                    auto& logProb = mLogProbs.at(beam);
                    logProb.resize(newPromptLen - mPromptLen);
                }
            }
            mMaxNewTokens -= (newPromptLen - mPromptLen);
            mPromptLen = newPromptLen;
        }
        mState = REQUEST_STATE_CONTEXT_INIT;
        mContextCurrentPosition = 0;
        mContextChunkSize = std::nullopt;
        mSeqSlot = -1;
    }

    /// @brief Get the maximum position of the tokens returned to the client. Use to ensure we don't return to
    /// client duplicated token positions.
    /// @return The maximum position of the tokens sent to the client
    SizeType getMaxSentTokenPos() const
    {
        return mMaxSentTokenPos;
    }

    /// @brief Sets the maximum position of the tokens returned to the client. Use to ensure we don't return to
    /// client duplicated token positions.
    /// @param pos The maximum position
    void setMaxSentTokenPos(SizeType pos)
    {
        mMaxSentTokenPos = pos;
    }

    std::optional<TensorPtr> getPromptEmbeddingTable() const
    {
        return mPromptEmbeddingTable;
    }

    std::optional<SizeType> getPromptVocabSize() const
    {
        return mPromptVocabSize;
    }

    std::optional<TensorPtr> getLoraWeights() const
    {
        return mLoraWeights;
    }

    std::optional<TensorPtr> getLoraConfig() const
    {
        return mLoraConfig;
    }

    std::optional<TensorPtr> getEmbeddingBias() const
    {
        return mEmbeddingBias;
    }

    std::optional<TensorPtr> getBadWordsList() const
    {
        return mBadWordsList;
    }

    std::optional<TensorPtr> getStopWordsList() const
    {
        return mStopWordsList;
    }

    bool returnLogProbs() const
    {
        return mReturnLogProbs;
    }

    std::vector<VecLogProbs> const& getLogProbs() const
    {
        return mLogProbs;
    }

    VecLogProbs const& getLogProbs(SizeType beam) const
    {
        return mLogProbs.at(beam);
    }

    void setLogProbs(VecLogProbs const& logProbs, SizeType beam)
    {
        mLogProbs.at(beam).resize(mPromptLen - mOrigPromptLen);
        mLogProbs.at(beam).insert(mLogProbs.at(beam).end(), logProbs.begin(), logProbs.end());
    }

    VecLogProbs const& getCumLogProbs() const
    {
        return mCumLogProbs;
    }

    void setCumLogProb(float cumLogProb, SizeType beam)
    {
        mCumLogProbs.at(beam) = cumLogProb;
    }

    SizeType getOrigPromptLen() const
    {
        return mOrigPromptLen;
    }

    void setDraftTokens(const std::shared_ptr<VecTokens>& draftTokens)
    {
        mDraftTokens = draftTokens;
    }

    void setDraftLogits(const std::optional<TensorPtr>& draftLogits)
    {
        mDraftLogits = draftLogits;
    }

    TensorPtr const& getContextLogitsHost() const
    {
        return mContextLogitsHost;
    }

    void setContextLogitsHost(TensorPtr contextLogitsHost)
    {
        mContextLogitsHost = std::move(contextLogitsHost);
    }

    TensorPtr const& getGenerationLogitsHost() const
    {
        return mGenerationLogitsHost;
    }

    void setGenerationLogitsHost(TensorPtr generationLogitsHost)
    {
        mGenerationLogitsHost = std::move(generationLogitsHost);
    }

    std::vector<TensorPtr> const& getGenerationLogitsFragments() const
    {
        return mGenerationLogitsFragments;
    }

    void addGenerationFragments(TensorPtr& genLogits)
    {
        mGenerationLogitsFragments.push_back(genLogits);
    }

    SizeType getGenerationLogitsFragmentsSize()
    {
        return mGenerationLogitsFragments.size();
    }

    void clearGenerationLogitsFragments()
    {
        mGenerationLogitsFragments.clear();
    }

    bool isContextInitState() const noexcept
    {
        return mState == REQUEST_STATE_CONTEXT_INIT;
    }

    bool isGenerationInProgessState() const noexcept
    {
        return mState == REQUEST_STATE_GENERATION_IN_PROGRESS;
    }

    /// To determine whether the context is unchunked. When a context is chunked into only a part, it
    /// is still different from the unchunked state, which indicates the initial status.
    bool isFullContextRequest() const noexcept
    {
        return isContextInitState() && !mContextChunkSize;
    }

    /// When chunked, the position of the current chunk is returned. Otherwise, only the beginning
    /// or end of the context is returned.
    SizeType getContextCurrentPosition() const noexcept
    {
        return mContextCurrentPosition;
    }

    /// Return the length of the context that has not yet been processed.
    SizeType getContextRemainingLength() const noexcept
    {
        return mPromptLen - getContextCurrentPosition();
    }

    /// To retrieve the context chunk size, throw an exception when the context is not chunked.
    SizeType getContextChunkSize() const
    {
        TLLM_CHECK_WITH_INFO(
            isContextInitState() && mContextChunkSize, "The current request is not in context chunking state.");
        return mContextChunkSize.value();
    }

    /// To set the context chunk size, throw an exception when the chunk size is negative. If the chunk
    /// size is greater than the remaining length of the context, the size will be reduced to fit the
    /// remaining length.
    void setContextChunkSize(SizeType size)
    {
        TLLM_CHECK_WITH_INFO(isContextInitState(), "Chunking is only possible during the context phase.");
        TLLM_CHECK_WITH_INFO(size >= 0, "The chunk size of context (%d) can't be negative.", size);
        mContextChunkSize = std::min(size, getContextRemainingLength());
    }

    /// Determines whether the current position is only one chunk away from the end of the context.
    /// It will return true when the context is not chunked.
    bool isLastContextChunk() const noexcept
    {
        return isFullContextRequest()
            || (isContextInitState() && getContextCurrentPosition() + getContextChunkSize() == mPromptLen);
    }

    /// Returns whether the position is at the beginning of the context. It will return true when the
    /// context is not chunked.
    bool isFirstContextChunk() const noexcept
    {
        return isFullContextRequest() || getContextCurrentPosition() == 0;
    }

    /// Move the cursor forward one chunk. When not chunked, move forward to the end of the context.
    void moveToNextContextChunk()
    {
        TLLM_CHECK_WITH_INFO(isContextInitState(), "Chunking is only possible during the context phase.");
        if (mContextChunkSize)
        {
            mContextCurrentPosition += getContextChunkSize();
            setContextChunkSize(0);
        }
        else
        {
            TLLM_CHECK_WITH_INFO(mContextCurrentPosition == 0, "Full context out of bounds.");
            mContextCurrentPosition = mPromptLen;
        }
    }

    RequestIdType mRequestId;
    SizeType mPromptLen;
    SizeType mMaxNewTokens;
    // Tokens [beam_size, mPromptLen + getMaxNumGeneratedTokens()]
    runtime::SamplingConfig mSamplingConfig;
    LlmRequestState_t mState;
    bool mIsStreaming;
    std::optional<SizeType> mEndId;
    std::optional<SizeType> mPadId;
    SizeType mSeqSlot;

protected:
    SizeType mOrigPromptLen;
    BeamTokens mTokens;
    SizeType mMaxSentTokenPos;

    std::optional<TensorPtr> mEmbeddingBias;
    std::optional<TensorPtr> mBadWordsList;
    std::optional<TensorPtr> mStopWordsList;

    std::optional<TensorPtr> mPromptEmbeddingTable;
    std::optional<SizeType> mPromptVocabSize;

    std::optional<TensorPtr> mLoraWeights;
    std::optional<TensorPtr> mLoraConfig;

    bool mReturnLogProbs;

    // To enable chunked context, the FHMA paged kv-cache also needs to be enabled. Except for the last one,
    // the size of the context chunk needs to be an integer multiple of the kv-cache block size. The meaning
    // of null value is that the context is not chunked.
    std::optional<SizeType> mContextChunkSize;
    SizeType mContextCurrentPosition;

    std::vector<VecLogProbs> mLogProbs; // [beamSize, seqLen]
    VecLogProbs mCumLogProbs;           // [beamSize]
    std::shared_ptr<VecTokens> mDraftTokens;
    std::optional<TensorPtr> mDraftLogits;

    // Save logits
    TensorPtr mContextLogits;    // [mPromptLen, vocab_size_padded]
    TensorPtr mContextLogitsHost;
    TensorPtr mGenerationLogits; // [beam_size, mMaxNewTokens, vocab_size_padded]
    TensorPtr mGenerationLogitsHost;
    std::vector<TensorPtr> mGenerationLogitsFragments;
};

class LlmRequest : public GenericLlmRequest<runtime::ITensor::SharedPtr>
{
public:
    using Base = GenericLlmRequest<runtime::ITensor::SharedPtr>;
    using TensorPtr = Base::TensorPtr;
    using SizeType = Base::SizeType;
    using TokenIdType = Base::TokenIdType;
    using RequestIdType = Base::RequestIdType;
    using VecLogProbs = Base::VecLogProbs;
    using BeamTokens = Base::BeamTokens;
    using VecTokens = Base::VecTokens;

    LlmRequest(RequestIdType requestId, SizeType maxNewTokens, std::shared_ptr<VecTokens> inputTokens,
        runtime::SamplingConfig samplingConfig, bool isStreaming, std::optional<SizeType> endId = std::nullopt,
        std::optional<SizeType> padId = std::nullopt, std::optional<TensorPtr> embeddingBias = std::nullopt,
        std::optional<TensorPtr> badWordsList = std::nullopt, std::optional<TensorPtr> stopWordsList = std::nullopt,
        std::optional<TensorPtr> promptEmbeddingTable = std::nullopt,
        std::optional<SizeType> promptVocabSize = std::nullopt, std::optional<TensorPtr> loraWeights = std::nullopt,
        std::optional<TensorPtr> loraConfig = std::nullopt, bool returnLogProbs = false,
        std::optional<std::shared_ptr<VecTokens>> draftTokens = std::nullopt,
        std::optional<TensorPtr> draftLogits = std::nullopt)
        : Base(requestId, maxNewTokens, inputTokens, samplingConfig, isStreaming, endId, padId, embeddingBias,
            badWordsList, stopWordsList, promptEmbeddingTable, promptVocabSize, loraWeights, loraConfig, returnLogProbs,
            draftTokens, draftLogits)
    {
    }

    void movePromptEmbeddingTableToGpu(runtime::BufferManager const& manager)
    {
        if (!mPromptEmbeddingTable.has_value()
            || mPromptEmbeddingTable.value()->getMemoryType() == runtime::MemoryType::kGPU)
        {
            return;
        }
        else
        {
            TensorPtr gpuPromptEmbeddingTable
                = manager.copyFrom(*mPromptEmbeddingTable.value(), runtime::MemoryType::kGPU);
            mPromptEmbeddingTable = gpuPromptEmbeddingTable;
        }
    }

    void moveLoraWeightsToGpu(runtime::BufferManager const& manager)
    {
        if (!mLoraWeights.has_value() || mLoraWeights.value()->getMemoryType() == runtime::MemoryType::kGPU)
        {
            return;
        }
        // TODO for tp / pp models we only need to move the bit that belong on the local device
        TensorPtr gpuLoraWeights = manager.copyFrom(*mLoraWeights.value(), runtime::MemoryType::kGPU);
        mLoraWeights = gpuLoraWeights;
    }
};

} // namespace tensorrt_llm::batch_manager
