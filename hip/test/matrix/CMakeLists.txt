ginkgo_create_hip_test(fbcsr_kernels)
if (hipfft_FOUND)
    ginkgo_create_hip_test(fft_kernels)
endif()
