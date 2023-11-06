import argparse
import datetime
import gc
import itertools
import logging
import os
import sys
import time

import numpy as np
import onnx
import psutil
import torch
from benchmark_helper import setup_logger
from llama_inputs import (
    convert_inputs_for_ort,
    get_merged_sample_with_past_kv_inputs,
    get_msft_sample_inputs,
    get_sample_inputs,
    get_sample_with_past_kv_inputs,
    get_dml_sample_inputs,
)
from optimum.onnxruntime import ORTModelForCausalLM
from torch.profiler import ProfilerActivity, profile, record_function
from tqdm import trange
from transformers import LlamaConfig, LlamaForCausalLM, LlamaTokenizer

import onnxruntime as ort
from onnxruntime.transformers.benchmark_helper import measure_memory

logger = logging.getLogger(__name__)


# For determining whether the ONNX model can do both prompt generation and token generation or only one of the two
def get_ort_model_inputs_len(args, model):
    if args.benchmark_type in {"hf-pt-eager", "hf-pt-compile"}:
        return 0
    if args.benchmark_type == "hf-ort":
        try:
            # New Optimum export (https://github.com/huggingface/optimum/blob/888332364c2e0091da1fc974737c7e277af168bf/optimum/onnxruntime/modeling_ort.py#L268)
            return len(model.inputs_names)
        except Exception:
            # Old Optimum export (https://github.com/huggingface/optimum/blob/c5ad7f971cb0a494eac03dc0909f146725f999c5/optimum/onnxruntime/base.py#L54)
            return len(model.decoder.input_names)
    return len(model.get_inputs())


def get_inputs(args: argparse.Namespace, ort_model_inputs_len: int):
    init_inputs, iter_inputs = None, None

    # For past_present_share_buffer:
    # Set max_seq_len to 2048 for Hugging Face model since that is the default value
    # Set max_seq_len to 2048 for Microsoft model since that is the max value currently supported
    max_seq_len = 2048

    if args.benchmark_type in {"hf-pt-eager", "hf-pt-compile"}:
        init_inputs = get_sample_inputs(
            args.config,
            args.target_device,
            args.batch_size,
            args.sequence_length,
            return_dict=True,
        )
        iter_inputs = get_sample_with_past_kv_inputs(
            args.config,
            args.target_device,
            args.batch_size,
            args.sequence_length,
            use_fp16=args.use_fp16,
            return_dict=True,
        )

    elif args.benchmark_type == "hf-ort":
        if ort_model_inputs_len == 3:  # [input_ids, attention_mask, position_ids]
            # Using split models in Optimum (e.g. created by Optimum export)
            init_inputs = get_sample_inputs(
                args.config,
                args.target_device,
                args.batch_size,
                args.sequence_length,
                return_dict=True,
            )
            iter_inputs = get_sample_with_past_kv_inputs(
                args.config,
                args.target_device,
                args.batch_size,
                args.sequence_length,
                use_fp16=args.use_fp16,
                return_dict=True,
            )
        else:
            # Using merged model in Optimum (e.g. created by convert_to_onnx export)
            init_inputs = get_merged_sample_with_past_kv_inputs(
                args.config,
                args.target_device,
                args.batch_size,
                seq_len=args.sequence_length,
                past_seq_len=0,
                use_fp16=args.use_fp16,
                return_dict=True,
            )
            iter_inputs = get_merged_sample_with_past_kv_inputs(
                args.config,
                args.target_device,
                args.batch_size,
                seq_len=1,
                past_seq_len=args.sequence_length,
                use_fp16=args.use_fp16,
                return_dict=True,
            )

    elif args.benchmark_type == "ort-convert-to-onnx":
        # Microsoft export from convert_to_onnx
        init_inputs = get_merged_sample_with_past_kv_inputs(
            args.config,
            args.target_device,
            args.batch_size,
            seq_len=args.sequence_length,
            past_seq_len=0,
            use_fp16=args.use_fp16,
            return_dict=True,
        )
        iter_inputs = get_merged_sample_with_past_kv_inputs(
            args.config,
            args.target_device,
            args.batch_size,
            seq_len=1,
            past_seq_len=args.sequence_length,
            use_fp16=args.use_fp16,
            return_dict=True,
        )
        init_inputs = convert_inputs_for_ort(
            init_inputs,
            use_fp16=args.use_fp16,
            use_buffer_share=args.past_present_share_buffer,
            past_seq_len=0,
            max_seq_len=max_seq_len,
            device=args.device,
            device_id=args.device_id,
        )
        iter_inputs = convert_inputs_for_ort(
            iter_inputs,
            use_fp16=args.use_fp16,
            use_buffer_share=args.past_present_share_buffer,
            past_seq_len=args.sequence_length,
            max_seq_len=max_seq_len,
            device=args.device,
            device_id=args.device_id,
        )

    elif args.benchmark_type == "ort-msft":
        # Microsoft export from https://github.com/microsoft/Llama-2-Onnx
        split_kv = ort_model_inputs_len > 5  # original inputs: [x, attn_mask, k_cache, v_cache, pos]

        init_inputs = get_msft_sample_inputs(
            args.config,
            args.batch_size,
            past_seq_len=0,
            seq_len=args.sequence_length,
            use_fp16=args.use_fp16,
            split_kv=split_kv,
        )
        iter_inputs = get_msft_sample_inputs(
            args.config,
            args.batch_size,
            past_seq_len=args.sequence_length,
            seq_len=1,
            use_fp16=args.use_fp16,
            split_kv=split_kv,
        )
        init_inputs = convert_inputs_for_ort(
            init_inputs,
            use_fp16=args.use_fp16,
            use_buffer_share=args.past_present_share_buffer,
            past_seq_len=0,
            max_seq_len=max_seq_len,
            device=args.device,
            device_id=args.device_id,
        )
        iter_inputs = convert_inputs_for_ort(
            iter_inputs,
            use_fp16=args.use_fp16,
            use_buffer_share=args.past_present_share_buffer,
            past_seq_len=args.sequence_length,
            max_seq_len=max_seq_len,
            device=args.device,
            device_id=args.device_id,
        )

    elif args.benchmark_type == "ort-dml":
        # Olive-optimized model from https://github.com/microsoft/Olive/tree/user/pavignol/directml-llama-sample/examples/directml/llama_v2
        # specialized for DirectML
        init_inputs = get_dml_sample_inputs(
            args.config,
            args.batch_size,
            seq_len=args.sequence_length,
            max_seq_len=args.sequence_length,
            use_fp16=args.use_fp16,
            use_cache_branch=False,
        )
        iter_inputs = get_dml_sample_inputs(
            args.config,
            args.batch_size,
            seq_len=1,
            max_seq_len=args.sequence_length,
            use_fp16=args.use_fp16,
            use_cache_branch=True,
        )

    else:
        raise Exception("Unable to auto-detect inputs for provided model")

    return init_inputs, iter_inputs


def get_model(args: argparse.Namespace):
    model, sess_options = None, None
    start_time, end_time = None, None

    # There are multiple sources that the model could come from:
    # 1) Benchmark LLaMA-2 from unofficial source on Hugging Face
    # 2) Benchmark LLaMA-2 from official source on Hugging Face, which requires an authentication token
    # 3) Benchmark LLaMA-2 from local download of model
    # 4) Benchmark LLaMA-2 from Microsoft (already optimized, available at https://github.com/microsoft/Llama-2-Onnx)
    # 5) Benchmark LLaMA-2 from convert_to_onnx

    if args.benchmark_type in {"hf-pt-eager", "hf-pt-compile"}:
        source = args.hf_pt_dir_path if args.hf_pt_dir_path else args.model_name
        start_time = time.time()
        model = LlamaForCausalLM.from_pretrained(
            source,
            torch_dtype=torch.float16 if args.use_fp16 else torch.float32,
            use_auth_token=args.auth,
            use_cache=True,
        ).to(args.target_device)
        end_time = time.time()

        if args.benchmark_type == "hf-pt-compile":
            model = torch.compile(model)

    elif args.benchmark_type in {"hf-ort", "ort-msft", "ort-convert-to-onnx"}:
        sess_options = ort.SessionOptions()
        sess_options.enable_profiling = args.profile
        if args.verbose:
            sess_options.log_verbosity_level = 1
            sess_options.log_severity_level = 1

    elif args.benchmark_type == "ort-dml":
        sess_options = ort.SessionOptions()
        sess_options.enable_profiling = args.profile
        sess_options.add_free_dimension_override_by_name("max_seq_len", 256)
        sess_options.add_free_dimension_override_by_name("seq_len_increment", 1)
        sess_options.log_severity_level = 3
        if args.verbose:
            sess_options.log_verbosity_level = 1

    else:
        raise Exception(f"Cannot recognize {args.benchmark_type}")

    if args.benchmark_type == "hf-ort":
        # Optimum export or convert_to_onnx.py export
        provider = args.execution_provider[0] if type(args.execution_provider) is tuple else args.execution_provider
        provider_options = args.execution_provider[1] if type(args.execution_provider) is tuple else None

        decoder_file_name = None
        decoder_with_past_file_name = None
        for filename in os.listdir(args.hf_ort_dir_path):
            if ".onnx" not in filename or ".onnx_data" in filename or ".onnx.data" in filename:
                continue
            if "decoder_model" in filename or filename == "model.onnx":
                decoder_file_name = filename
            if "decoder_with_past_model" in filename:
                decoder_with_past_file_name = filename
            if "decoder_merged_model" in filename:
                decoder_file_name = filename
                decoder_with_past_file_name = filename

        start_time = time.time()
        model = ORTModelForCausalLM.from_pretrained(
            args.hf_ort_dir_path,
            decoder_file_name=decoder_file_name,
            decoder_with_past_file_name=decoder_with_past_file_name,
            use_auth_token=args.auth,
            use_io_binding=(args.device != "cpu"),
            use_merged=(True if decoder_file_name == "model.onnx" else None),
            provider=provider,
            provider_options=provider_options,
            session_options=sess_options,
        )
        end_time = time.time()

    if args.benchmark_type in {"ort-msft", "ort-convert-to-onnx", "ort-dml"}:
        # Ex: Microsoft export from https://github.com/microsoft/Llama-2-Onnx
        logger.info(f"Loading model from {args.ort_model_path}")
        start_time = time.time()
        model = ort.InferenceSession(
            args.ort_model_path,
            sess_options,
            providers=[args.execution_provider],
        )
        end_time = time.time()

    logger.info(f"Loaded model in {end_time - start_time} s")
    return model


def time_fn(args, fn, inputs):
    # Warm up
    warmup_range = (
        range(args.warmup_runs)
        if args.benchmark_type in {"ort-msft", "ort-convert-to-onnx", "ort-dml"}
        else trange(args.warmup_runs, file=sys.stdout, desc="Warm up")
    )

    if args.verbose:
        outputs = fn(inputs)
        logger.info(outputs)

    for _ in warmup_range:
        fn(inputs)

    # Benchmark
    if args.device == "dml":
        inputs.synchronize_outputs()
    elif args.device != "cpu":
        torch.cuda.synchronize()

    start_time = time.time()

    bench_range = (
        range(args.num_runs)
        if args.benchmark_type in {"ort-msft", "ort-convert-to-onnx", "ort-dml"}
        else trange(args.num_runs, file=sys.stdout, desc="Benchmark")
    )
    for _ in bench_range:
        fn(inputs)

    if args.device == "dml":
        inputs.synchronize_outputs()
    elif args.device != "cpu":
        torch.cuda.synchronize()

    end_time = time.time()

    # Newline print after trange in order to print metrics on new lines without progress bar on same line
    if args.benchmark_type not in {"ort-msft", "ort-convert-to-onnx", "ort-dml"}:
        logger.info("")

    latency = (end_time - start_time) / args.num_runs
    throughput = args.batch_size / latency

    logger.info(f"Batch Size: {args.batch_size}")
    logger.info(f"Sequence Length: {args.sequence_length}")
    logger.info(f"Latency: {latency} s")
    logger.info(f"Throughput: {throughput} tps")
    return


def profile_fn(args, fn, inputs, inputs_type):
    # Filename prefix format:
    # "b<batch-size>_s<sequence-length>_<benchmark-type>-<precision>-<device>_<inference-step>_<inputs-type>_<current-time>"
    prefix = f"b{args.batch_size}_s{args.sequence_length}_{args.benchmark_type.lower()}-{args.precision}-{args.device}_{fn.__name__.replace('_', '-')}_{inputs_type}_{datetime.datetime.now():%Y-%m-%d_%H:%M:%S}"
    filename = None

    if args.benchmark_type in {"hf-pt-eager", "hf-pt-compile"}:
        # Profile PyTorch kernels
        with profile(  # noqa: SIM117
            activities=[ProfilerActivity.CPU, ProfilerActivity.CUDA], record_shapes=True, profile_memory=True
        ) as prof:
            with record_function("model_inference"):
                fn(inputs)
        prof_data = prof.key_averages(group_by_stack_n=5).table(sort_by=args.pt_filter_by, row_limit=args.pt_num_rows)

        filename = os.path.join(args.log_folder, f"{prefix}.log")
        with open(filename, "w") as f:
            f.write(prof_data)

    else:
        # Profile ORT kernels
        fn(inputs)

        # Set new log name for ORT profile log generated
        filename = f"{prefix}.json"

    return filename


def measure_fn(args, fn, inputs):
    # Measure CPU usage
    pid = os.getpid()
    process = psutil.Process(pid)
    process.cpu_percent(interval=0.1)

    fn(inputs)
    logger.info(f"CPU usage: {process.cpu_percent(interval=None)}%")

    # Measure memory usage
    gc.collect()
    torch.cuda.empty_cache()
    measure_memory(is_gpu=(args.device != "cpu"), func=lambda: fn(inputs))

    # Flush output so memory usage is printed
    sys.stdout.flush()


def run_hf_inference(args, init_inputs, iter_inputs, model):
    # Inference steps to measure
    def get_logits(inputs):
        # Inference pass without decoding
        outputs = model(**inputs)
        return outputs

    # Examples of other inference steps that can be measured:
    # To use, uncomment the function and assign it to `generate_fn`

    # def get_pred_ids(inputs):
    #     # Inference pass with predicted token ids generation
    #     predicted_ids = model.generate(**inputs)
    #     return predicted_ids

    # def gen_and_dec(inputs):
    #     # Inference pass with generation and decoding
    #     predicted_ids = get_pred_ids(inputs)
    #     transcription = []
    #     for bs in range(args.batch_size):
    #         for rs in range(args.num_return_sequences):
    #             transcription.append(
    #                 args.tokenizer.batch_decode(
    #                     predicted_ids[bs * args.num_return_sequences + rs], skip_special_tokens=True
    #                 )[0]
    #             )
    #     return transcription

    generate_fn = get_logits

    if args.benchmark_type == "hf-pt-compile":
        # Run forward pass once with each set of inputs to process through Dynamo
        generate_fn(init_inputs)
        generate_fn(iter_inputs)

    if args.profile:
        new_logname = profile_fn(args, generate_fn, init_inputs, "prompt")
        if args.benchmark_type == "hf-ort":
            # Turn profiling off to stop appending to log
            old_logname = model.decoder.session.end_profiling()
            logger.warning(f"Renaming {old_logname} to {new_logname}")
            os.rename(old_logname, os.path.join(args.log_folder, new_logname))

        new_logname = profile_fn(args, generate_fn, iter_inputs, "token")
        if args.benchmark_type == "hf-ort":
            # Turn profiling off to stop appending to log
            old_logname = model.decoder_with_past.session.end_profiling()
            logger.warning(f"Renaming {old_logname} to {new_logname}")
            os.rename(old_logname, os.path.join(args.log_folder, new_logname))

        return

    # PyTorch evaluations
    logger.info("\nEvaluating `model(inputs)` step to get past_key_values")
    time_fn(args, generate_fn, init_inputs)
    measure_fn(args, generate_fn, init_inputs)

    logger.info("\nEvaluating `model(inputs)` step with past_key_values")
    time_fn(args, generate_fn, iter_inputs)
    measure_fn(args, generate_fn, iter_inputs)


def run_ort_inference(args, init_inputs, iter_inputs, model):
    def prepare_ort_inputs(inputs):
        # Check that all model inputs will be provided
        model_inputs = set(map(lambda model_input: model_input.name, model.get_inputs()))
        user_inputs = set(inputs.keys())
        missing_inputs = model_inputs - user_inputs
        if len(missing_inputs):
            logger.error(f"The following model inputs are missing: {missing_inputs}")
            raise Exception("There are missing inputs to the model. Please add them and try again.")

        # Remove unnecessary inputs from model inputs
        unnecessary_inputs = user_inputs - model_inputs
        if len(unnecessary_inputs):
            for unnecessary_input in unnecessary_inputs:
                logger.info(f"Removing unnecessary input '{unnecessary_input}' from user provided inputs")
                del inputs[unnecessary_input]

        # Add IO bindings for non-CPU execution providers
        if args.device != "cpu":
            io_binding = model.io_binding()

            for k, v in inputs.items():
                if args.past_present_share_buffer:
                    # Bind all OrtValue inputs to device
                    io_binding.bind_ortvalue_input(k, v)
                else:
                    io_binding.bind_cpu_input(k, v)

            for output in model.get_outputs():
                name = output.name
                if args.past_present_share_buffer and ("out" in name or "present" in name):
                    # Bind present KV cache outputs to OrtValue with buffer sharing
                    io_binding.bind_ortvalue_output(
                        name, inputs[name.replace("out", "cache").replace("present", "past_key_values")]
                    )
                else:
                    io_binding.bind_output(name, device_type=args.device, device_id=args.device_id)

            return io_binding

        return inputs

    def with_io_binding(io_binding):
        # Inference pass with IO binding
        model.run_with_iobinding(io_binding)

    def without_io_binding(inputs):
        # Inference pass without IO binding
        outputs = model.run(None, inputs)
        return outputs

    generate_fn = with_io_binding if args.device != "cpu" else without_io_binding

    if args.profile:
        ort_init_inputs = prepare_ort_inputs(init_inputs)
        new_logname = profile_fn(args, generate_fn, ort_init_inputs, "prompt")

        # Turn profiling off to stop appending to log file
        old_logname = model.end_profiling()
        logger.warning(f"Renaming {old_logname} to {new_logname}")
        os.rename(old_logname, os.path.join(args.log_folder, new_logname))

        # Re-initialize model for new log file instead of appending to old log file
        model = get_model(args)
        ort_iter_inputs = prepare_ort_inputs(iter_inputs)
        new_logname = profile_fn(args, generate_fn, ort_iter_inputs, "token")

        # Turn profiling off to stop appending to log
        old_logname = model.end_profiling()
        logger.warning(f"Renaming {old_logname} to {new_logname}")
        os.rename(old_logname, os.path.join(args.log_folder, new_logname))
        return

    # ORT evaluations
    logger.info("\nEvaluating `model(inputs)` step to get past_key_values")
    ort_init_inputs = prepare_ort_inputs(init_inputs)
    time_fn(args, generate_fn, ort_init_inputs)
    measure_fn(args, generate_fn, ort_init_inputs)

    logger.info("\nEvaluating `model(inputs)` step with past_key_values")
    ort_iter_inputs = prepare_ort_inputs(iter_inputs)
    time_fn(args, generate_fn, ort_iter_inputs)
    measure_fn(args, generate_fn, ort_iter_inputs)


def run_inference(args, init_inputs, iter_inputs, model):
    if args.benchmark_type in {"hf-pt-eager", "hf-pt-compile", "hf-ort"}:
        run_hf_inference(args, init_inputs, iter_inputs, model)
    elif args.benchmark_type in {"ort-msft", "ort-convert-to-onnx", "ort-dml"}:
        run_ort_inference(args, init_inputs, iter_inputs, model)
    else:
        raise Exception(f"Cannot recognize {args.benchmark_type}")


def get_args():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "-bt",
        "--benchmark-type",
        type=str,
        required=True,
        choices=["hf-pt-eager", "hf-pt-compile", "hf-ort", "ort-msft", "ort-convert-to-onnx", "ort-dml"],
    )
    parser.add_argument(
        "-m",
        "--model-name",
        type=str,
        required=True,
        help="Hugging Face name of model (e.g. 'meta-llama/Llama-2-7b-hf')",
    )
    parser.add_argument(
        "-a", "--auth", default=False, action="store_true", help="Use Hugging Face authentication token to access model"
    )

    # Args for choosing the model
    parser.add_argument(
        "-p",
        "--precision",
        required=True,
        type=str,
        default="fp32",
        choices=["int4", "int8", "fp16", "fp32"],
        help="Precision for model. For ONNX models, the model's precision should be set before running this script.",
    )
    parser.add_argument(
        "--hf-pt-dir-path",
        type=str,
        default="",
        help="Path to directory containing all PyTorch files (e.g. tokenizer, PyTorch model)",
    )
    parser.add_argument(
        "--hf-ort-dir-path",
        type=str,
        default="",
        help="Path to directory containing all ONNX files (e.g. tokenizer, decoder_merged, decoder, decoder_with_past)",
    )
    parser.add_argument(
        "--ort-model-path",
        type=str,
        default="",
        help="Path to ONNX model",
    )

    # Args for running and evaluating the model
    parser.add_argument(
        "-b",
        "--batch-sizes",
        default="1 2",
    )
    parser.add_argument(
        "-s",
        "--sequence-lengths",
        default="8 16 32 64 128 256 512",
    )
    parser.add_argument(
        "-d",
        "--device",
        type=str,
        default="cuda" if torch.cuda.is_available() else "cpu",
        choices=["cpu", "cuda", "rocm", "dml"],
    )
    parser.add_argument("-id", "--device-id", type=int, default=0)
    parser.add_argument("-w", "--warmup-runs", type=int, default=5)
    parser.add_argument("-n", "--num-runs", type=int, default=10)
    parser.add_argument("--seed", type=int, default=2)

    # Args for decoding logic
    parser.add_argument("--max-length", type=int, default=32)
    parser.add_argument("--num-return-sequences", type=int, default=1)

    # Args for accessing detailed info
    parser.add_argument("--profile", default=False, action="store_true")
    parser.add_argument(
        "--pt-filter-by", type=str, default="self_cpu_time_total", help="What to filter PyTorch profiler by"
    )
    parser.add_argument("--pt-num-rows", type=int, default=1000, help="Number of rows for PyTorch profiler to display")
    parser.add_argument("--verbose", default=False, action="store_true")
    parser.add_argument("--log-folder", type=str, default=os.path.join("."), help="Folder to cache log files")

    args = parser.parse_args()

    # Set seed properties
    np.random.seed(args.seed)
    torch.manual_seed(args.seed)

    # Set runtime properties
    if "ort" in args.benchmark_type:
        if args.device.upper() == "DML":
            setattr(args, "execution_provider", "DmlExecutionProvider")  # noqa: B010
        else:
            setattr(args, "execution_provider", f"{args.device.upper()}ExecutionProvider")  # noqa: B010
        if args.execution_provider == "CUDAExecutionProvider":
            args.execution_provider = (args.execution_provider, {"device_id": args.device_id})
        elif args.execution_provider == "ROCMExecutionProvider":
            args.execution_provider = (args.execution_provider, {"device_id": args.device_id})
            args.device = "cuda"

    # Check that paths have been specified for any benchmarking with ORT
    if args.benchmark_type == "hf-ort":
        assert args.hf_ort_dir_path, "Please specify a path to `--hf-ort-dir-path`"
    if args.benchmark_type in {"ort-msft", "ort-convert-to-onnx", "ort-dml"}:
        assert args.ort_model_path, "Please specify a path to `--ort-model-path`"

    args.batch_sizes = args.batch_sizes.split(" ")
    args.sequence_lengths = args.sequence_lengths.split(" ")

    # Use FP32 precision for FP32, INT8, INT4 CPU models, use FP16 precision for FP16 and INT4 GPU models
    args.precision = (
        "fp32" if args.precision in {"int8", "fp32"} or (args.precision == "int4" and args.device == "cpu") else "fp16"
    )

    # Check that only one (batch_size, sequence_length) combination is set for profiling
    if args.profile:
        assert (
            len(args.batch_sizes) == 1 and len(args.sequence_lengths) == 1
        ), "Please provide only one (batch_size, sequence_length) combination for profiling"

    return args


def main():
    args = get_args()
    setup_logger(args.verbose)
    logger.info(args.__dict__)
    torch.backends.cudnn.benchmark = True

    tokenizer = LlamaTokenizer.from_pretrained(args.model_name)
    config = LlamaConfig.from_pretrained(args.model_name)
    target_device = f"cuda:{args.device_id}" if args.device != "cpu" else args.device
    use_fp16 = args.precision == "fp16"

    setattr(args, "tokenizer", tokenizer)  # noqa: B010
    setattr(args, "config", config)  # noqa: B010
    setattr(args, "target_device", target_device)  # noqa: B010
    setattr(args, "use_fp16", use_fp16)  # noqa: B010

    # Get model and model info
    model = get_model(args)
    ort_model_inputs_len = get_ort_model_inputs_len(args, model)

    # Check if past_present_share_buffer can be enabled (only for FP16 models with GQA)
    if args.benchmark_type in {"ort-convert-to-onnx", "ort-msft"}:
        onnx_model = onnx.load_model(args.ort_model_path, load_external_data=False)
        gqa_nodes = list(filter(lambda node: node.op_type == "GroupQueryAttention", onnx_model.graph.node))

        use_buffer_share = use_fp16 and len(gqa_nodes) > 0 and args.device != "cpu"
        setattr(args, "past_present_share_buffer", use_buffer_share)  # noqa: B010
    else:
        setattr(args, "past_present_share_buffer", False)  # noqa: B010

    # Measure prompt cost (init_inputs) and generated token cost (iter_inputs)
    for batch_size, sequence_length in itertools.product(args.batch_sizes, args.sequence_lengths):
        logger.info(f"\nBatch size = {batch_size} and sequence length = {sequence_length}...")
        setattr(args, "batch_size", int(batch_size))  # noqa: B010
        setattr(args, "sequence_length", int(sequence_length))  # noqa: B010

        init_inputs, iter_inputs = get_inputs(args, ort_model_inputs_len)
        run_inference(args, init_inputs, iter_inputs, model)


if __name__ == "__main__":
    main()
