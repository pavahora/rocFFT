// Copyright (c) 2020 - present Advanced Micro Devices, Inc. All rights reserved.
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

#include <boost/scope_exit.hpp>
#include <gtest/gtest.h>
#include <math.h>
#include <stdexcept>
#include <utility>
#include <vector>

#include "../client_utils.h"
#include "accuracy_test.h"
#include "fftw_transform.h"
#include "gpubuf.h"
#include "rocfft.h"
#include "rocfft_against_fftw.h"

accuracy_test::cpu_fft_params accuracy_test::compute_cpu_fft(const std::vector<size_t>& length,
                                                             size_t                     nbatch,
                                                             rocfft_precision           precision,
                                                             rocfft_transform_type transform_type)
{
    // Check cache first - nbatch is a >= comparison because we compute
    // the largest batch size and cache it.  Smaller batch runs can
    // compare against the larger data.
    if(std::get<0>(last_cpu_fft) == length && std::get<2>(last_cpu_fft) == precision
       && std::get<3>(last_cpu_fft) == transform_type)
    {
        if(std::get<1>(last_cpu_fft) >= nbatch)
        {
            return std::get<4>(last_cpu_fft);
        }
        else
            // Something's unexpected with our test order - we should have
            // generated the bigger batch first.  Batch ranges provided to
            // the test suites need to be in descending order.
            abort();
    }

    const size_t dim = length.size();

    // Input cpu parameters:
    auto ilength = length;
    if(transform_type == rocfft_transform_type_real_inverse)
        ilength[dim - 1] = ilength[dim - 1] / 2 + 1;
    auto istride = compute_stride(ilength);
    auto itype   = contiguous_itype(transform_type);
    auto idist   = set_idist(rocfft_placement_notinplace, transform_type, length, istride);

    // Output cpu parameters:
    auto olength = length;
    if(transform_type == rocfft_transform_type_real_forward)
        olength[dim - 1] = olength[dim - 1] / 2 + 1;
    auto ostride = compute_stride(olength);
    auto odist   = set_odist(rocfft_placement_notinplace, transform_type, length, ostride);
    auto otype   = contiguous_otype(transform_type);

    if(verbose > 3)
    {
        std::cout << "CPU  params:\n";
        std::cout << "\tilength:";
        for(auto i : ilength)
            std::cout << " " << i;
        std::cout << "\n";
        std::cout << "\tcpu_istride:";
        for(auto i : istride)
            std::cout << " " << i;
        std::cout << "\n";
        std::cout << "\tcpu_idist: " << idist << std::endl;

        std::cout << "\tolength:";
        for(auto i : olength)
            std::cout << " " << i;
        std::cout << "\n";
        std::cout << "\tcpu_ostride:";
        for(auto i : ostride)
            std::cout << " " << i;
        std::cout << "\n";
        std::cout << "\tcpu_odist: " << odist << std::endl;
    }

    // Hook up the futures
    std::shared_future<fftw_data_t> input = std::async(std::launch::async, [=]() {
        return compute_input<fftwAllocator<char>>(precision, itype, length, istride, idist, nbatch);
    });

    if(verbose > 3)
    {
        std::cout << "CPU input:\n";
        printbuffer(precision, itype, input.get(), ilength, istride, nbatch, idist);
    }

    auto input_norm = std::async(std::launch::async, [=]() {
        auto ret_norm = norm(input.get(), ilength, nbatch, precision, itype, istride, idist);
        if(verbose > 2)
        {
            std::cout << "CPU Input Linf norm:  " << ret_norm.l_inf << "\n";
            std::cout << "CPU Input L2 norm:    " << ret_norm.l_2 << "\n";
        }
        return ret_norm;
    });

    std::shared_future<fftw_data_t> output      = std::async(std::launch::async, [=]() {
        // copy input, as FFTW may overwrite it
        auto input_copy = input.get();
        auto output     = fftw_via_rocfft(
            length, istride, ostride, nbatch, idist, odist, precision, transform_type, input_copy);
        if(verbose > 3)
        {
            std::cout << "CPU output:\n";
            printbuffer(precision, otype, output, olength, ostride, nbatch, odist);
        }
        return std::move(output);
    });
    std::shared_future<VectorNorms> output_norm = std::async(std::launch::async, [=]() {
        auto ret_norm = norm(output.get(), olength, nbatch, precision, otype, ostride, odist);
        if(verbose > 2)
        {
            std::cout << "CPU Output Linf norm: " << ret_norm.l_inf << "\n";
            std::cout << "CPU Output L2 norm:   " << ret_norm.l_2 << "\n";
        }
        return ret_norm;
    });

    cpu_fft_params ret;
    ret.ilength = ilength;
    ret.istride = istride;
    ret.itype   = itype;
    ret.idist   = idist;
    ret.olength = olength;
    ret.ostride = ostride;
    ret.otype   = otype;
    ret.odist   = odist;

    ret.input       = std::move(input);
    ret.input_norm  = std::move(input_norm);
    ret.output      = std::move(output);
    ret.output_norm = std::move(output_norm);

    // Cache our result
    std::get<0>(last_cpu_fft) = length;
    std::get<1>(last_cpu_fft) = nbatch;
    std::get<2>(last_cpu_fft) = precision;
    std::get<3>(last_cpu_fft) = transform_type;
    std::get<4>(last_cpu_fft) = ret;

    return std::move(ret);
}

// Compute a FFT using rocFFT and compare with the provided CPU reference computation.
void rocfft_transform(const rocfft_params&                  params,
                      const std::vector<size_t>&            cpu_istride,
                      const std::vector<size_t>&            cpu_ostride,
                      const size_t                          cpu_idist,
                      const size_t                          cpu_odist,
                      const rocfft_array_type               cpu_itype,
                      const rocfft_array_type               cpu_otype,
                      const std::shared_future<fftw_data_t> cpu_input,
                      const std::shared_future<fftw_data_t> cpu_output,
                      const size_t                          ramgb,
                      const std::shared_future<VectorNorms> cpu_output_norm)
{
    if(ramgb > 0 && params.needed_ram(verbose) > ramgb * 1e9)
    {
        if(verbose > 2)
        {
            std::cout << "skipped!" << std::endl;
        }
        return;
    }

    if(!params.valid(verbose))
    {
        // Invalid parameters; skip this test.
        return;
    }

    const size_t dim = params.length.size();

    if(verbose > 1)
    {
        std::cout << params.str() << std::flush;
    }

    rocfft_status fft_status = rocfft_status_success;

    // Create FFT description
    rocfft_plan_description desc = NULL;
    fft_status                   = rocfft_plan_description_create(&desc);
    EXPECT_TRUE(fft_status == rocfft_status_success) << "rocFFT description creation failure";
    fft_status = rocfft_plan_description_set_data_layout(desc,
                                                         params.itype,
                                                         params.otype,
                                                         params.ioffset.data(),
                                                         params.ooffset.data(),
                                                         params.istride_cm().size(),
                                                         params.istride_cm().data(),
                                                         params.idist,
                                                         params.ostride_cm().size(),
                                                         params.ostride_cm().data(),
                                                         params.odist);
    EXPECT_TRUE(fft_status == rocfft_status_success)
        << "rocFFT data layout failure: " << fft_status;

    // Create the plan
    rocfft_plan gpu_plan = NULL;
    fft_status           = rocfft_plan_create(&gpu_plan,
                                    params.placement,
                                    params.transform_type,
                                    params.precision,
                                    params.length_cm().size(),
                                    params.length_cm().data(),
                                    params.nbatch,
                                    desc);
    EXPECT_TRUE(fft_status == rocfft_status_success) << "rocFFT plan creation failure";

    // Create execution info
    rocfft_execution_info info = NULL;
    fft_status                 = rocfft_execution_info_create(&info);
    EXPECT_TRUE(fft_status == rocfft_status_success) << "rocFFT execution info creation failure";
    size_t workbuffersize = 0;
    fft_status            = rocfft_plan_get_work_buffer_size(gpu_plan, &workbuffersize);
    EXPECT_TRUE(fft_status == rocfft_status_success) << "rocFFT get buffer size get failure";

    // Sizes of individual input and output variables
    const size_t isize_t = var_size<size_t>(params.precision, params.itype);
    const size_t osize_t = var_size<size_t>(params.precision, params.otype);

    // Numbers of input and output buffers:
    const int nibuffer = params.nibuffer();
    const int nobuffer = params.nobuffer();

    // Check if the problem fits on the device; if it doesn't skip it.
    if(!vram_fits_problem(
           nibuffer * params.isize * isize_t,
           (params.placement == rocfft_placement_inplace) ? 0 : nobuffer * params.osize * osize_t,
           workbuffersize))
    {
        rocfft_plan_destroy(gpu_plan);
        rocfft_plan_description_destroy(desc);
        rocfft_execution_info_destroy(info);

        if(verbose)
        {
            std::cout << "Problem won't fit on device; skipped\n";
        }
        return;
    }

    hipError_t hip_status = hipSuccess;

    // Allocate work memory and associate with the execution info
    gpubuf wbuffer;
    if(workbuffersize > 0)
    {
        hip_status = wbuffer.alloc(workbuffersize);
        EXPECT_TRUE(hip_status == hipSuccess) << "hipMalloc failure for work buffer";
        fft_status = rocfft_execution_info_set_work_buffer(info, wbuffer.data(), workbuffersize);
        EXPECT_TRUE(fft_status == rocfft_status_success) << "rocFFT set work buffer failure";
    }

    // Formatted input data:
    auto gpu_input = allocate_host_buffer<fftwAllocator<char>>(
        params.precision, params.itype, params.length, params.istride, params.idist, params.nbatch);

    // Copy from contiguous_input to input.
    copy_buffers(cpu_input.get(),
                 gpu_input,
                 params.ilength(),
                 params.nbatch,
                 params.precision,
                 cpu_itype,
                 cpu_istride,
                 cpu_idist,
                 params.itype,
                 params.istride,
                 params.idist);

    if(verbose > 4)
    {
        std::cout << "GPU input:\n";
        printbuffer(params.precision,
                    params.itype,
                    gpu_input,
                    params.ilength(),
                    params.istride,
                    params.nbatch,
                    params.idist);
    }
    if(verbose > 5)
    {
        std::cout << "flat GPU input:\n";
        printbuffer_flat(params.precision, params.itype, gpu_input, params.idist);
    }

    // GPU input and output buffers:
    auto ibuffer_sizes = buffer_sizes(params.precision, params.itype, params.idist, params.nbatch);
    std::vector<gpubuf> ibuffer(ibuffer_sizes.size());
    std::vector<void*>  pibuffer(ibuffer_sizes.size());
    for(unsigned int i = 0; i < ibuffer.size(); ++i)
    {
        hip_status = ibuffer[i].alloc(ibuffer_sizes[i]);
        ASSERT_TRUE(hip_status == hipSuccess) << "hipMalloc failure for input buffer " << i
                                              << " size " << ibuffer_sizes[i] << params.str();
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
        auto obuffer_sizes
            = buffer_sizes(params.precision, params.otype, params.odist, params.nbatch);
        obuffer_data.resize(obuffer_sizes.size());
        for(unsigned int i = 0; i < obuffer_data.size(); ++i)
        {
            hip_status = obuffer_data[i].alloc(obuffer_sizes[i]);
            ASSERT_TRUE(hip_status == hipSuccess) << "hipMalloc failure for output buffer " << i
                                                  << " size " << obuffer_sizes[i] << params.str();
        }
    }
    std::vector<void*> pobuffer(obuffer->size());
    for(unsigned int i = 0; i < obuffer->size(); ++i)
    {
        pobuffer[i] = obuffer->at(i).data();
    }

    // Copy the input data to the GPU:
    for(int idx = 0; idx < gpu_input.size(); ++idx)
    {
        hip_status = hipMemcpy(ibuffer[idx].data(),
                               gpu_input[idx].data(),
                               gpu_input[idx].size(),
                               hipMemcpyHostToDevice);
        EXPECT_TRUE(hip_status == hipSuccess) << "hipMemcpy failure";
    }

    // Execute the transform:
    fft_status = rocfft_execute(gpu_plan, // plan
                                (void**)pibuffer.data(), // in buffers
                                (void**)pobuffer.data(), // out buffers
                                info); // execution info
    EXPECT_TRUE(fft_status == rocfft_status_success) << "rocFFT plan execution failure";

    // Copy the data back to the host:
    auto gpu_output = allocate_host_buffer<fftwAllocator<char>>(params.precision,
                                                                params.otype,
                                                                params.olength(),
                                                                params.ostride,
                                                                params.odist,
                                                                params.nbatch);
    for(int idx = 0; idx < gpu_output.size(); ++idx)
    {
        hip_status = hipMemcpy(gpu_output[idx].data(),
                               obuffer->at(idx).data(),
                               gpu_output[idx].size(),
                               hipMemcpyDeviceToHost);
        EXPECT_TRUE(hip_status == hipSuccess) << "hipMemcpy failure";
    }

    if(verbose > 2)
    {
        std::cout << "GPU output:\n";
        printbuffer(params.precision,
                    params.otype,
                    gpu_output,
                    params.olength(),
                    params.ostride,
                    params.nbatch,
                    params.odist);
    }
    if(verbose > 5)
    {
        std::cout << "flat GPU output:\n";
        printbuffer_flat(params.precision, params.otype, gpu_output, params.odist);
    }

    // Compute the Linfinity and L2 norm of the GPU output:
    std::shared_future<VectorNorms> gpu_norm = std::async(std::launch::async, [&]() {
        return norm(gpu_output,
                    params.olength(),
                    params.nbatch,
                    params.precision,
                    params.otype,
                    params.ostride,
                    params.odist);
    });

    // Compute the l-infinity and l-2 distance between the CPU and GPU output:
    std::vector<std::pair<size_t, size_t>> linf_failures;
    const auto                             total_length
        = std::accumulate(params.length.begin(), params.length.end(), 1, std::multiplies<size_t>());
    const double linf_cutoff
        = type_epsilon(params.precision) * cpu_output_norm.get().l_inf * log(total_length);
    auto diff = distance(cpu_output.get(),
                         gpu_output,
                         params.olength(),
                         params.nbatch,
                         params.precision,
                         cpu_otype,
                         cpu_ostride,
                         cpu_odist,
                         params.otype,
                         params.ostride,
                         params.odist,
                         linf_failures,
                         linf_cutoff);

    if(verbose > 1)
    {
        std::cout << "GPU output Linf norm: " << gpu_norm.get().l_inf << "\n";
        std::cout << "GPU output L2 norm:   " << gpu_norm.get().l_2 << "\n";
        std::cout << "GPU linf norm failures:";
        std::sort(linf_failures.begin(), linf_failures.end());
        for(const auto& i : linf_failures)
        {
            std::cout << " (" << i.first << "," << i.second << ")";
        }
        std::cout << std::endl;
    }

    EXPECT_TRUE(std::isfinite(gpu_norm.get().l_inf)) << params.str();
    EXPECT_TRUE(std::isfinite(gpu_norm.get().l_2)) << params.str();

    if(verbose > 1)
    {
        std::cout << "L2 diff: " << diff.l_2 << "\n";
        std::cout << "Linf diff: " << diff.l_inf << "\n";
    }

    // TODO: handle case where norm is zero?
    EXPECT_TRUE(diff.l_inf < linf_cutoff)
        << "Linf test failed.  Linf:" << diff.l_inf
        << "\tnormalized Linf: " << diff.l_inf / cpu_output_norm.get().l_inf
        << "\tcutoff: " << linf_cutoff << params.str();

    EXPECT_TRUE(diff.l_2 / cpu_output_norm.get().l_2
                < sqrt(log2(total_length)) * type_epsilon(params.precision))
        << "L2 test failed. L2: " << diff.l_2
        << "\tnormalized L2: " << diff.l_2 / cpu_output_norm.get().l_2
        << "\tepsilon: " << sqrt(log2(total_length)) * type_epsilon(params.precision)
        << params.str();

    rocfft_plan_destroy(gpu_plan);
    gpu_plan = NULL;
    rocfft_plan_description_destroy(desc);
    desc = NULL;
    rocfft_execution_info_destroy(info);
    info = NULL;
}

// Test for comparison between FFTW and rocFFT.
TEST_P(accuracy_test, vs_fftw)
{
    rocfft_params params;
    params.length                 = std::get<0>(GetParam());
    params.precision              = std::get<1>(GetParam());
    params.nbatch                 = std::get<2>(GetParam());
    params.istride                = std::get<3>(GetParam());
    params.ostride                = std::get<4>(GetParam());
    type_place_io_t type_place_io = std::get<5>(GetParam());
    params.transform_type         = std::get<0>(type_place_io);
    params.placement              = std::get<1>(type_place_io);
    params.itype                  = std::get<2>(type_place_io);
    params.otype                  = std::get<3>(type_place_io);

    // NB: Input data is row-major.

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

    params.idist
        = set_idist(params.placement, params.transform_type, params.length, params.istride);
    params.odist
        = set_odist(params.placement, params.transform_type, params.length, params.ostride);

    params.isize = params.nbatch * params.idist;
    params.osize = params.nbatch * params.odist;

    if(ramgb > 0)
    {
        // Estimate the amount of memory needed, and skip if it's more than we allow.

        // Host input, output, and input copy, gpu input and output: 5 buffers.
        // This test assumes that all buffers are contiguous; other cases are dealt with when they
        // are called.
        // FFTW may require work memory; this is not accounted for.
        size_t needed_ram
            = 5
              * std::accumulate(
                  params.length.begin(), params.length.end(), 1, std::multiplies<size_t>());

        // Account for precision and data type:
        if(params.transform_type != rocfft_transform_type_real_forward
           || params.transform_type != rocfft_transform_type_real_inverse)
        {
            needed_ram *= 2;
        }
        switch(params.precision)
        {
        case rocfft_precision_single:
            needed_ram *= 4;
            break;
        case rocfft_precision_double:
            needed_ram *= 8;
            break;
        }

        if(needed_ram > ramgb * 1e9)
        {
            GTEST_SKIP();
            return;
        }
    }
    auto cpu = accuracy_test::compute_cpu_fft(
        params.length, params.nbatch, params.precision, params.transform_type);

    // Set up GPU computations:
    if(verbose)
    {
        std::cout << params.str() << std::endl;
    }

    rocfft_transform(params,
                     cpu.istride,
                     cpu.ostride,
                     cpu.idist,
                     cpu.odist,
                     cpu.itype,
                     cpu.otype,
                     cpu.input,
                     cpu.output,
                     ramgb,
                     cpu.output_norm);

    auto cpu_input_norm = cpu.input_norm.get();
    ASSERT_TRUE(std::isfinite(cpu_input_norm.l_2));
    ASSERT_TRUE(std::isfinite(cpu_input_norm.l_inf));

    auto cpu_output_norm = cpu.output_norm.get();
    ASSERT_TRUE(std::isfinite(cpu_output_norm.l_2));
    ASSERT_TRUE(std::isfinite(cpu_output_norm.l_inf));

    SUCCEED();
}
