// Copyright (c) 2016 - present Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include <cmath>
#include <cstddef>
#include <iostream>
#include <sstream>

#include "../../shared/gpubuf.h"
#include "rider.h"
#include "rocfft.h"
#include <boost/program_options.hpp>
namespace po = boost::program_options;

int main(int argc, char* argv[])
{
    // This helps with mixing output of both wide and narrow characters to the screen
    std::ios::sync_with_stdio(false);

    // Control output verbosity:
    int verbose;

    // hip Device number for running tests:
    int deviceId;

    // Number of performance trial samples
    int ntrial;

    // FFT parameters:
    rocfft_params params;

    // Declare the supported options.

    // clang-format doesn't handle boost program options very well:
    // clang-format off
    po::options_description opdesc("rocfft rider command line options");
    opdesc.add_options()("help,h", "produces this help message")
        ("version,v", "Print queryable version information from the rocfft library")
        ("device", po::value<int>(&deviceId)->default_value(0), "Select a specific device id")
        ("verbose", po::value<int>(&verbose)->default_value(0), "Control output verbosity")
        ("ntrial,N", po::value<int>(&ntrial)->default_value(1), "Trial size for the problem")
        ("notInPlace,o", "Not in-place FFT transform (default: in-place)")
        ("double", "Double precision transform (default: single)")
        ("transformType,t", po::value<rocfft_transform_type>(&params.transform_type)
         ->default_value(rocfft_transform_type_complex_forward),
         "Type of transform:\n0) complex forward\n1) complex inverse\n2) real "
         "forward\n3) real inverse")
        ( "batchSize,b", po::value<size_t>(&params.nbatch)->default_value(1),
          "If this value is greater than one, arrays will be used ")
        ( "itype", po::value<rocfft_array_type>(&params.itype)
          ->default_value(rocfft_array_type_unset),
          "Array type of input data:\n0) interleaved\n1) planar\n2) real\n3) "
          "hermitian interleaved\n4) hermitian planar")
        ( "otype", po::value<rocfft_array_type>(&params.otype)
          ->default_value(rocfft_array_type_unset),
          "Array type of output data:\n0) interleaved\n1) planar\n2) real\n3) "
          "hermitian interleaved\n4) hermitian planar")
        ("length",  po::value<std::vector<size_t>>(&params.length)->multitoken(), "Lengths.")
        ("istride", po::value<std::vector<size_t>>(&params.istride)->multitoken(), "Input strides.")
        ("ostride", po::value<std::vector<size_t>>(&params.ostride)->multitoken(), "Output strides.")
        ("idist", po::value<size_t>(&params.idist)->default_value(0),
         "Logical distance between input batches.")
        ("odist", po::value<size_t>(&params.odist)->default_value(0),
         "Logical distance between output batches.")
        ("isize", po::value<std::vector<size_t>>(&params.isize)->multitoken(),
         "Logical size of input buffer.")
        ("osize", po::value<std::vector<size_t>>(&params.osize)->multitoken(),
         "Logical size of output buffer.")
        ("ioffset", po::value<std::vector<size_t>>(&params.ioffset)->multitoken(), "Input offsets.")
        ("ooffset", po::value<std::vector<size_t>>(&params.ooffset)->multitoken(), "Output offsets.");
    // clang-format on

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, opdesc), vm);
    po::notify(vm);

    if(vm.count("help"))
    {
        std::cout << opdesc << std::endl;
        return 0;
    }

    if(vm.count("version"))
    {
        char v[256];
        rocfft_get_version_string(v, 256);
        std::cout << "version " << v << std::endl;
        return 0;
    }

    if(!vm.count("length"))
    {
        std::cout << "Please specify transform length!" << std::endl;
        std::cout << opdesc << std::endl;
        return 0;
    }

    params.placement
        = vm.count("notInPlace") ? rocfft_placement_notinplace : rocfft_placement_inplace;
    params.precision = vm.count("double") ? rocfft_precision_double : rocfft_precision_single;

    if(vm.count("notInPlace"))
    {
        std::cout << "out-of-place\n";
    }
    else
    {
        std::cout << "in-place\n";
    }

    if(vm.count("ntrial"))
    {
        std::cout << "Running profile with " << ntrial << " samples\n";
    }

    if(vm.count("length"))
    {
        std::cout << "length:";
        for(auto& i : params.length)
            std::cout << " " << i;
        std::cout << "\n";
    }

    if(vm.count("istride"))
    {
        std::cout << "istride:";
        for(auto& i : params.istride)
            std::cout << " " << i;
        std::cout << "\n";
    }
    if(vm.count("ostride"))
    {
        std::cout << "ostride:";
        for(auto& i : params.ostride)
            std::cout << " " << i;
        std::cout << "\n";
    }

    if(params.idist > 0)
    {
        std::cout << "idist: " << params.idist << "\n";
    }
    if(params.odist > 0)
    {
        std::cout << "odist: " << params.odist << "\n";
    }

    if(vm.count("ioffset"))
    {
        std::cout << "ioffset:";
        for(auto& i : params.ioffset)
            std::cout << " " << i;
        std::cout << "\n";
    }
    if(vm.count("ooffset"))
    {
        std::cout << "ooffset:";
        for(auto& i : params.ooffset)
            std::cout << " " << i;
        std::cout << "\n";
    }

    std::cout << std::flush;

    rocfft_setup();

    // Fixme: set the device id properly after the IDs are synced
    // bewteen hip runtime and rocm-smi.
    // HIP_V_THROW(hipSetDevice(deviceId), "set device failed!");

    check_set_iotypes(params.placement, params.transform_type, params.itype, params.otype);

    params.istride
        = compute_stride(params.ilength(),
                         params.istride,
                         params.placement == rocfft_placement_inplace
                             && params.transform_type == rocfft_transform_type_real_forward);
    params.ostride
        = compute_stride(params.olength(),
                         params.ostride,
                         params.placement == rocfft_placement_inplace
                             && params.transform_type == rocfft_transform_type_real_inverse);

    if(params.idist == 0)
    {
        params.idist
            = set_idist(params.placement, params.transform_type, params.length, params.istride);
    }
    if(params.odist == 0)
    {
        params.odist
            = set_odist(params.placement, params.transform_type, params.length, params.ostride);
    }

    if(params.isize.empty())
    {
        for(int i = 0; i < params.nibuffer(); ++i)
        {
            params.isize.push_back(params.nbatch * params.idist);
        }
    }
    if(params.osize.empty())
    {
        for(int i = 0; i < params.nobuffer(); ++i)
        {
            params.osize.push_back(params.nbatch * params.odist);
        }
    }

    if(!params.valid(verbose))
    {
        throw std::runtime_error("Invalid parameters, add --verbose=1 for detail");
    }

    if(verbose)
    {
        std::cout << params.str() << std::endl;
    }

    // Create FFT description
    rocfft_plan_description desc = NULL;
    LIB_V_THROW(rocfft_plan_description_create(&desc), "rocfft_plan_description_create failed");
    LIB_V_THROW(rocfft_plan_description_set_data_layout(desc,
                                                        params.itype,
                                                        params.otype,
                                                        params.ioffset.data(),
                                                        params.ooffset.data(),
                                                        params.istride_cm().size(),
                                                        params.istride_cm().data(),
                                                        params.idist,
                                                        params.ostride_cm().size(),
                                                        params.ostride_cm().data(),
                                                        params.odist),
                "rocfft_plan_description_data_layout failed");
    assert(desc != NULL);

    // Create the plan
    rocfft_plan plan = NULL;
    LIB_V_THROW(rocfft_plan_create(&plan,
                                   params.placement,
                                   params.transform_type,
                                   params.precision,
                                   params.length_cm().size(),
                                   params.length_cm().data(),
                                   params.nbatch,
                                   desc),
                "rocfft_plan_create failed");

    // Get work buffer size and allocated info-associated work buffer is necessary
    size_t workBufferSize = 0;
    LIB_V_THROW(rocfft_plan_get_work_buffer_size(plan, &workBufferSize),
                "rocfft_plan_get_work_buffer_size failed");
    rocfft_execution_info info = NULL;
    LIB_V_THROW(rocfft_execution_info_create(&info), "rocfft_execution_info_create failed");
    gpubuf wbuffer;
    if(workBufferSize > 0)
    {
        HIP_V_THROW(wbuffer.alloc(workBufferSize), "Creating intermediate Buffer failed");
        LIB_V_THROW(rocfft_execution_info_set_work_buffer(info, wbuffer.data(), workBufferSize),
                    "rocfft_execution_info_set_work_buffer failed");
    }

    // Input data:
    const auto gpu_input = compute_input(params);

    if(verbose > 1)
    {
        std::cout << "GPU input:\n";
        printbuffer(params.precision,
                    params.itype,
                    gpu_input,
                    params.ilength(),
                    params.istride,
                    params.nbatch,
                    params.idist,
                    params.ioffset);
    }

    // GPU input and output buffers:
    auto                ibuffer_sizes = params.ibuffer_sizes();
    std::vector<gpubuf> ibuffer(ibuffer_sizes.size());
    std::vector<void*>  pibuffer(ibuffer_sizes.size());
    for(unsigned int i = 0; i < ibuffer.size(); ++i)
    {
        HIP_V_THROW(ibuffer[i].alloc(ibuffer_sizes[i]), "Creating input Buffer failed");
        pibuffer[i] = ibuffer[i].data();
    }

    std::vector<gpubuf>  obuffer_data;
    std::vector<gpubuf>* obuffer = &obuffer_data;
    if(params.placement == rocfft_placement_inplace)
    {
        obuffer = &ibuffer;
    }
    else
    {
        auto obuffer_sizes = params.obuffer_sizes();
        obuffer_data.resize(obuffer_sizes.size());
        for(unsigned int i = 0; i < obuffer_data.size(); ++i)
        {
            HIP_V_THROW(obuffer_data[i].alloc(obuffer_sizes[i]), "Creating output Buffer failed");
        }
    }
    std::vector<void*> pobuffer(obuffer->size());
    for(unsigned int i = 0; i < obuffer->size(); ++i)
    {
        pobuffer[i] = obuffer->at(i).data();
    }

    // Warm up once:
    for(int idx = 0; idx < gpu_input.size(); ++idx)
    {
        HIP_V_THROW(
            hipMemcpy(
                pibuffer[idx], gpu_input[idx].data(), gpu_input[idx].size(), hipMemcpyHostToDevice),
            "hipMemcpy failed");
    }
    rocfft_execute(plan, pibuffer.data(), pobuffer.data(), info);

    // Run the transform several times and record the execution time:
    std::vector<double> gpu_time(ntrial);

    hipEvent_t start, stop;
    HIP_V_THROW(hipEventCreate(&start), "hipEventCreate failed");
    HIP_V_THROW(hipEventCreate(&stop), "hipEventCreate failed");
    for(int itrial = 0; itrial < gpu_time.size(); ++itrial)
    {
        // Copy the input data to the GPU:
        for(int idx = 0; idx < gpu_input.size(); ++idx)
        {
            HIP_V_THROW(hipMemcpy(pibuffer[idx],
                                  gpu_input[idx].data(),
                                  gpu_input[idx].size(),
                                  hipMemcpyHostToDevice),
                        "hipMemcpy failed");
        }

        HIP_V_THROW(hipEventRecord(start), "hipEventRecord failed");

        rocfft_execute(plan, pibuffer.data(), pobuffer.data(), info);

        HIP_V_THROW(hipEventRecord(stop), "hipEventRecord failed");
        HIP_V_THROW(hipEventSynchronize(stop), "hipEventSynchronize failed");

        float time;
        hipEventElapsedTime(&time, start, stop);
        gpu_time[itrial] = time;

        if(verbose > 2)
        {
            auto output = allocate_host_buffer(params.precision, params.otype, params.osize);
            for(int idx = 0; idx < output.size(); ++idx)
            {
                hipMemcpy(
                    output[idx].data(), pobuffer[idx], output[idx].size(), hipMemcpyDeviceToHost);
            }
            std::cout << "GPU output:\n";
            printbuffer(params.precision,
                        params.otype,
                        output,
                        params.olength(),
                        params.ostride,
                        params.nbatch,
                        params.odist,
                        params.ooffset);
        }
    }

    std::cout << "\nExecution gpu time:";
    for(const auto& i : gpu_time)
    {
        std::cout << " " << i;
    }
    std::cout << " ms" << std::endl;

    std::cout << "Execution gflops:  ";
    const double totsize
        = std::accumulate(params.length.begin(), params.length.end(), 1, std::multiplies<size_t>());
    const double k
        = ((params.itype == rocfft_array_type_real) || (params.otype == rocfft_array_type_real))
              ? 2.5
              : 5.0;
    const double opscount = (double)params.nbatch * k * totsize * log(totsize) / log(2.0);
    for(const auto& i : gpu_time)
    {
        std::cout << " " << opscount / (1e6 * i);
    }
    std::cout << std::endl;

    // Clean up:
    rocfft_plan_description_destroy(desc);
    rocfft_execution_info_destroy(info);
    rocfft_plan_destroy(plan);

    rocfft_cleanup();
}
