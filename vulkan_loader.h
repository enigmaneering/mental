#ifndef VULKAN_LOADER_H
#define VULKAN_LOADER_H

#ifdef __cplusplus
extern "C" {
#endif

// Initialize Vulkan loader
// Returns 1 on success, 0 if Vulkan is not available
int vulkan_loader_init();

// Check if Vulkan is available
int vulkan_loader_available();

// Get error message if initialization failed
const char* vulkan_loader_error();

#ifdef __cplusplus
}
#endif

#endif // VULKAN_LOADER_H
