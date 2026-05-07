#include <stdio.h>
#include <cuda_runtime.h>

int main() {
    int count;
    cudaError_t err = cudaGetDeviceCount(&count);
    if (err != cudaSuccess) {
        printf("Error: %s\n", cudaGetErrorString(err));
        return 1;
    }
    printf("Found %d CUDA devices\n", count);
    for (int i = 0; i < count; i++) {
        cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, i);
        printf("Device %d: %s (SM %d.%d)\n", i, prop.name, prop.major, prop.minor);
    }
    return 0;
}
