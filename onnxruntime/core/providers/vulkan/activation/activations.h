// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/framework/op_kernel.h"
#include "core/providers/vulkan/vulkan_kernel.h"

namespace onnxruntime {
class Node;
class VulkanExecutionProvider;
namespace vulkan {

class SigmoidKernel : VulkanKernel {
 public:
  static bool IsSupported(const GraphViewer&, const onnxruntime::Node&, const logging::Logger&) {
    return true;
  }

  static std::unique_ptr<VulkanKernel> Create(const VulkanExecutionProvider& vulkan_ep,
                                              const GraphViewer&,
                                              const onnxruntime::Node& node) {
    return std::unique_ptr<VulkanKernel>(new SigmoidKernel(vulkan_ep, node));
  }

 private:
  SigmoidKernel(const VulkanExecutionProvider& vulkan_ep, const onnxruntime::Node& node)
      : VulkanKernel{vulkan_ep, node} {
  }
};

class ClipKernel : VulkanKernel {
 public:
  static bool IsSupported(const GraphViewer& graph_viewer, const onnxruntime::Node& node,
                          const logging::Logger& logger);

  static std::unique_ptr<VulkanKernel> Create(const VulkanExecutionProvider& vulkan_ep,
                                              const onnxruntime::Node& node) {
    return std::unique_ptr<VulkanKernel>(new ClipKernel(vulkan_ep, node));
  }

  // std::string_view GetNcnnLayerName() const override {
  // if we add a Relu6 layer to NCNN we need this to plug in calling that for Clip(0, 6)
  // }

 private:
  ClipKernel(const VulkanExecutionProvider& vulkan_ep,
             const onnxruntime::Node& node)
      : VulkanKernel{vulkan_ep, node} {
  }
};

}  // namespace vulkan
}  // namespace onnxruntime
