#ifndef OPENCL_LOADER_H
#define OPENCL_LOADER_H

#ifdef __cplusplus
extern "C" {
#endif

// Initialize OpenCL loader
// Returns 1 on success, 0 if OpenCL is not available
int opencl_loader_init();

// Check if OpenCL is available
int opencl_loader_available();

// Get error message if initialization failed
const char* opencl_loader_error();

#ifdef __cplusplus
}
#endif

#endif // OPENCL_LOADER_H
