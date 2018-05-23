/*******************************************************************************
 * Copyright (C) 2016 Advanced Micro Devices, Inc. All rights reserved.
 ******************************************************************************/



#include <vector>
#include <iostream>
#include <math.h>
#include <fftw3.h>

#include <hip/hip_runtime_api.h>
#include <hip/hip_vector_types.h>
#include "rocfft/rocfft.h"


int main()
{
    // For size N >= 8192, temporary buffer is required to allocated
	const size_t N = 64*2048;

	// FFTW reference compute
	// ==========================================

	std::vector<double2> cx(N);
	fftw_complex *in, *out;
	fftw_plan p;

	in = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * N);
	out = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * N);
	p = fftw_plan_dft_1d(N, in, out, FFTW_FORWARD, FFTW_ESTIMATE);

	for (size_t i = 0; i < N; i++)
	{
		cx[i].x = in[i][0] = i + (i%3) - (i%7);
		cx[i].y = in[i][1] = 0;
	}

	fftw_execute(p);

	fftw_destroy_plan(p);


	// rocfft gpu compute
	// ========================================

    rocfft_setup();

	size_t Nbytes = N * sizeof(double2);

	// Create HIP device object.
	double2 *x;
	hipMalloc(&x, Nbytes);

	//  Copy data to device
	hipMemcpy(x, &cx[0], Nbytes, hipMemcpyHostToDevice);

	// Create plan
	rocfft_plan plan = NULL;
	size_t length = N;
	rocfft_plan_create(&plan, rocfft_placement_inplace, rocfft_transform_type_complex_forward, rocfft_precision_double, 1, &length, 1, NULL);

	// Setup work buffer
	void *workBuffer = nullptr;
	size_t workBufferSize = 0;
	rocfft_plan_get_work_buffer_size(plan, &workBufferSize);

	// Setup exec info to pass work buffer to the library
	rocfft_execution_info info = nullptr;
	rocfft_execution_info_create(&info);

	if(workBufferSize > 0)
	{
            printf("size of workbuffer=%d\n", (int)workBufferSize);
        	hipMalloc(&workBuffer, workBufferSize);
	        rocfft_execution_info_set_work_buffer(info, workBuffer, workBufferSize);
	}

	// Execute plan
	rocfft_execute(plan, (void**) &x, NULL, info);
	hipDeviceSynchronize();

	// Destroy plan
	rocfft_plan_destroy(plan);

	if(workBuffer)
  	     	hipFree(workBuffer);

	rocfft_execution_info_destroy(info);

	// Copy result back to host
	std::vector<double2> y(N);
	hipMemcpy(&y[0], x, Nbytes, hipMemcpyDeviceToHost);

	// Compute normalized root mean square error
	double maxv = 0;
	double nrmse = 0;
	for (size_t i = 0; i < N; i++)
	{
		double dr = out[i][0] - y[i].x;
		double di = out[i][1] - y[i].y;

        printf("element %d: input %f, %f; FFTW result %f, %f; rocFFT result %f, %f \n", (int)i, cx[i].x, cx[i].y, out[i][0], out[i][1], y[i].x, y[i].y);
		maxv = fabs(out[i][0]) > maxv ? fabs(out[i][0]) : maxv;
		maxv = fabs(out[i][1]) > maxv ? fabs(out[i][1]) : maxv;

		nrmse += ((dr*dr) + (di*di));
	}
	nrmse /= (double)N;
	nrmse = sqrt(nrmse);
	nrmse /= maxv;

	std::cout << "normalized root mean square error (nrmse): " << nrmse << std::endl;

	// Deallocate buffers
	fftw_free(in);
	fftw_free(out);

    rocfft_cleanup();


	return 0;

}
