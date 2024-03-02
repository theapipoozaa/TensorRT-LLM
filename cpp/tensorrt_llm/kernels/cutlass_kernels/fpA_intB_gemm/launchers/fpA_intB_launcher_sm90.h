/*
 * Copyright (c) 2020-2023, NVIDIA CORPORATION.  All rights reserved.
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

#include "cutlass_extensions/gemm_configs.h"
#include "cutlass_extensions/weight_only_quant_op.h"
#include <cuda_runtime_api.h>

namespace tensorrt_llm
{
namespace kernels
{
namespace cutlass_kernels
{

template <typename ActivationType, typename WeightType, typename ScaleZeroType, typename BiasType, typename OutputType,
    cutlass::WeightOnlyQuantOp QuantOp, typename EpilogueTag, typename CTAShape, typename ClusterShape,
    typename MainloopScheduleType, typename EpilogueScheduleType>
void sm90_generic_mixed_gemm_kernelLauncher(const ActivationType* A, const WeightType* B,
    const ScaleZeroType* weight_scales, const ScaleZeroType* weight_zero_points, const BiasType* biases,
    const float alpha, OutputType* C, int m, int n, int k, const int group_size,
    tensorrt_llm::cutlass_extensions::CutlassGemmConfig gemm_config, char* workspace, size_t workspace_bytes,
    cudaStream_t stream, int* occupancy = nullptr);

} // namespace cutlass_kernels
} // namespace kernels
} // namespace tensorrt_llm
