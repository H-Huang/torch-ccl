/*
 * Copyright (c) 2020-2021, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <ATen/record_function.h>
#include <ProcessGroupCCL.hpp>
#include <dispatch_stub.h>
#include <ipex.h>

namespace oneccl_bindings_for_pytorch
{

namespace {

// Check that all `tensors' have the same device and type and shape and
// are distributed across distinct GPUs if these are GPU tensors.
c10::DeviceType check_tensors_properties(const std::vector<at::Tensor>& tensors) {
  if (tensors.size() == 0) {
    throw std::runtime_error("Tensor list must be nonempty");
  }
  auto device_count = xpu::dpcpp::device_count();
  if (tensors.size() > static_cast<size_t>(device_count)) {
    throw std::runtime_error(
      "Tensor list mustn't be larger than the number of available GPUs");
  }

  const auto& first = tensors.front();
  auto dev_type = first.device().type();

  // Set for ensuring that tensors are on separate devices.
  std::unordered_set<decltype(first.get_device())> usedDevices;
  usedDevices.reserve(tensors.size());

  for (const auto& t : tensors) {
    if (t.is_sparse()) {
      throw std::runtime_error("Tensors must be dense");
    }
    if (t.scalar_type() != first.scalar_type()) {
      throw std::runtime_error("Tensors must have identical type");
    }
    if (t.sizes() != first.sizes()) {
      throw std::runtime_error("Tensors must have identical size");
    }
    if (!t.is_contiguous()) {
      throw std::runtime_error("Tensors must be contiguous");
    }
    if (dev_type != t.device().type()) {
      throw std::runtime_error("Tensors must be on the same device type");
    }
    const auto inserted = usedDevices.insert(t.get_device()).second;
    if (!inserted) {
      throw std::runtime_error("Tensors must be on distinct devices");
    }
  }

  return dev_type;
}

Comms& get_ccl_comms(c10d::ProcessGroupCCL& pg_ccl, const std::string& devices_key, const std::vector<at::Device>& devices) {

  RECORD_FUNCTION("oneccl_bindings_for_pytorch::xpu::get_ccl_comms", std::vector<c10::IValue>());
  // Sanity check
  if (devices_key.empty()) {
    throw std::runtime_error(
            "Not able to create/get the CCL Communicator since "
            "the devices are empty ");
  }

  if (devices.size() != 1) {
    throw std::runtime_error("Torch CCL only support one device per process now");
  }

  auto cached_comms = pg_ccl.ccl_member_->get_comms(devices_key);
  if (cached_comms) {
    return *cached_comms;
  }

  // Only support the symmetric distributed communication
  int total_rank_size = pg_ccl.getSize() * devices.size();
  int local_base_rank = pg_ccl.getRank() * devices.size();

  ccl::vector_class<ccl::pair_class<int, ccl::device>> devs_rank;
  std::vector<ccl::stream> ccl_streams;
  ccl_streams.reserve(devices.size());

  // Use the same queue for computation and communication.
  // TODO: IPEX doesn't support multiple queue for now. Copy engine requires a dedicate queue
  auto q = xpu::dpcpp::getCurrentDPCPPStream(devices[0].index()).dpcpp_queue();
  ccl_streams.push_back(ccl::create_stream(q));

  int rank = local_base_rank;
  devs_rank.emplace_back(rank, ccl::create_device(q.get_device()));

  auto ctx = ccl::create_context(q.get_context());
  auto dpcpp_comms = ccl::create_communicators(total_rank_size, devs_rank, ctx, pg_ccl.ccl_member_->get_kvs(pg_ccl.getRank(), *pg_ccl.store_));


  std::shared_ptr<Comms> dpcpp_comms_ptr = std::make_shared<Comms>(dpcpp_comms, ccl_streams);
  // Store the comms to cache
  pg_ccl.ccl_member_->add_comms(devices_key, dpcpp_comms_ptr);

  return *dpcpp_comms_ptr.get();
}

template <typename RunF, typename CommType, typename InputType, typename OutputType, typename attr_t>
class XPUWorkCCL : public CollectiveAsyncWorkCCL<RunF, CommType, InputType, OutputType, attr_t> {
public:
  XPUWorkCCL(const std::vector<InputType>& inputs,
             const std::vector<OutputType>& outputs,
             const RunF f,
             CommType& comms,
             attr_t& attr,
             std::chrono::milliseconds timeout,
             int rank,
             c10d::OpType opType,
             const char* profilingTitle,
             const c10::optional<std::vector<at::Tensor>>& inputTensors) :
             CollectiveAsyncWorkCCL<RunF, CommType, InputType, OutputType, attr_t>(
                     inputs, outputs, f, comms, attr, timeout, rank, opType, profilingTitle, inputTensors) {}

  void run() override {
    CollectiveAsyncWorkCCL<RunF, CommType, InputType, OutputType, attr_t>::run();
    // add SYCL running dependency communication -> computation.
  };

  // No explicitly synchronization.
  virtual ~XPUWorkCCL() {
    this->rets.clear();
  }

  // Waiting on the work's on XPU backend
  bool wait(std::chrono::milliseconds timeout) override {
    this->synchronizeInternal(timeout);
    // Check for errors and throw appropriate exception.
    this->checkAndThrowException();
    return true;
  }

private:

};

void execute(c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> work) {
//  if(work->recordFunctionBeforeCallback_){
//    work->recordFunctionBeforeCallback_();
//  }
  try {
    work->run();
  } catch (...) {
    work->finishAsyncWorkCCLError(std::current_exception());
    return;
  }

  work->finishAsyncWorkCCL();
}

} //namespace anonymous

class XPUCCLStubs final: public DispatchStub {

public:

  XPUCCLStubs() {}

  ~XPUCCLStubs() {}

protected:

  c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> allreduce_(std::vector<at::Tensor>& tensors,
                                                            const AllreduceOptions& opts,
                                                            ProcessGroupCCL& pg_ccl) override;


  c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> reduce_(std::vector<at::Tensor>& tensors,
                                                         const ReduceOptions& opts,
                                                         ProcessGroupCCL& pg_ccl) override;

  c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> broadcast_(std::vector<at::Tensor>& tensors,
                                                            const BroadcastOptions& opts,
                                                            ProcessGroupCCL& pg_ccl) override;

  c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> allgather_(std::vector<std::vector<at::Tensor>>& outputTensors,
                                                            std::vector<at::Tensor>& inputTensors,
                                                            const AllgatherOptions& opts,
                                                            ProcessGroupCCL& pg_ccl) override;

  c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> gather_(std::vector<std::vector<at::Tensor>>& outputTensors,
                                                         std::vector<at::Tensor>& inputTensors,
                                                         const GatherOptions& opts,
                                                         ProcessGroupCCL& pg) override;

  c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> alltoall_(std::vector<at::Tensor>& outputTensors,
                                                           std::vector<at::Tensor>& inputTensors,
                                                           const AllToAllOptions& opts,
                                                           ProcessGroupCCL& pg) override;

  c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> alltoall_base_(at::Tensor& outputTensor,
                                                                at::Tensor& inputTensor,
                                                                std::vector<int64_t>& outputSplitSizes,
                                                                std::vector<int64_t>& inputSplitSizes,
                                                                const AllToAllOptions& opts,
                                                                ProcessGroupCCL& pg) override;

  void reset() override {
  }

private:
};

struct RegisterXPUMethods {
  RegisterXPUMethods() {
    static XPUCCLStubs methods;
    DispatchStub::register_ccl_stub(c10::DeviceType::XPU, &methods);
  }
};

void checkGPUTensor(const at::Tensor& tensor)
{
//  TORCH_CHECK(!is_block_format(tensor), "ccl doesn't support block format tensor");
}

void checkGPUTensor(const std::vector<at::Tensor>& tensors)
{
  checkGPUTensor(tensors[0]);
}

c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> XPUCCLStubs::allreduce_(std::vector<at::Tensor>& tensors,
                                                                       const AllreduceOptions& opts,
                                                                       ProcessGroupCCL& pg_ccl) {
  checkGPUTensor(tensors);
  c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> work;
  work = collective<get_ccl_comms, XPUWorkCCL>(
    pg_ccl,
    tensors,
    tensors,
    [=](at::Tensor input,
        at::Tensor output,
        ccl::allreduce_attr attr,
        ccl::communicator& comm,
        ccl::stream& stream) {
      RECORD_FUNCTION("oneccl_bindings_for_pytorch::xpu::allreduce", std::vector<c10::IValue>({input}));

      ccl::event ret_evt;
      call_with_lock(c10d::ProcessGroupCCL::globalMutex, [&](){
          CCL_CHECK(ret_evt = ccl::allreduce(input.data_ptr(),
                                             output.data_ptr(),
                                             (size_t) input.numel(),
                                             cclDatatypes.at(input.scalar_type()),
                                             cclOps.at(opts.reduceOp),
                                             comm,
                                             stream,
                                             attr););
      });
      return ret_evt;
  },
  c10d::OpType::ALLREDUCE,
  "oneccl_bindings_for_pytorch::xpu_work::allreduce");

  work->debugName = std::string("xpu::allreduce");
  execute(work);

  return work;
}

c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> XPUCCLStubs::reduce_(std::vector<at::Tensor>& tensors,
                                                                    const ReduceOptions& opts,
                                                                    ProcessGroupCCL& pg_ccl) {
  checkGPUTensor(tensors);
  const int root = opts.rootRank * tensors.size() + opts.rootTensor;
  c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> work;
  work = collective<get_ccl_comms, XPUWorkCCL>(
    pg_ccl,
    tensors,
    tensors,
    [=](at::Tensor input,
        at::Tensor output,
        ccl::reduce_attr attr,
        ccl::communicator& comm,
        ccl::stream& stream) {
      RECORD_FUNCTION("oneccl_bindings_for_pytorch::xpu::reduce", std::vector<c10::IValue>{input});

      ccl::event ret_evt;
      call_with_lock(c10d::ProcessGroupCCL::globalMutex, [&]() {
        CCL_CHECK(ret_evt = ccl::reduce(input.data_ptr(),
                                output.data_ptr(),
                                (size_t) input.numel(),
                                cclDatatypes.at(input.scalar_type()),
                                cclOps.at(opts.reduceOp),
                                root,
                                comm,
                                stream););
      });
      return ret_evt;

  },
    c10d::OpType::REDUCE,
    "oneccl_bindings_for_pytorch::xpu_work::reduce");

  work->debugName = std::string("xpu::reduce");
  execute(work);

  return work;
}

c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> XPUCCLStubs::broadcast_(std::vector<at::Tensor>& tensors,
                                                                       const BroadcastOptions &opts,
                                                                       ProcessGroupCCL& pg_ccl) {
  checkGPUTensor(tensors);
  const int root = opts.rootRank * tensors.size() + opts.rootTensor;
  c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> work;
  work = collective<get_ccl_comms, XPUWorkCCL>(
    pg_ccl,
    tensors,
    tensors,
    [=](at::Tensor input,
        at::Tensor output,
        ccl::broadcast_attr attr,
        ccl::communicator& comm,
        ccl::stream& stream) {
      RECORD_FUNCTION("oneccl_bindings_for_pytorch::xpu::broadcast", std::vector<c10::IValue>({input}));

      ccl::event ret_evt;
      call_with_lock(c10d::ProcessGroupCCL::globalMutex, [&](){
          CCL_CHECK(ret_evt = ccl::broadcast(input.data_ptr(),
                                             (size_t) input.numel(),
                                             cclDatatypes.at(input.scalar_type()),
                                             root,
                                             comm,
                                             stream,
                                             attr));
      });
      return ret_evt;
    },
    c10d::OpType::BROADCAST,
    "oneccl_bindings_for_pytorch::xpu_work::broadcast");


  work->debugName = std::string("xpu::broadcast");
  execute(work);

  return work;
}


c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> XPUCCLStubs::allgather_(std::vector<std::vector<at::Tensor>>& outputTensors,
                                                                       std::vector<at::Tensor>& inputTensors,
                                                                       const AllgatherOptions& opts,
                                                                       ProcessGroupCCL& pg_ccl) {
  const int rank = pg_ccl.getRank();
  c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> work;
  work = collective<get_ccl_comms, XPUWorkCCL>(
    pg_ccl,
    inputTensors,
    outputTensors,
    [=](at::Tensor input,
        const std::vector<at::Tensor>& outputs,
        ccl::allgatherv_attr attr,
        ccl::communicator& comm,
        ccl::stream& stream) {
      RECORD_FUNCTION("oneccl_bindings_for_pytorch::xpu::allgather", std::vector<c10::IValue>({input}));

      ccl::event ret_evt;
      std::vector<size_t> recvCounts(outputs.size(), 0);
      std::transform(outputs.begin(), outputs.end(), recvCounts.begin(),
                     [](const at::Tensor& t) {
                          return t.numel();
                     });

      TORCH_CHECK((size_t)input.numel() == recvCounts[rank], "allgather: send and recv count doesn't match");
      std::vector<void*> recvBufs(outputs.size(), nullptr);
      std::transform(outputs.begin(), outputs.end(), recvBufs.begin(),
                     [](const at::Tensor& t) {
                        return t.data_ptr();
                     });

      call_with_lock(c10d::ProcessGroupCCL::globalMutex, [&]() {
        CCL_CHECK(ret_evt = ccl::allgatherv(input.data_ptr(),
                                  (size_t) input.numel(),
                                  recvBufs,
                                  recvCounts,
                                  cclDatatypes.at(input.scalar_type()),
                                  comm,
                                  stream););
      });

      return ret_evt;
    },
    c10d::OpType::ALLGATHER,
    "oneccl_bindings_for_pytorch::xpu_work::allgather");

  work->debugName = std::string("xpu::allgather");
  execute(work);

  return work;
}


c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> XPUCCLStubs::gather_(std::vector<std::vector<at::Tensor>>& outputTensors,
                                                                    std::vector<at::Tensor>& inputTensors,
                                                                    const GatherOptions& opts,
                                                                    ProcessGroupCCL& pg) {
  checkSingleTensor(inputTensors);
  c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> work;
  auto grp_size = pg.getSize();
  auto rank = pg.getRank();

  if (rank != opts.rootRank)
  {
    TORCH_CHECK(outputTensors.size() == 0,
                "gather: number of output tensors should be 0 "
                "for non-root");
  }
  else
  {
    TORCH_CHECK(outputTensors.size() == 1,
                "gather: multi-GPU collective is not supported");

    TORCH_CHECK(static_cast<size_t>(grp_size) == outputTensors[0].size(),
                "gather: number of output tensors should equal "
                "to the world size");
  }
  work = collective<get_ccl_comms, XPUWorkCCL>(
          pg,
          inputTensors,
          outputTensors,
          [=](at::Tensor input,
              const std::vector<at::Tensor>& outputs,
              ccl::alltoallv_attr attr,
              ccl::communicator& comm,
              ccl::stream& stream) {

              std::vector<size_t> sendCounts(grp_size, 0);
              std::vector<size_t> recvCounts(grp_size, 0);
              sendCounts[opts.rootRank] = input.numel();

              at::Tensor flatOutput;
              int64_t flatRecvCount = 0;
              bool isOutputFlat = false;

              if (rank == opts.rootRank)
              {
                isOutputFlat =
                        computeLengthsAndCheckAndGetFlat(outputs,
                                                         recvCounts, flatOutput, flatRecvCount);
                TORCH_CHECK(sendCounts[rank] == recvCounts[rank],
                            "gather: send and recv count doesn't match");
              }
              else
              {
                // a workaround for the oneCCL to pass the address checking
                flatOutput = at::empty({1}, input.options());
              }

              ccl::event ret_evt;
              CCL_DISPATCH_INTEGRAL_FLOATS_TYPES(input.scalar_type(), "gather", [&] {
                  call_with_lock(c10d::ProcessGroupCCL::globalMutex, [&](){
                      CCL_CHECK(ret_evt = ccl::alltoallv(input.data_ptr<scalar_t>(),
                                                         sendCounts,
                                                         flatOutput.data_ptr<scalar_t>(),
                                                         recvCounts,
                                                         cclDatatypes.at(flatOutput.scalar_type()),
                                                         comm,
                                                         stream););
                  });
              });

              // TODO : add to post and pre hooks
              if (rank == opts.rootRank)
              {
                if (!isOutputFlat)
                {
                  // TODO: add dependency instead of waiting explicitly.
                  ret_evt.wait();
                  auto flatOutputSplits =
                          flatOutput.split_with_sizes(c10::IntArrayRef((int64_t*)recvCounts.data(),
                                                                       recvCounts.size()), 0);

                  for (int i = 0; i < grp_size; i++)
                  {
                    outputs[i].view({-1}).copy_(flatOutputSplits[i]);
                  }
                }
              }

              return ret_evt;
          },
          c10d::OpType::GATHER,
          "oneccl_bindings_for_pytorch::xpu_work::gather");

  work->debugName = std::string("xpu::gather");
  execute(work);

  return work;
}

c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> XPUCCLStubs::alltoall_base_(at::Tensor& outputTensor,
                                                                           at::Tensor& inputTensor,
                                                                           std::vector<int64_t>& outputSplitSizes,
                                                                           std::vector<int64_t>& inputSplitSizes,
                                                                           const AllToAllOptions& opts,
                                                                           ProcessGroupCCL& pg){
  checkSingleTensorHelper(inputTensor);
  checkSingleTensorHelper(outputTensor);

  std::vector<at::Tensor> inputs{inputTensor};
  std::vector<at::Tensor> outputs{outputTensor};
  auto grp_size = pg.getSize();
  c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> work;
  if (outputSplitSizes.size() == 0 && inputSplitSizes.size() == 0){
    TORCH_CHECK(outputTensor.numel() == inputTensor.numel() &&
                outputTensor.scalar_type() == inputTensor.scalar_type(),
                "alltoall_base: tensors are not equal in size or data type");

    TORCH_CHECK(outputTensor.size(0) % grp_size == 0,
                "alltoall_base: tensor's dim 0 does not divide equally across group size");
    work = collective<get_ccl_comms, XPUWorkCCL>(
            pg,
            inputs,
            outputs,
            [=](at::Tensor input,
                at::Tensor output,
                ccl::alltoall_attr attr,
                ccl::communicator& comm,
                ccl::stream& stream) {
                ccl::event ret_evt;
                CCL_DISPATCH_INTEGRAL_FLOATS_TYPES(input.scalar_type(), "alltoall_base", [&] {
                    call_with_lock(c10d::ProcessGroupCCL::globalMutex, [&](){
                        CCL_CHECK(ret_evt = ccl::alltoall(input.data_ptr<scalar_t>(),
                                                          output.data_ptr<scalar_t>(),
                                                          (size_t)output.numel() / comm.size(),
                                                          cclDatatypes.at(output.scalar_type()),
                                                          comm,
                                                          stream,
                                                          attr););
                    });
                });

                return ret_evt;
            },
            c10d::OpType::ALLTOALL_BASE,
            "oneccl_bindings_for_pytorch::xpu_work::alltoall_base");
  }
  else{
    // Need alltoallv
    work = collective<get_ccl_comms, XPUWorkCCL>(
            pg,
            inputs,
            outputs,
            [=](at::Tensor input,
                at::Tensor output,
                ccl::alltoallv_attr attr,
                ccl::communicator& comm,
                ccl::stream& stream) {
                ccl::event ret_evt;
                CCL_DISPATCH_INTEGRAL_FLOATS_TYPES(input.scalar_type(), "alltoall_base", [&] {
                    c10d::checkSplitSizes(inputSplitSizes, input, grp_size);
                    c10d::checkSplitSizes(outputSplitSizes, output, grp_size);

                    std::vector<size_t> sendCounts(grp_size);
                    std::vector<size_t> recvCounts(grp_size);
                    bool inputSplitsEqual = inputSplitSizes.size() == 0;
                    bool outputSplitsEqual = outputSplitSizes.size() == 0;

                    size_t inLen = input.numel();
                    size_t outLen = output.numel();
                    if (inLen) inLen /= (inputSplitsEqual ? grp_size : input.size(0));
                    if (outLen) outLen /= (outputSplitsEqual ? grp_size : output.size(0));

                    for (int i = 0; i < grp_size; i++)
                    {
                      sendCounts[i] = (inputSplitsEqual ? inLen : inputSplitSizes[i] * inLen);
                      recvCounts[i] = (outputSplitsEqual ? outLen : outputSplitSizes[i] * outLen);
                    }

                    call_with_lock(c10d::ProcessGroupCCL::globalMutex, [&](){
                        CCL_CHECK(ret_evt = ccl::alltoallv(input.data_ptr<scalar_t>(),
                                                           sendCounts,
                                                           output.data_ptr<scalar_t>(),
                                                           recvCounts,
                                                           cclDatatypes.at(output.scalar_type()),
                                                           comm,
                                                           stream,
                                                           attr););
                    });
                });
                return ret_evt;
            },
            c10d::OpType::ALLTOALL_BASE,
            "oneccl_bindings_for_pytorch::xpu_work::alltoall_base");
  }

  work->debugName = std::string("xpu::alltoall_base");
  execute(work);

  return work;
}

c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> XPUCCLStubs::alltoall_(std::vector<at::Tensor>& outputTensors,
                                                                      std::vector<at::Tensor>& inputTensors,
                                                                      const AllToAllOptions& opts,
                                                                      ProcessGroupCCL& pg){
  c10::intrusive_ptr<ProcessGroupCCL::AsyncWorkCCL> work;
  auto grp_size = pg.getSize();

  std::vector<std::vector<at::Tensor>> outputTensors_list = {outputTensors};
  std::vector<std::vector<at::Tensor>> inputTensors_list = {inputTensors};
  work = collective<get_ccl_comms, XPUWorkCCL>(
          pg,
          inputTensors_list,
          outputTensors_list,
          [=](std::vector<at::Tensor> inputs,
              std::vector<at::Tensor> outputs,
              ccl::alltoallv_attr attr,
              ccl::communicator& comm,
              ccl::stream& stream) {

              at::Tensor flatInput;
              at::Tensor flatOutput;

              std::vector<size_t> sendCounts(grp_size);
              std::vector<size_t> recvCounts(grp_size);

              int64_t flatSendCount;
              int64_t flatRecvCount;

              bool isInputFlat =
                      computeLengthsAndCheckAndGetFlat(inputTensors, sendCounts, flatInput, flatSendCount);

              bool isOutputFlat =
                      computeLengthsAndCheckAndGetFlat(outputTensors, recvCounts, flatOutput, flatRecvCount);

              if (!isInputFlat)
              {
                auto flatInputSplits =
                        flatInput.split_with_sizes(c10::IntArrayRef((int64_t*)sendCounts.data(),
                                                                    sendCounts.size()), 0);

                for (int i = 0; i < grp_size; i++)
                {
                  flatInputSplits[i].copy_(inputs[i].view({-1}));
                }
              }

              ccl::event ret_evt;
              CCL_DISPATCH_INTEGRAL_FLOATS_TYPES(flatInput.scalar_type(), "xpu::alltoall", [&] {
                  call_with_lock(c10d::ProcessGroupCCL::globalMutex, [&](){
                      CCL_CHECK(ret_evt = ccl::alltoallv(flatInput.data_ptr<scalar_t>(),
                                                         sendCounts,
                                                         flatOutput.data_ptr<scalar_t>(),
                                                         recvCounts,
                                                         cclDatatypes.at(flatOutput.scalar_type()),
                                                         comm,
                                                         stream););
                  });

              });

              if (!isOutputFlat) {
                ret_evt.wait();
                auto flatOutputSplits =
                        flatOutput.split_with_sizes(c10::IntArrayRef((int64_t*)recvCounts.data(),
                                                                     recvCounts.size()), 0);

                for (int i = 0; i < grp_size; i++)
                {
                  outputs[i].view({-1}).copy_(flatOutputSplits[i]);
                }
              }
              return ret_evt;
          },
          c10d::OpType::ALLTOALL,
          "oneccl_bindings_for_pytorch::xpu_work::alltoall");

  work->debugName = std::string("xpu::alltoall");
  execute(work);

  return work;
}

RegisterXPUMethods xpu_register;

}