
/*
 * SPDX-FileCopyrightText: Copyright (c) 1993-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "tensorrt_llm/kernels/cutlass_kernels/moe_gemm/moe_gemm_kernels.h"
#include <cuda_runtime_api.h>

namespace tensorrt_llm {
namespace kernels {
namespace cutlass_kernels {

// Keep in sync with the signature generated by generate_kernels.py
template <typename T, typename WeightType, typename EpilogueTag, typename TileShape, typename ClusterShape, bool BIAS>
void sm90_generic_moe_gemm_kernelLauncher(HopperGroupedGemmInput hopper_input, int num_experts,
                                          int multi_processor_count, cudaStream_t stream, int* kernel_occupancy, size_t* workspace_size);

}  // namespace cutlass_kernels
}  // namespace kernels
}  // namespace tensorrt_llm