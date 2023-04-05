#pragma once
#include "onnxruntime_cxx_api.h"
#include <optional>
#include <numeric>
#include <unordered_set>

namespace Ort {
namespace Custom2 {

class Tensor {
 public:
  Tensor(OrtKernelContext* ctx) : ctx_(ctx) {}
  operator bool() const {
    return shape_.has_value();
  }

 protected:
  struct KernelContext ctx_;
  std::optional<std::vector<int64_t>> shape_;
};

template <typename T>
struct Span {
  const T* data_ = {};
  size_t size_ = {};
  void Assign(const T* data, size_t size) {
    data_ = data;
    size_ = size;
  }
  size_t size() const { return size_; }
  T operator[](size_t indice) const {
    return data_[indice];
  }
};

// std::enable_if<std::is_same<T, std::string>::value>::type
template <typename T>
class TensorT : public Tensor {
 public:
  using TT = typename std::remove_reference<T>::type;
  TensorT(OrtKernelContext* ctx, size_t indice, bool is_input) : Tensor(ctx), indice_(indice), is_input_(is_input) {
    if (is_input && indice < ctx_.GetInputCount()) {
      const_value_ = ctx_.GetInput(indice);
      auto type_shape_info = const_value_.GetTensorTypeAndShapeInfo();
      shape_ = type_shape_info.GetShape();
    }
  }

  const std::vector<int64_t>& Shape() const {
    return shape_.value();
  }
  int64_t NumberOfElement() const {
    if (shape_.has_value()) {
      return std::accumulate(shape_->begin(), shape_->end(), 1ULL, std::multiplies<int64_t>());
    } else {
      return 0;
    }
  }
  const TT* Data() const {
    return reinterpret_cast<const TT*>(const_value_.GetTensorRawData());
  }
  TT* Allocate(const std::vector<int64_t>& shape) {
    shape_ = shape;
    if (!data_) {
      shape_ = shape;
      data_ = ctx_.GetOutput(indice_, shape).GetTensorMutableData<TT>();
    }
    return data_;
  }
  static TT GetT() { return (TT)0; }
  const Span<T>& AsSpan() {
    // assert shape_ is 1-d
    span_.Assign(Data(), (*shape_)[0]);
    return span_;
  }
  const T& AsScalar() {
    // assert shape_ is {1}
    return *Data();
  }

 private:
  size_t indice_;
  bool is_input_;
  ConstValue const_value_;  // for input
  TT* data_{};              // for output
  Span<T> span_;
};

template <>
class TensorT<std::string> : public Tensor {
 public:
  using strings = std::vector<std::string>;

  TensorT(OrtKernelContext* ctx, size_t indice, bool is_input) : Tensor(ctx), indice_(indice), is_input_(is_input) {
    if (is_input) {
      auto const_value = ctx_.GetInput(indice);
      auto type_shape_info = const_value.GetTensorTypeAndShapeInfo();
      shape_ = type_shape_info.GetShape();
      auto num_chars = const_value.GetStringTensorDataLength();
      // todo - too much copies here ...
      std::vector<char> chars(num_chars + 1, '\0');
      auto num_strings = NumberOfElement();
      std::vector<size_t> offsets(NumberOfElement());
      const_value.GetStringTensorContent(static_cast<void*>(chars.data()), num_chars, offsets.data(), offsets.size());
      auto upper_bound = static_cast<int64_t>(num_strings) - 1;
      input_strings_.resize(num_strings);
      for (int64_t i = upper_bound; i >= 0; --i) {
        if (i < upper_bound) {
          chars[offsets[i + 1]] = '\0';
        }
        input_strings_[i] = chars.data() + offsets[i];
      }
    }
  }
  const std::vector<int64_t>& Shape() const {
    return *shape_;
  }
  int64_t NumberOfElement() const {
    if (shape_.has_value()) {
      return std::accumulate(shape_->begin(), shape_->end(), 1ULL, std::multiplies<int64_t>());
    } else {
      return 0;
    }
  }
  const strings& Data() const {
    return input_strings_;
  }
  void SetStringOutput(const strings& ss, const std::vector<int64_t>& dims) {
    shape_ = dims;
    std::vector<const char*> raw;
    for (const auto& s : ss) {
      raw.push_back(s.data());
    }
    auto output = ctx_.GetOutput(indice_, dims.data(), dims.size());
    // note - there will be copy ...
    output.FillStringTensor(raw.data(), raw.size());
  }
  const std::string& AsScalar() {
    // assert shape_ is {1}
    return input_strings_[0];
  }

 private:
  size_t indice_;
  bool is_input_;
  std::vector<std::string> input_strings_;  // for input
  // TT* data_{};              // for output
};

template <>
class TensorT<std::string_view> : public Tensor {
 public:
  using strings = std::vector<std::string>;
  using string_views = std::vector<std::string_view>;

  TensorT(OrtKernelContext* ctx, size_t indice, bool is_input) : Tensor(ctx), indice_(indice), is_input_(is_input) {
    if (is_input) {
      auto const_value = ctx_.GetInput(indice);
      auto type_shape_info = const_value.GetTensorTypeAndShapeInfo();
      shape_ = type_shape_info.GetShape();
      auto num_chars = const_value.GetStringTensorDataLength();
      chars_.resize(num_chars + 1, '\0');
      auto num_strings = NumberOfElement();
      std::vector<size_t> offsets(NumberOfElement());
      const_value.GetStringTensorContent(static_cast<void*>(chars_.data()), num_chars, offsets.data(), offsets.size());
      offsets.push_back(num_chars);
      auto upper_bound = static_cast<int64_t>(num_strings) - 1;
      for (size_t i = 0; i < num_strings; ++i) {
        input_string_views_.emplace_back(chars_.data() + offsets[i], offsets[i + 1] - offsets[i]);
      }
    }
  }
  const std::vector<int64_t>& Shape() const {
    return *shape_;
  }
  int64_t NumberOfElement() const {
    if (shape_.has_value()) {
      return std::accumulate(shape_->begin(), shape_->end(), 1ULL, std::multiplies<int64_t>());
    } else {
      return 0;
    }
  }
  const string_views& Data() const {
    return input_string_views_;
  }
  void SetStringOutput(const strings& ss, const std::vector<int64_t>& dims) {
    shape_ = dims;
    std::vector<const char*> raw;
    for (const auto& s : ss) {
      raw.push_back(s.data());
    }
    auto output = ctx_.GetOutput(indice_, dims.data(), dims.size());
    // note - there will be copy ...
    output.FillStringTensor(raw.data(), raw.size());
  }
  std::string_view AsScalar() {
    // assert shape_ is {1}
    return input_string_views_[0];
  }

 private:
  size_t indice_;
  bool is_input_;
  std::vector<char> chars_;                           // for input
  std::vector<std::string_view> input_string_views_;  // for input
};

using TensorPtr = std::unique_ptr<Custom2::Tensor>;

//////////////////////////// OrtCustomOpBase ////////////////////////////////

struct OrtCustomOpBase : public OrtCustomOp {
  using ConstOptionalFloatTensor = std::optional<const Custom2::TensorT<float>&>;
  using OptionalFloatTensor = std::optional<Custom2::TensorT<float>>;

  // CreateTuple
  template <size_t ith_input, size_t ith_output, typename... Ts>
  static typename std::enable_if<sizeof...(Ts) == 0, std::tuple<>>::type
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t, size_t) {
    return std::make_tuple();
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  static typename std::enable_if<std::is_same<T, OrtKernelContext*>::value, std::tuple<T, Ts...>>::type
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output) {
    std::tuple<T> current = std::tuple<OrtKernelContext*>{context};
    auto next = CreateTuple<ith_input, ith_output, Ts...>(context, tensors, num_input, num_output);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  static typename std::enable_if<std::is_same<T, std::optional<const Custom2::TensorT<float>*>>::value, std::tuple<T, Ts...>>::type
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output) {
    if (ith_input < num_input) {
      tensors.push_back(std::make_unique<Custom2::TensorT<float>>(context, ith_input, true));
      std::tuple<T> current = std::tuple<T>{reinterpret_cast<const Custom2::TensorT<float>*>(tensors.back().get())};
      auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, tensors, num_input, num_output);
      return std::tuple_cat(current, next);
    } else {
      std::tuple<T> current = std::tuple<T>{};
      auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, tensors, num_input, num_output);
      return std::tuple_cat(current, next);
    }
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  static typename std::enable_if<std::is_same<T, std::optional<Custom2::TensorT<float>*>>::value, std::tuple<T, Ts...>>::type
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output) {
    if (ith_output < num_output) {
      tensors.push_back(std::make_unique<Custom2::TensorT<float>>(context, ith_output, false));
      std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom2::TensorT<float>*>(tensors.back().get())};
      auto next = CreateTuple<ith_input, ith_output + 1, Ts...>(context, tensors, num_input, num_output);
      return std::tuple_cat(current, next);
    } else {
      std::tuple<T> current = std::tuple<T>{};
      auto next = CreateTuple<ith_input, ith_output + 1, Ts...>(context, tensors, num_input, num_output);
      return std::tuple_cat(current, next);
    }
  }

#define CREATE_TUPLE(data_type)                                                                                              \
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>                                                 \
  static typename std::enable_if<std::is_same<T, const Custom2::TensorT<data_type>&>::value, std::tuple<T, Ts...>>::type     \
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output) {             \
    tensors.push_back(std::make_unique<Custom2::TensorT<data_type>>(context, ith_input, true));                              \
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors.back())};                                             \
    auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, tensors, num_input, num_output);                      \
    return std::tuple_cat(current, next);                                                                                    \
  }                                                                                                                          \
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>                                                 \
  static typename std::enable_if<std::is_same<T, const Custom2::Span<data_type>&>::value, std::tuple<T, Ts...>>::type        \
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output) {             \
    tensors.push_back(std::make_unique<Custom2::TensorT<data_type>>(context, ith_input, true));                              \
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom2::TensorT<data_type>*>(tensors.back().get())->AsSpan()};   \
    auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, tensors, num_input, num_output);                      \
    return std::tuple_cat(current, next);                                                                                    \
  }                                                                                                                          \
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>                                                 \
  static typename std::enable_if<std::is_same<T, data_type>::value, std::tuple<T, Ts...>>::type                              \
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output) {             \
    tensors.push_back(std::make_unique<Custom2::TensorT<data_type>>(context, ith_input, true));                              \
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom2::TensorT<data_type>*>(tensors.back().get())->AsScalar()}; \
    auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, tensors, num_input, num_output);                      \
    return std::tuple_cat(current, next);                                                                                    \
  }                                                                                                                          \
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>                                                 \
  static typename std::enable_if<std::is_same<T, Custom2::TensorT<data_type>&>::value, std::tuple<T, Ts...>>::type           \
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output) {             \
    tensors.push_back(std::make_unique<Custom2::TensorT<data_type>>(context, ith_output, false));                            \
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors.back())};                                             \
    auto next = CreateTuple<ith_input, ith_output + 1, Ts...>(context, tensors, num_input, num_output);                      \
    return std::tuple_cat(current, next);                                                                                    \
  }

  CREATE_TUPLE(bool)
  CREATE_TUPLE(float)
  CREATE_TUPLE(Ort::Float16_t)
  CREATE_TUPLE(Ort::BFloat16_t)
  CREATE_TUPLE(double)
  CREATE_TUPLE(int8_t)
  CREATE_TUPLE(int16_t)
  CREATE_TUPLE(int32_t)
  CREATE_TUPLE(int64_t)
  CREATE_TUPLE(uint8_t)
  CREATE_TUPLE(uint16_t)
  CREATE_TUPLE(uint32_t)
  CREATE_TUPLE(uint64_t)
  CREATE_TUPLE(std::string)
  CREATE_TUPLE(std::string_view)  // todo - remove string_view output

  // ParseArgs ...
  template <typename... Ts>
  static typename std::enable_if<0 == sizeof...(Ts)>::type
  ParseArgs(std::vector<ONNXTensorElementDataType>&, std::vector<ONNXTensorElementDataType>&) {
  }

  template <typename T, typename... Ts>
  static typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, OrtKernelContext*>::value>::type
  ParseArgs(std::vector<ONNXTensorElementDataType>& input_types, std::vector<ONNXTensorElementDataType>& output_types) {
    ParseArgs<Ts...>(input_types, output_types);
  }

  template <typename T, typename... Ts>
  static typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, std::optional<const Custom2::TensorT<float>*>>::value>::type
  ParseArgs(std::vector<ONNXTensorElementDataType>& input_types, std::vector<ONNXTensorElementDataType>& output_types) {
    input_types.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    ParseArgs<Ts...>(input_types, output_types);
  }

  template <typename T, typename... Ts>
  static typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, std::optional<Custom2::TensorT<float>*>>::value>::type
  ParseArgs(std::vector<ONNXTensorElementDataType>& input_types, std::vector<ONNXTensorElementDataType>& output_types) {
    output_types.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    ParseArgs<Ts...>(input_types, output_types);
  }

#define PARSE_INPUT_BASE(pack_type, onnx_type)                                                                                         \
  template <typename T, typename... Ts>                                                                                                \
  static typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, pack_type>::value>::type                                        \
  ParseArgs(std::vector<ONNXTensorElementDataType>& input_types, std::vector<ONNXTensorElementDataType>& output_types) {               \
    input_types.push_back(onnx_type);                                                                                                  \
    ParseArgs<Ts...>(input_types, output_types);                                                                                       \
  }

#define PARSE_INPUT(data_type, onnx_type)                         \
  PARSE_INPUT_BASE(const Custom2::TensorT<data_type>&, onnx_type) \
  PARSE_INPUT_BASE(const Custom2::Span<data_type>&, onnx_type)    \
  PARSE_INPUT_BASE(data_type, onnx_type)

#define PARSE_OUTPUT(data_type, onnx_type)                                                                               \
  template <typename T, typename... Ts>                                                                                  \
  static typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, Custom2::TensorT<data_type>&>::value>::type       \
  ParseArgs(std::vector<ONNXTensorElementDataType>& input_types, std::vector<ONNXTensorElementDataType>& output_types) { \
    output_types.push_back(onnx_type);                                                                                   \
    ParseArgs<Ts...>(input_types, output_types);                                                                         \
  }

#define PARSE_ARGS(data_type, onnx_type) \
  PARSE_INPUT(data_type, onnx_type)      \
  PARSE_OUTPUT(data_type, onnx_type)

  PARSE_ARGS(bool, ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL)
  PARSE_ARGS(float, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
  PARSE_ARGS(Ort::Float16_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16)
  PARSE_ARGS(Ort::BFloat16_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16)
  PARSE_ARGS(double, ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE)
  PARSE_ARGS(int8_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8)
  PARSE_ARGS(int16_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16)
  PARSE_ARGS(int32_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32)
  PARSE_ARGS(int64_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
  PARSE_ARGS(uint8_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8)
  PARSE_ARGS(uint16_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16)
  PARSE_ARGS(uint32_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32)
  PARSE_ARGS(uint64_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64)
  PARSE_ARGS(std::string, ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING)
  PARSE_ARGS(std::string_view, ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING)  // todo - remove string_view output

  OrtCustomOpBase(const char* op_name,
                  const char* execution_provider) : op_name_(op_name),
                                                    execution_provider_(execution_provider) {
    OrtCustomOp::version = ORT_API_VERSION;

    OrtCustomOp::GetName = [](const OrtCustomOp* op) { return static_cast<const OrtCustomOpBase*>(op)->op_name_.c_str(); };
    OrtCustomOp::GetExecutionProviderType = [](const OrtCustomOp* op) { return ((OrtCustomOpBase*)op)->execution_provider_.c_str(); };
    OrtCustomOp::GetInputMemoryType = [](const OrtCustomOp*, size_t) { return OrtMemTypeDefault; };

    OrtCustomOp::GetInputTypeCount = [](const OrtCustomOp* op) {
      auto self = reinterpret_cast<const OrtCustomOpBase*>(op);
      return self->input_types_.size();
    };

    OrtCustomOp::GetInputType = [](const OrtCustomOp* op, size_t indice) {
      auto self = reinterpret_cast<const OrtCustomOpBase*>(op);
      return self->input_types_[indice];
    };

    OrtCustomOp::GetOutputTypeCount = [](const OrtCustomOp* op) {
      auto self = reinterpret_cast<const OrtCustomOpBase*>(op);
      return self->output_types_.size();
    };

    OrtCustomOp::GetOutputType = [](const OrtCustomOp* op, size_t indice) {
      auto self = reinterpret_cast<const OrtCustomOpBase*>(op);
      return self->output_types_[indice];
    };

    OrtCustomOp::GetInputCharacteristic = [](const OrtCustomOp*, size_t) {
      return INPUT_OUTPUT_OPTIONAL;
    };

    OrtCustomOp::GetOutputCharacteristic = [](const OrtCustomOp*, size_t) {
      return INPUT_OUTPUT_OPTIONAL;
    };

    OrtCustomOp::GetVariadicInputMinArity = [](const OrtCustomOp*) { return 0; };
    OrtCustomOp::GetVariadicInputHomogeneity = [](const OrtCustomOp*) { return 0; };
    OrtCustomOp::GetVariadicOutputMinArity = [](const OrtCustomOp*) { return 0; };
    OrtCustomOp::GetVariadicOutputHomogeneity = [](const OrtCustomOp*) { return 0; };
  }

  const std::string op_name_;
  const std::string execution_provider_;

  std::vector<ONNXTensorElementDataType> input_types_;
  std::vector<ONNXTensorElementDataType> output_types_;
};

//////////////////////////// OrtCustomOpT1 ////////////////////////////////

template <typename... Args>
struct OrtCustomOpT1 : public OrtCustomOpBase {
  using ComputeFn = void (*)(Args...);
  using MyType = OrtCustomOpT1<Args...>;

  struct Kernel {
    size_t num_input_{};
    size_t num_output_{};
    ComputeFn compute_fn_{};
  };

  OrtCustomOpT1(const char* op_name,
                const char* execution_provider,
                ComputeFn compute_fn) : OrtCustomOpBase(op_name, execution_provider),
                                        compute_fn_(compute_fn) {
    ParseArgs<Args...>(input_types_, output_types_);

    OrtCustomOp::KernelCompute = [](void* op_kernel, OrtKernelContext* context) {
      auto kernel = reinterpret_cast<Kernel*>(op_kernel);
      std::vector<TensorPtr> tensors;
      auto t = CreateTuple<0, 0, Args...>(context, tensors, kernel->num_input_, kernel->num_output_);
      std::apply([kernel](Args const&... t_args) { kernel->compute_fn_(t_args...); }, t);
    };

    OrtCustomOp::CreateKernel = [](const OrtCustomOp* this_, const OrtApi* ort_api, const OrtKernelInfo* info) {
      auto kernel = std::make_unique<Kernel>();
      kernel->compute_fn_ = static_cast<const MyType*>(this_)->compute_fn_;
      ort_api->KernelInfo_GetInputCount(info, &kernel->num_input_);
      ort_api->KernelInfo_GetOutputCount(info, &kernel->num_output_);
      return reinterpret_cast<void*>(kernel.release());
    };

    OrtCustomOp::KernelDestroy = [](void* op_kernel) {
      delete reinterpret_cast<Kernel*>(op_kernel);
    };
  }

  ComputeFn compute_fn_;
};  // struct OrtCustomOpT1

/////////////////////////// OrtCustomOpT2 ///////////////////////////

template <typename CustomOp>
struct OrtCustomOpT2 : public OrtCustomOpBase {
  template <typename... Args>
  using CustomComputeFn = void (CustomOp::*)(Args...);
  using MyType = OrtCustomOpT2<CustomOp>;

  struct Kernel {
    size_t num_input_{};
    size_t num_output_{};
    std::unique_ptr<CustomOp> custom_op_;
  };

  OrtCustomOpT2(const char* op_name,
                const char* execution_provider) : OrtCustomOpBase(op_name,
                                                                  execution_provider) {
    init(&CustomOp::Compute);
  }

  template <typename... Args>
  void init(CustomComputeFn<Args...> custom_compute_fn) {
    ParseArgs<Args...>(input_types_, output_types_);

    OrtCustomOp::KernelCompute = [](void* op_kernel, OrtKernelContext* context) {
      auto kernel = reinterpret_cast<Kernel*>(op_kernel);
      std::vector<TensorPtr> tensors;
      auto t = CreateTuple<0, 0, Args...>(context, tensors, kernel->num_input_, kernel->num_output_);
      std::apply([kernel](Args const&... t_args) { kernel->custom_op_->Compute(t_args...); }, t);
    };

    OrtCustomOp::CreateKernel = [](const OrtCustomOp*, const OrtApi* ort_api, const OrtKernelInfo* info) {
      auto kernel = std::make_unique<Kernel>();
      ort_api->KernelInfo_GetInputCount(info, &kernel->num_input_);
      ort_api->KernelInfo_GetOutputCount(info, &kernel->num_output_);
      kernel->custom_op_ = std::make_unique<CustomOp>(ort_api, info);
      return reinterpret_cast<void*>(kernel.release());
    };

    OrtCustomOp::KernelDestroy = [](void* op_kernel) {
      delete reinterpret_cast<Kernel*>(op_kernel);
    };
  }
};  // struct OrtCustomOpT2

/////////////////////////// CreateCustomOp ////////////////////////////

template <typename... Args>
OrtCustomOp* CreateCustomOp(const char* op_name,
                            const char* execution_provider,
                            void (*custom_compute_fn)(Args...)) {
  using OrtCustomOpTPtr = OrtCustomOpT1<Args...>;
  return std::make_unique<OrtCustomOpTPtr>(op_name, execution_provider, custom_compute_fn).release();
}

template <typename CustomOp>
OrtCustomOp* CreateCustomOp(const char* op_name,
                            const char* execution_provider) {
  using OrtCustomOpTPtr = OrtCustomOpT2<CustomOp>;
  return std::make_unique<OrtCustomOpTPtr>(op_name, execution_provider).release();
}

}  // namespace Custom2
}  // namespace Ort
