#include "gfx_backend.h"
#include "gfx_draw_batch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef AOT_ANDROID_APP
/* ---- host stub: no window; logs frames so the D3D11 layer can be tested anywhere ---- */
struct AOT_PRESENT_TEXTURE { uint32_t width, height; uint8_t *rgba; };
struct AOT_PRESENT_SAMPLER { uint32_t filter, address[3]; };
static unsigned frames;
int aot_present_init(void) { fprintf(stderr, "[gfx] host stub backend (1280x720)\n"); return 0; }
void aot_present_size(uint32_t *w, uint32_t *h) { *w = 1280; *h = 720; }
int aot_present_texture_create(uint32_t width, uint32_t height, const uint8_t *rgba,
                               AOT_PRESENT_TEXTURE **out) {
  if (!width || !height || !rgba || !out || (size_t)width > SIZE_MAX / 4 / height) return -1;
  AOT_PRESENT_TEXTURE *texture = calloc(1, sizeof *texture);
  if (!texture) return -1;
  texture->rgba = malloc((size_t)width * height * 4);
  if (!texture->rgba) { free(texture); return -1; }
  memcpy(texture->rgba, rgba, (size_t)width * height * 4);
  texture->width = width; texture->height = height; *out = texture;
  return 0;
}
void aot_present_texture_destroy(AOT_PRESENT_TEXTURE *texture) {
  if (texture) { free(texture->rgba); free(texture); }
}
int aot_present_sampler_create(uint32_t filter, uint32_t address_u, uint32_t address_v,
                               uint32_t address_w, AOT_PRESENT_SAMPLER **out) {
  if (!out) return -1;
  AOT_PRESENT_SAMPLER *sampler = calloc(1, sizeof *sampler);
  if (!sampler) return -1;
  sampler->filter = filter; sampler->address[0] = address_u;
  sampler->address[1] = address_v; sampler->address[2] = address_w;
  *out = sampler;
  return 0;
}
void aot_present_sampler_destroy(AOT_PRESENT_SAMPLER *sampler) { free(sampler); }
void aot_present_clear(const float c[4]) {
  frames++; if (frames == 1 || frames % 30 == 0 || getenv("AOT_GFX_VERBOSE")) { fflush(stdout); fprintf(stderr, "[gfx] present #%u clear=(%.3f,%.3f,%.3f,%.3f)\n", frames, c[0], c[1], c[2], c[3]); }
}
int aot_present_draw_batch(const float rgba[4], const AOT_GFX_DRAW_BATCH *batch,
                           AOT_PRESENT_FRAME_STATS *stats) {
  if (!rgba || !batch) return -1;
  if (stats) memset(stats, 0, sizeof *stats);
  if (aot_gfx_draw_batch_count(batch)) {
    fprintf(stderr, "[gfx] host presentation backend does not render shader draw batches\n");
    return -1;
  }
  aot_present_clear(rgba);
  return 0;
}
int aot_present_draw(const float rgba[4], const uint32_t *vertex_spirv, size_t vertex_spirv_size,
                     const uint32_t *pixel_spirv, size_t pixel_spirv_size, const void *vertices,
                     uint32_t vertex_stride, const AOT_PRESENT_VERTEX_ATTRIBUTE *attributes,
                     uint32_t attribute_count, uint32_t vertex_count, uint32_t first_vertex,
                     const AOT_PRESENT_TEXTURE *texture, const AOT_PRESENT_SAMPLER *sampler,
                     const void *vertex_constants, size_t vertex_constants_size) {
  (void)vertex_spirv; (void)vertex_spirv_size; (void)pixel_spirv; (void)pixel_spirv_size;
  (void)vertices; (void)vertex_stride; (void)attributes; (void)attribute_count; (void)vertex_count; (void)first_vertex;
  (void)texture; (void)sampler; (void)vertex_constants; (void)vertex_constants_size;
  if (!rgba) return -1;
  aot_present_clear(rgba);
  if (vertex_count) {
    fprintf(stderr, "[gfx] host presentation backend does not render shaders\n");
    return -1;
  }
  return 0;
}
void aot_present_graphics_cleanup(void) {}
#else
/* ---- Android: Vulkan swapchain, clear-colour present ---- */
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <stdarg.h>
static void gfx_log(const char *format, ...) {
  char message[512];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof message, format, args);
  va_end(args);
  __android_log_print(ANDROID_LOG_INFO, "aotgfx", "%s", message);
  fprintf(stderr, "[aotgfx] %s\n", message);
}
#define LG(...) gfx_log(__VA_ARGS__)
#define CK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { LG("FAILED %s -> %d (line %d)", #x, (int)_r, __LINE__); return -1; } } while (0)
static ANativeWindow *g_win; static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
void aot_android_set_window(ANativeWindow *w) { pthread_mutex_lock(&g_mu); g_win = w; pthread_mutex_unlock(&g_mu); }
static VkInstance inst; static VkSurfaceKHR surf; static VkPhysicalDevice pd; static VkDevice dev; static uint32_t qf; static VkQueue q;
static VkSwapchainKHR sc; static VkExtent2D ext; static VkFormat color_format; static uint32_t nimg; static VkImage img[8]; static VkImageView views[8]; static VkFramebuffer fb[8]; static VkRenderPass render_pass; static VkPipelineLayout pipeline_layout; static VkSemaphore rel[8], acq; static VkFence fence; static VkCommandPool pool; static VkCommandBuffer cb; static int ok, supports_spirv16, supports_vulkan_memory_model;
static VkImage depth_img[8]; static VkDeviceMemory depth_mem[8]; static VkImageView depth_views[8]; static VkFormat depth_format;
static VkDescriptorSetLayout resource_layout; static VkDescriptorPool descriptor_pool;
struct AOT_PRESENT_TEXTURE { VkImage image; VkDeviceMemory memory; VkImageView view; uint32_t width, height; };
struct AOT_PRESENT_SAMPLER { VkSampler sampler; };
typedef struct {
  VkPipeline pipeline;
  VkShaderModule vs_module, ps_module;
  const uint32_t *vs, *ps;
  size_t vs_size, ps_size;
  uint32_t strides[AOT_GFX_MAX_VERTEX_STREAMS];
  uint32_t attribute_count;
  AOT_PRESENT_VERTEX_ATTRIBUTE attributes[AOT_GFX_MAX_DRAW_ATTRIBUTES];
  uint8_t alpha_blend, cull_mode, front_counter_clockwise;
} PipelineCacheEntry;
static PipelineCacheEntry *pipeline_cache;
static size_t pipeline_cache_count, pipeline_cache_capacity;
static VkDeviceSize uniform_alignment = 1;
static uint32_t max_uniform_range;
static int find_memory_type(uint32_t bits, VkMemoryPropertyFlags flags, uint32_t *index);
static void destroy_pipeline_cache(void);
static int create_depth_attachment(uint32_t width, uint32_t height, VkImage *image,
                                   VkDeviceMemory *memory, VkImageView *view) {
  VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
  ii.imageType = VK_IMAGE_TYPE_2D; ii.format = depth_format;
  ii.extent = (VkExtent3D){ width, height, 1 }; ii.mipLevels = 1; ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateImage(dev, &ii, 0, image) != VK_SUCCESS) return -1;
  VkMemoryRequirements requirements; vkGetImageMemoryRequirements(dev, *image, &requirements);
  uint32_t type;
  if (!find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
    vkDestroyImage(dev, *image, 0); *image = VK_NULL_HANDLE; return -1;
  }
  VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  ai.allocationSize = requirements.size; ai.memoryTypeIndex = type;
  if (vkAllocateMemory(dev, &ai, 0, memory) != VK_SUCCESS ||
      vkBindImageMemory(dev, *image, *memory, 0) != VK_SUCCESS) {
    vkDestroyImage(dev, *image, 0);
    if (*memory) vkFreeMemory(dev, *memory, 0);
    *image = VK_NULL_HANDLE; *memory = VK_NULL_HANDLE; return -1;
  }
  VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
  vi.image = *image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = depth_format;
  vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
  if (vkCreateImageView(dev, &vi, 0, view) != VK_SUCCESS) {
    vkDestroyImage(dev, *image, 0); vkFreeMemory(dev, *memory, 0);
    *image = VK_NULL_HANDLE; *memory = VK_NULL_HANDLE; return -1;
  }
  return 0;
}
static int make_sc(void) {
  VkSurfaceCapabilitiesKHR cap; CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &cap));
  uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, 0); if (!nf) return -1; VkSurfaceFormatKHR fm[32]; if (nf > 32) nf = 32; CK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fm));
  VkSurfaceFormatKHR pick = fm[0]; for (uint32_t i = 0; i < nf; i++) if (fm[i].format == VK_FORMAT_R8G8B8A8_UNORM || fm[i].format == VK_FORMAT_B8G8R8A8_UNORM) { pick = fm[i]; break; }
  ext = cap.currentExtent; if (ext.width == 0xFFFFFFFFu) { ext.width = ANativeWindow_getWidth(g_win); ext.height = ANativeWindow_getHeight(g_win); }
  if (!(cap.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || cap.minImageCount > 8) return -1;
  uint32_t cnt = cap.minImageCount + 1; if (cap.maxImageCount && cnt > cap.maxImageCount) cnt = cap.maxImageCount; if (cnt > 8) cnt = 8;
  VkSwapchainKHR old = sc;
  destroy_pipeline_cache();
  for (uint32_t i = 0; i < nimg; i++) {
    if (fb[i]) vkDestroyFramebuffer(dev, fb[i], 0);
    if (views[i]) vkDestroyImageView(dev, views[i], 0);
    if (depth_views[i]) vkDestroyImageView(dev, depth_views[i], 0);
    if (depth_img[i]) vkDestroyImage(dev, depth_img[i], 0);
    if (depth_mem[i]) vkFreeMemory(dev, depth_mem[i], 0);
    if (rel[i]) vkDestroySemaphore(dev, rel[i], 0);
    fb[i] = VK_NULL_HANDLE; views[i] = VK_NULL_HANDLE; depth_views[i] = VK_NULL_HANDLE;
    depth_img[i] = VK_NULL_HANDLE; depth_mem[i] = VK_NULL_HANDLE; rel[i] = VK_NULL_HANDLE;
  }
  nimg = 0;
  if (render_pass) { vkDestroyRenderPass(dev, render_pass, 0); render_pass = VK_NULL_HANDLE; }
  VkSwapchainCreateInfoKHR s = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
  s.surface = surf; s.minImageCount = cnt; s.imageFormat = pick.format; s.imageColorSpace = pick.colorSpace; s.imageExtent = ext; s.imageArrayLayers = 1;
  s.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; s.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; s.preTransform = cap.currentTransform;
  s.compositeAlpha = (cap.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  s.presentMode = VK_PRESENT_MODE_FIFO_KHR; s.clipped = VK_TRUE; s.oldSwapchain = old; CK(vkCreateSwapchainKHR(dev, &s, 0, &sc));
  if (old) vkDestroySwapchainKHR(dev, old, 0);
  color_format = pick.format;
  nimg = 8; CK(vkGetSwapchainImagesKHR(dev, sc, &nimg, img));
  VkAttachmentDescription attachments[2] = { { 0 }, { 0 } };
  attachments[0].format = color_format; attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
  attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  attachments[1].format = depth_format; attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
  attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
  VkAttachmentReference depth_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
  VkSubpassDescription subpass = { 0 }; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color_ref; subpass.pDepthStencilAttachment = &depth_ref;
  VkSubpassDependency dependency = { 0 };
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL; dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
  rp.attachmentCount = 2; rp.pAttachments = attachments; rp.subpassCount = 1; rp.pSubpasses = &subpass;
  rp.dependencyCount = 1; rp.pDependencies = &dependency;
  CK(vkCreateRenderPass(dev, &rp, 0, &render_pass));
  for (uint32_t i = 0; i < nimg; i++) {
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = color_format;
    vi.components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    CK(vkCreateImageView(dev, &vi, 0, &views[i]));
    if (create_depth_attachment(ext.width, ext.height, &depth_img[i], &depth_mem[i], &depth_views[i])) return -1;
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkImageView framebuffer_attachments[2] = { views[i], depth_views[i] };
    fi.renderPass = render_pass; fi.attachmentCount = 2; fi.pAttachments = framebuffer_attachments;
    fi.width = ext.width; fi.height = ext.height; fi.layers = 1;
    CK(vkCreateFramebuffer(dev, &fi, 0, &fb[i]));
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    CK(vkCreateSemaphore(dev, &si, 0, &rel[i]));
  }
  LG("swapchain %ux%u images=%u transform=%d", ext.width, ext.height, nimg, (int)cap.currentTransform); return 0;
}
int aot_present_init(void) {
  for (int i = 0; i < 300 && !g_win; i++) usleep(50000);   /* wait up to 15 s for the Android window */
  if (!g_win) { LG("no ANativeWindow"); return -1; }
  const char *ie[2] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
  uint32_t api_version = VK_API_VERSION_1_0;
  PFN_vkEnumerateInstanceVersion enumerate_version =
    (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
  if (!enumerate_version || enumerate_version(&api_version) != VK_SUCCESS) api_version = VK_API_VERSION_1_0;
  if (api_version > VK_API_VERSION_1_3) api_version = VK_API_VERSION_1_3;
  VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO }; ai.apiVersion = api_version; ai.pApplicationName = "aotgame";
  VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ii.pApplicationInfo = &ai; ii.enabledExtensionCount = 2; ii.ppEnabledExtensionNames = ie; CK(vkCreateInstance(&ii, 0, &inst));
  VkAndroidSurfaceCreateInfoKHR si = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR }; si.window = g_win; CK(vkCreateAndroidSurfaceKHR(inst, &si, 0, &surf));
  uint32_t np = 8; VkPhysicalDevice pds[8]; CK(vkEnumeratePhysicalDevices(inst, &np, pds)); if (!np) return -1;
  for (uint32_t i = 0; i < np && !pd; i++) { uint32_t nq = 16; VkQueueFamilyProperties qp[16]; vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qp);
    for (uint32_t k = 0; k < nq; k++) { VkBool32 sup = 0; vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], k, surf, &sup); if ((qp[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && sup) { pd = pds[i]; qf = k; break; } } }
  if (!pd) return -1;
  VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
  { /* capability report for the D3D10/11-over-Vulkan decision: what this phone's driver exposes (DXVK needs several of these) */
    VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(pd, &f); uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ex = ne ? malloc(ne * sizeof *ex) : NULL; if (ex) vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ex);
    static const char *const want[] = { "VK_EXT_robustness2", "VK_EXT_extended_dynamic_state", "VK_EXT_extended_dynamic_state2", "VK_EXT_extended_dynamic_state3", "VK_EXT_transform_feedback", "VK_EXT_depth_clip_enable", "VK_EXT_custom_border_color",
      "VK_EXT_vertex_attribute_divisor", "VK_EXT_shader_demote_to_helper_invocation", "VK_EXT_memory_priority", "VK_EXT_4444_formats", "VK_EXT_graphics_pipeline_library", "VK_EXT_shader_stencil_export", "VK_KHR_dynamic_rendering", "VK_KHR_maintenance4", "VK_KHR_maintenance5",
      "VK_KHR_timeline_semaphore", "VK_KHR_draw_indirect_count", "VK_KHR_driver_properties", "VK_KHR_push_descriptor", "VK_EXT_conservative_rasterization", NULL };
    fprintf(stderr, "[vkcaps] %s api=%u.%u.%u driver=0x%x vendor=0x%x device=0x%x\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion), props.driverVersion, props.vendorID, props.deviceID);
    fprintf(stderr, "[vkcaps] features: geometryShader=%d tessellation=%d independentBlend=%d depthClamp=%d depthBiasClamp=%d fillModeNonSolid=%d sampleRateShading=%d textureBC=%d fullDrawIndexUint32=%d imageCubeArray=%d shaderClipDistance=%d shaderCullDistance=%d logicOp=%d multiViewport=%d occlusionQueryPrecise=%d pipelineStatistics=%d shaderFloat64=%d samplerAniso=%d robustBufferAccess=%d\n",
      f.geometryShader, f.tessellationShader, f.independentBlend, f.depthClamp, f.depthBiasClamp, f.fillModeNonSolid, f.sampleRateShading, f.textureCompressionBC, f.fullDrawIndexUint32, f.imageCubeArray, f.shaderClipDistance, f.shaderCullDistance, f.logicOp, f.multiViewport, f.occlusionQueryPrecise, f.pipelineStatisticsQuery, f.shaderFloat64, f.samplerAnisotropy, f.robustBufferAccess);
    fprintf(stderr, "[vkcaps] extensions:"); for (int w = 0; want[w]; w++) { int has = 0; for (uint32_t e = 0; e < ne && ex; e++) if (!strcmp(ex[e].extensionName, want[w])) { has = 1; break; } fprintf(stderr, " %s=%d", want[w] + 3, has); } fprintf(stderr, "\n"); free(ex);
  }
  uniform_alignment = props.limits.minUniformBufferOffsetAlignment;
  if (!uniform_alignment) uniform_alignment = 1;
  max_uniform_range = props.limits.maxUniformBufferRange;
  const VkFormat depth_candidates[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM };
  for (size_t i = 0; i < sizeof depth_candidates / sizeof depth_candidates[0]; i++) {
    VkFormatProperties format_props;
    vkGetPhysicalDeviceFormatProperties(pd, depth_candidates[i], &format_props);
    if (format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
      depth_format = depth_candidates[i];
      break;
    }
  }
  if (depth_format == VK_FORMAT_UNDEFINED) { LG("no supported depth attachment format"); return -1; }
  supports_spirv16 = api_version >= VK_API_VERSION_1_3 && props.apiVersion >= VK_API_VERSION_1_3;
  VkPhysicalDeviceVulkan12Features available12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
  if (api_version >= VK_API_VERSION_1_2 && props.apiVersion >= VK_API_VERSION_1_2) {
    VkPhysicalDeviceFeatures2 available = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    PFN_vkGetPhysicalDeviceFeatures2 get_features2 =
      (PFN_vkGetPhysicalDeviceFeatures2)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceFeatures2");
    if (get_features2) {
      available.pNext = &available12; get_features2(pd, &available);
      supports_vulkan_memory_model = available12.vulkanMemoryModel;
    }
  }
  float pr = 1; VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO }; qi.queueFamilyIndex = qf; qi.queueCount = 1; qi.pQueuePriorities = &pr; const char *de[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
  VkPhysicalDeviceVulkan12Features enabled12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
  enabled12.vulkanMemoryModel = supports_vulkan_memory_model ? VK_TRUE : VK_FALSE;
  VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO }; di.pNext = supports_vulkan_memory_model ? &enabled12 : 0; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = de; CK(vkCreateDevice(pd, &di, 0, &dev));
  vkGetDeviceQueue(dev, qf, 0, &q);
  VkDescriptorSetLayoutBinding all_resource_bindings[12];
  for (uint32_t slot = 0; slot < AOT_GFX_MAX_TEXTURES; slot++) {
    all_resource_bindings[slot] = (VkDescriptorSetLayoutBinding){ 0 };
    all_resource_bindings[slot].binding = slot;
    all_resource_bindings[slot].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    all_resource_bindings[slot].descriptorCount = 1;
    all_resource_bindings[slot].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    all_resource_bindings[4 + slot] = (VkDescriptorSetLayoutBinding){ 0 };
    all_resource_bindings[4 + slot].binding = 4 + slot;
    all_resource_bindings[4 + slot].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    all_resource_bindings[4 + slot].descriptorCount = 1;
    all_resource_bindings[4 + slot].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  for (uint32_t slot = 0; slot < AOT_GFX_MAX_CONSTANT_BUFFERS; slot++) {
    all_resource_bindings[8 + slot] = (VkDescriptorSetLayoutBinding){ 0 };
    all_resource_bindings[8 + slot].binding = 8 + slot;
    all_resource_bindings[8 + slot].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    all_resource_bindings[8 + slot].descriptorCount = 1;
    all_resource_bindings[8 + slot].stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo resource_layout_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
  resource_layout_info.bindingCount = 12; resource_layout_info.pBindings = all_resource_bindings;
  CK(vkCreateDescriptorSetLayout(dev, &resource_layout_info, 0, &resource_layout));
  VkDescriptorPoolSize descriptor_sizes[3] = {
    { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 16384 },
    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384 }
  };
  VkDescriptorPoolCreateInfo descriptor_pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
  descriptor_pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  descriptor_pool_info.maxSets = 4096; descriptor_pool_info.poolSizeCount = 3; descriptor_pool_info.pPoolSizes = descriptor_sizes;
  CK(vkCreateDescriptorPool(dev, &descriptor_pool_info, 0, &descriptor_pool));
  VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
  li.setLayoutCount = 1; li.pSetLayouts = &resource_layout;
  CK(vkCreatePipelineLayout(dev, &li, 0, &pipeline_layout));
  VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = qf; CK(vkCreateCommandPool(dev, &pi, 0, &pool));
  VkCommandBufferAllocateInfo ci = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; ci.commandPool = pool; ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ci.commandBufferCount = 1; CK(vkAllocateCommandBuffers(dev, &ci, &cb));
  VkSemaphoreCreateInfo se = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO }; CK(vkCreateSemaphore(dev, &se, 0, &acq));
  VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT; CK(vkCreateFence(dev, &fi, 0, &fence));
  if (make_sc()) return -1; ok = 1; return 0;
}
void aot_present_size(uint32_t *w, uint32_t *h) {
  int rot = 0; if (ok) { VkSurfaceCapabilitiesKHR c; if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &c) == VK_SUCCESS) rot = c.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR); }
  uint32_t ww = g_win ? ANativeWindow_getWidth(g_win) : 1280, hh = g_win ? ANativeWindow_getHeight(g_win) : 720; if (ok) { ww = ext.width; hh = ext.height; if (rot) { uint32_t t = ww; ww = hh; hh = t; } } *w = ww; *h = hh;
}
static int find_memory_type(uint32_t bits, VkMemoryPropertyFlags flags, uint32_t *index) {
  VkPhysicalDeviceMemoryProperties props;
  vkGetPhysicalDeviceMemoryProperties(pd, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
    if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
      *index = i;
      return 1;
    }
  }
  return 0;
}
int aot_present_texture_create(uint32_t width, uint32_t height, const uint8_t *rgba,
                               AOT_PRESENT_TEXTURE **out) {
  if (!ok || !out || !rgba || !width || !height || width > 4096 || height > 4096 ||
      (size_t)width > SIZE_MAX / 4 / height) return -1;
  *out = NULL;
  AOT_PRESENT_TEXTURE *texture = calloc(1, sizeof *texture);
  if (!texture) return -1;
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory = VK_NULL_HANDLE;
  VkCommandBuffer upload = VK_NULL_HANDLE;
  VkResult result = VK_SUCCESS;
  VkImageCreateInfo image_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
  image_info.imageType = VK_IMAGE_TYPE_2D; image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = (VkExtent3D){ width, height, 1 }; image_info.mipLevels = 1;
  image_info.arrayLayers = 1; image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  result = vkCreateImage(dev, &image_info, 0, &texture->image);
  if (result != VK_SUCCESS) goto fail;
  VkMemoryRequirements requirements; vkGetImageMemoryRequirements(dev, texture->image, &requirements);
  uint32_t memory_type;
  if (!find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type))
    goto fail;
  VkMemoryAllocateInfo image_allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  image_allocation.allocationSize = requirements.size; image_allocation.memoryTypeIndex = memory_type;
  if (vkAllocateMemory(dev, &image_allocation, 0, &texture->memory) != VK_SUCCESS ||
      vkBindImageMemory(dev, texture->image, texture->memory, 0) != VK_SUCCESS)
    goto fail;

  VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
  view_info.image = texture->image; view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  view_info.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  if (vkCreateImageView(dev, &view_info, 0, &texture->view) != VK_SUCCESS) goto fail;

  VkDeviceSize image_size = (VkDeviceSize)width * height * 4;
  VkBufferCreateInfo buffer_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
  buffer_info.size = image_size; buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(dev, &buffer_info, 0, &staging) != VK_SUCCESS) goto fail;
  vkGetBufferMemoryRequirements(dev, staging, &requirements);
  if (!find_memory_type(requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &memory_type))
    goto fail;
  VkMemoryAllocateInfo staging_allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  staging_allocation.allocationSize = requirements.size; staging_allocation.memoryTypeIndex = memory_type;
  if (vkAllocateMemory(dev, &staging_allocation, 0, &staging_memory) != VK_SUCCESS ||
      vkBindBufferMemory(dev, staging, staging_memory, 0) != VK_SUCCESS)
    goto fail;
  void *mapped = NULL;
  if (vkMapMemory(dev, staging_memory, 0, image_size, 0, &mapped) != VK_SUCCESS) goto fail;
  memcpy(mapped, rgba, (size_t)image_size);
  vkUnmapMemory(dev, staging_memory);

  VkCommandBufferAllocateInfo command_allocation = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
  command_allocation.commandPool = pool; command_allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_allocation.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(dev, &command_allocation, &upload) != VK_SUCCESS) goto fail;
  VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(upload, &begin) != VK_SUCCESS) goto fail;
  VkImageMemoryBarrier to_transfer = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
  to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_transfer.image = texture->image;
  to_transfer.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 0, NULL, 0, NULL, 1, &to_transfer);
  VkBufferImageCopy copy = { 0 };
  copy.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
  copy.imageExtent = (VkExtent3D){ width, height, 1 };
  vkCmdCopyBufferToImage(upload, staging, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  VkImageMemoryBarrier to_shader = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
  to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_shader.image = texture->image;
  to_shader.subresourceRange = to_transfer.subresourceRange;
  vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      0, 0, NULL, 0, NULL, 1, &to_shader);
  if (vkEndCommandBuffer(upload) != VK_SUCCESS) goto fail;
  VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
  submit.commandBufferCount = 1; submit.pCommandBuffers = &upload;
  if (vkQueueSubmit(q, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS || vkQueueWaitIdle(q) != VK_SUCCESS)
    goto fail;
  vkFreeCommandBuffers(dev, pool, 1, &upload);
  vkDestroyBuffer(dev, staging, 0); vkFreeMemory(dev, staging_memory, 0);
  texture->width = width; texture->height = height; *out = texture;
  return 0;
fail:
  if (upload) vkFreeCommandBuffers(dev, pool, 1, &upload);
  if (staging) vkDestroyBuffer(dev, staging, 0);
  if (staging_memory) vkFreeMemory(dev, staging_memory, 0);
  if (texture->view) vkDestroyImageView(dev, texture->view, 0);
  if (texture->image) vkDestroyImage(dev, texture->image, 0);
  if (texture->memory) vkFreeMemory(dev, texture->memory, 0);
  free(texture);
  return -1;
}
void aot_present_texture_destroy(AOT_PRESENT_TEXTURE *texture) {
  if (!texture) return;
  if (dev) vkDeviceWaitIdle(dev);
  if (texture->view) vkDestroyImageView(dev, texture->view, 0);
  if (texture->image) vkDestroyImage(dev, texture->image, 0);
  if (texture->memory) vkFreeMemory(dev, texture->memory, 0);
  free(texture);
}
static VkSamplerAddressMode sampler_address_mode(uint32_t address, int *valid) {
  switch (address) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: *valid = 0; return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  }
}
int aot_present_sampler_create(uint32_t filter, uint32_t address_u, uint32_t address_v,
                               uint32_t address_w, AOT_PRESENT_SAMPLER **out) {
  if (!ok || !out) return -1;
  *out = NULL;
  int valid = 1;
  VkSamplerAddressMode u = sampler_address_mode(address_u, &valid);
  VkSamplerAddressMode v = sampler_address_mode(address_v, &valid);
  VkSamplerAddressMode w = sampler_address_mode(address_w, &valid);
  if (!valid || (filter != 0 && filter != 0x15)) return -1;
  AOT_PRESENT_SAMPLER *sampler = calloc(1, sizeof *sampler);
  if (!sampler) return -1;
  VkSamplerCreateInfo info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
  info.magFilter = filter == 0x15 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.minFilter = info.magFilter; info.mipmapMode = filter == 0x15 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = u; info.addressModeV = v; info.addressModeW = w;
  info.maxAnisotropy = 1.0f; info.compareOp = VK_COMPARE_OP_ALWAYS;
  info.minLod = 0.0f; info.maxLod = 0.0f; info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
  if (vkCreateSampler(dev, &info, 0, &sampler->sampler) != VK_SUCCESS) { free(sampler); return -1; }
  *out = sampler;
  return 0;
}
void aot_present_sampler_destroy(AOT_PRESENT_SAMPLER *sampler) {
  if (!sampler) return;
  if (sampler->sampler) vkDestroySampler(dev, sampler->sampler, 0);
  free(sampler);
}
static VkFormat vertex_format(uint32_t format) {
  switch (format) {
    case AOT_PRESENT_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case AOT_PRESENT_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case AOT_PRESENT_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
    case AOT_PRESENT_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case AOT_PRESENT_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
    case AOT_PRESENT_FORMAT_R32G32_UINT: return VK_FORMAT_R32G32_UINT;
    case AOT_PRESENT_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
  }
}
static uint32_t vertex_format_bytes(uint32_t format) {
  switch (format) {
    case AOT_PRESENT_FORMAT_R32_FLOAT:
    case AOT_PRESENT_FORMAT_R32_UINT:
    case AOT_PRESENT_FORMAT_R8G8B8A8_UNORM: return 4;
    case AOT_PRESENT_FORMAT_R32G32_FLOAT:
    case AOT_PRESENT_FORMAT_R32G32_UINT: return 8;
    case AOT_PRESENT_FORMAT_R32G32B32_FLOAT: return 12;
    case AOT_PRESENT_FORMAT_R32G32B32A32_FLOAT: return 16;
    default: return 0;
  }
}
static int make_pipeline(const uint32_t *vs, size_t vs_size, const uint32_t *ps, size_t ps_size,
                         const uint32_t strides[AOT_GFX_MAX_VERTEX_STREAMS],
                         const AOT_PRESENT_VERTEX_ATTRIBUTE *attrs, uint32_t attr_count,
                         int alpha_blend, uint8_t cull_mode,
                         uint8_t front_counter_clockwise,
                         VkPipeline *out, VkShaderModule *vs_module, VkShaderModule *ps_module) {
  if (!supports_spirv16 || !supports_vulkan_memory_model || !vs || !ps || !vs_size ||
      !ps_size || vs_size % 4 || ps_size % 4 ||
      attr_count > AOT_GFX_MAX_DRAW_ATTRIBUTES || (attr_count && !attrs)) {
    LG("pipeline prerequisites not met: Vulkan 1.3/SPIR-V 1.6=%d Vulkan memory model=%d vertex shader=%p/%zu pixel shader=%p/%zu attributes=%u",
        supports_spirv16, supports_vulkan_memory_model, (const void *)vs,
        vs_size, (const void *)ps, ps_size, attr_count);
    return -1;
  }
  VkShaderModuleCreateInfo sm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
  sm.codeSize = vs_size; sm.pCode = vs;
  VkResult result = vkCreateShaderModule(dev, &sm, 0, vs_module);
  if (result != VK_SUCCESS) {
    LG("vertex vkCreateShaderModule failed: %d", (int)result);
    return -1;
  }
  sm.codeSize = ps_size; sm.pCode = ps;
  result = vkCreateShaderModule(dev, &sm, 0, ps_module);
  if (result != VK_SUCCESS) {
    LG("pixel vkCreateShaderModule failed: %d", (int)result);
    vkDestroyShaderModule(dev, *vs_module, 0); *vs_module = VK_NULL_HANDLE; return -1;
  }
  VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = *vs_module; stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = *ps_module; stages[1].pName = "main";
  VkVertexInputBindingDescription bindings[AOT_GFX_MAX_VERTEX_STREAMS];
  uint32_t binding_count = 0;
  for (uint32_t slot = 0; slot < AOT_GFX_MAX_VERTEX_STREAMS; slot++) {
    int used = 0;
    for (uint32_t i = 0; i < attr_count; i++)
      if (attrs[i].binding == slot) used = 1;
    if (used) {
      if (!strides[slot]) goto invalid_layout;
      bindings[binding_count++] = (VkVertexInputBindingDescription){
        slot, strides[slot], VK_VERTEX_INPUT_RATE_VERTEX
      };
    }
  }
  VkVertexInputAttributeDescription attribute_desc[AOT_GFX_MAX_DRAW_ATTRIBUTES];
  for (uint32_t i = 0; i < attr_count; i++) {
    VkFormat format = vertex_format(attrs[i].format);
    uint32_t size = vertex_format_bytes(attrs[i].format);
    if (format == VK_FORMAT_UNDEFINED || attrs[i].binding >= AOT_GFX_MAX_VERTEX_STREAMS ||
        attrs[i].offset > strides[attrs[i].binding] ||
        size > strides[attrs[i].binding] - attrs[i].offset)
      goto invalid_layout;
    attribute_desc[i] = (VkVertexInputAttributeDescription){
      attrs[i].location, attrs[i].binding, format, attrs[i].offset
    };
  }
  VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
  vi.vertexBindingDescriptionCount = binding_count;
  vi.pVertexBindingDescriptions = binding_count ? bindings : NULL;
  vi.vertexAttributeDescriptionCount = attr_count; vi.pVertexAttributeDescriptions = attr_count ? attribute_desc : 0;
  VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
  vp.viewportCount = 1; vp.scissorCount = 1;
  VkDynamicState dynamic_states[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
  };
  VkPipelineDynamicStateCreateInfo dynamic = {
    VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO
  };
  dynamic.dynamicStateCount = sizeof dynamic_states / sizeof dynamic_states[0];
  dynamic.pDynamicStates = dynamic_states;
  VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = cull_mode == 1 ? VK_CULL_MODE_FRONT_BIT :
      cull_mode == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
  rs.frontFace = front_counter_clockwise ?
      VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
  depth.depthTestEnable = VK_TRUE; depth.depthWriteEnable = VK_TRUE;
  depth.depthCompareOp = VK_COMPARE_OP_LESS; depth.maxDepthBounds = 1.0f;
  VkPipelineColorBlendAttachmentState blend_attachment = { 0 };
  blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  if (alpha_blend) {
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
  }
  VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
  blend.attachmentCount = 1; blend.pAttachments = &blend_attachment;
  VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
  ci.stageCount = 2; ci.pStages = stages; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia;
  ci.pViewportState = &vp; ci.pRasterizationState = &rs; ci.pMultisampleState = &ms;
  ci.pDepthStencilState = &depth; ci.pColorBlendState = &blend;
  ci.pDynamicState = &dynamic;
  ci.layout = pipeline_layout; ci.renderPass = render_pass; ci.subpass = 0;
  result = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, 0, out);
  if (result != VK_SUCCESS) {
    LG("vkCreateGraphicsPipelines failed: %d", (int)result);
    vkDestroyShaderModule(dev, *ps_module, 0); vkDestroyShaderModule(dev, *vs_module, 0);
    *ps_module = *vs_module = VK_NULL_HANDLE; return -1;
  }
  return 0;
invalid_layout:
  vkDestroyShaderModule(dev, *ps_module, 0);
  vkDestroyShaderModule(dev, *vs_module, 0);
  *ps_module = *vs_module = VK_NULL_HANDLE;
  return -1;
}
static void destroy_pipeline_cache(void) {
  for (size_t i = 0; i < pipeline_cache_count; i++) {
    if (pipeline_cache[i].pipeline) vkDestroyPipeline(dev, pipeline_cache[i].pipeline, 0);
    if (pipeline_cache[i].vs_module) vkDestroyShaderModule(dev, pipeline_cache[i].vs_module, 0);
    if (pipeline_cache[i].ps_module) vkDestroyShaderModule(dev, pipeline_cache[i].ps_module, 0);
  }
  free(pipeline_cache);
  pipeline_cache = NULL;
  pipeline_cache_count = pipeline_cache_capacity = 0;
}
void aot_present_graphics_cleanup(void) {
  if (!dev) return;
  vkDeviceWaitIdle(dev);
  destroy_pipeline_cache();
}
static int get_pipeline(const uint32_t *vs, size_t vs_size, const uint32_t *ps, size_t ps_size,
                        const uint32_t strides[AOT_GFX_MAX_VERTEX_STREAMS],
                        const AOT_PRESENT_VERTEX_ATTRIBUTE *attrs,
                        uint32_t attr_count, int alpha_blend,
                        uint8_t cull_mode, uint8_t front_counter_clockwise,
                        VkPipeline *out) {
  for (size_t i = 0; i < pipeline_cache_count; i++) {
    PipelineCacheEntry *entry = &pipeline_cache[i];
    if (entry->vs == vs && entry->vs_size == vs_size && entry->ps == ps &&
        entry->ps_size == ps_size && entry->attribute_count == attr_count &&
        entry->alpha_blend == (uint8_t)alpha_blend &&
        entry->cull_mode == cull_mode &&
        entry->front_counter_clockwise == front_counter_clockwise &&
        !memcmp(entry->strides, strides, sizeof entry->strides) &&
        (!attr_count || !memcmp(entry->attributes, attrs,
            (size_t)attr_count * sizeof *attrs))) {
      *out = entry->pipeline;
      return 0;
    }
  }
  if (pipeline_cache_count == pipeline_cache_capacity) {
    size_t capacity = pipeline_cache_capacity ? pipeline_cache_capacity * 2 : 8;
    if (capacity > 1024) return -1;
    PipelineCacheEntry *entries = realloc(pipeline_cache,
        capacity * sizeof *entries);
    if (!entries) return -1;
    pipeline_cache = entries;
    pipeline_cache_capacity = capacity;
  }
  PipelineCacheEntry entry;
  memset(&entry, 0, sizeof entry);
  if (cull_mode > 2 || front_counter_clockwise > 1 ||
      make_pipeline(vs, vs_size, ps, ps_size, strides, attrs, attr_count,
                    alpha_blend, cull_mode, front_counter_clockwise,
                    &entry.pipeline, &entry.vs_module, &entry.ps_module))
    return -1;
  entry.vs = vs; entry.vs_size = vs_size; entry.ps = ps; entry.ps_size = ps_size;
  memcpy(entry.strides, strides, sizeof entry.strides);
  entry.attribute_count = attr_count;
  entry.alpha_blend = (uint8_t)alpha_blend;
  entry.cull_mode = cull_mode;
  entry.front_counter_clockwise = front_counter_clockwise;
  if (attr_count) memcpy(entry.attributes, attrs,
      (size_t)attr_count * sizeof *attrs);
  pipeline_cache[pipeline_cache_count++] = entry;
  *out = entry.pipeline;
  return 0;
}
static int upload_buffer(const void *data, size_t size, VkBufferUsageFlags usage,
                         VkBuffer *buffer, VkDeviceMemory *memory) {
  if (!data || !size) {
    LG("Vulkan upload rejected: data=%p size=%zu", data, size);
    return -1;
  }
  VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
  bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult result = vkCreateBuffer(dev, &bi, 0, buffer);
  if (result != VK_SUCCESS) {
    LG("vkCreateBuffer for frame upload failed: %d", (int)result);
    return -1;
  }
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(dev, *buffer, &req);
  uint32_t type;
  if (!find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type)) {
    LG("no host-visible/coherent memory type for frame upload");
    vkDestroyBuffer(dev, *buffer, 0); *buffer = VK_NULL_HANDLE; return -1;
  }
  VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ai.allocationSize = req.size; ai.memoryTypeIndex = type;
  result = vkAllocateMemory(dev, &ai, 0, memory);
  if (result != VK_SUCCESS) {
    LG("vkAllocateMemory for frame upload failed: %d", (int)result);
    vkDestroyBuffer(dev, *buffer, 0);
    *buffer = VK_NULL_HANDLE; return -1;
  }
  result = vkBindBufferMemory(dev, *buffer, *memory, 0);
  if (result != VK_SUCCESS) {
    LG("vkBindBufferMemory for frame upload failed: %d", (int)result);
    vkDestroyBuffer(dev, *buffer, 0);
    if (*memory) vkFreeMemory(dev, *memory, 0);
    *buffer = VK_NULL_HANDLE; *memory = VK_NULL_HANDLE; return -1;
  }
  void *mapped = 0;
  result = vkMapMemory(dev, *memory, 0, size, 0, &mapped);
  if (result != VK_SUCCESS) {
    LG("vkMapMemory for frame upload failed: %d", (int)result);
    vkDestroyBuffer(dev, *buffer, 0);
    vkFreeMemory(dev, *memory, 0);
    *buffer = VK_NULL_HANDLE; *memory = VK_NULL_HANDLE; return -1;
  }
  memcpy(mapped, data, size);
  vkUnmapMemory(dev, *memory);
  return 0;
}
typedef struct {
  VkDeviceSize vertex[AOT_GFX_MAX_VERTEX_STREAMS];
  VkDeviceSize indices;
  VkDeviceSize constants[AOT_GFX_MAX_CONSTANT_BUFFERS];
  uint32_t strides[AOT_GFX_MAX_VERTEX_STREAMS];
} DrawOffsets;

static int aligned_size(size_t value, size_t alignment, size_t *out) {
  size_t remainder = value % alignment;
  size_t padding = remainder ? alignment - remainder : 0;
  if (value > SIZE_MAX - padding) return -1;
  *out = value + padding;
  return 0;
}

static int draw_needs_descriptors(const AOT_GFX_DRAW_CALL *draw) {
  for (uint32_t slot = 0; slot < AOT_GFX_MAX_TEXTURES; slot++)
    if (draw->textures[slot] || draw->samplers[slot]) return 1;
  for (uint32_t slot = 0; slot < AOT_GFX_MAX_CONSTANT_BUFFERS; slot++)
    if (draw->constants[slot].data_size) return 1;
  return 0;
}

static int reset_frame_sync(int reset_fence) {
  if (vkDeviceWaitIdle(dev) != VK_SUCCESS) return -1;
  if (reset_fence) {
    if (fence) vkDestroyFence(dev, fence, NULL);
    fence = VK_NULL_HANDLE;
    VkFenceCreateInfo info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(dev, &info, NULL, &fence) != VK_SUCCESS) return -1;
  }
  if (acq) vkDestroySemaphore(dev, acq, NULL);
  acq = VK_NULL_HANDLE;
  VkSemaphoreCreateInfo semaphore_info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
  if (vkCreateSemaphore(dev, &semaphore_info, NULL, &acq) != VK_SUCCESS) return -1;
  for (uint32_t i = 0; i < nimg; i++) {
    if (rel[i]) vkDestroySemaphore(dev, rel[i], NULL);
    rel[i] = VK_NULL_HANDLE;
    if (vkCreateSemaphore(dev, &semaphore_info, NULL, &rel[i]) != VK_SUCCESS)
      return -1;
  }
  return 0;
}

int aot_present_draw_batch(const float c4[4], const AOT_GFX_DRAW_BATCH *batch,
                           AOT_PRESENT_FRAME_STATS *stats) {
  if (!ok || !c4 || !batch) return -1;
  if (stats) memset(stats, 0, sizeof *stats);
  size_t draw_count = aot_gfx_draw_batch_count(batch);
  if (draw_count > AOT_GFX_MAX_DRAWS_PER_FRAME) {
    LG("draw batch rejected: %zu draws exceeds limit %u",
        draw_count, AOT_GFX_MAX_DRAWS_PER_FRAME);
    return -1;
  }

  DrawOffsets *offsets = calloc(draw_count ? draw_count : 1, sizeof *offsets);
  VkPipeline *pipelines = calloc(draw_count ? draw_count : 1, sizeof *pipelines);
  VkDescriptorSet *descriptor_sets = calloc(draw_count ? draw_count : 1,
                                             sizeof *descriptor_sets);
  if (!offsets || !pipelines || !descriptor_sets) {
    LG("draw batch allocation failed for %zu draws", draw_count);
    goto cleanup_cpu;
  }

  size_t vertex_total = 0, index_total = 0, constants_total = 0,
      constants_used = 0;
  for (size_t index = 0; index < draw_count; index++) {
    const AOT_GFX_DRAW_CALL *draw = aot_gfx_draw_batch_get(batch, index);
    if (!draw || !draw->vertex_spirv || !draw->pixel_spirv ||
        !draw->vertex_count || !draw->attribute_count ||
        draw->attribute_count > AOT_GFX_MAX_DRAW_ATTRIBUTES || !draw->attributes) {
      LG("draw %u rejected: invalid shader, vertex count, or attribute count",
          (unsigned)index);
      goto cleanup_cpu;
    }
    uint64_t vertices_start = draw->first_vertex;
    uint64_t vertices_end = (uint64_t)draw->first_vertex + draw->vertex_count;
    if (draw->indexed) {
      uint32_t index_size = draw->index_format == 57u ? 2u :
          draw->index_format == 42u ? 4u : 0u;
      if (!index_size || !draw->indices ||
          (size_t)draw->vertex_count > SIZE_MAX / index_size ||
          draw->index_data_size != (size_t)draw->vertex_count * index_size ||
          aligned_size(index_total, index_size, &index_total) ||
          index_total > SIZE_MAX - draw->index_data_size) {
        LG("draw %u rejected: invalid index data format=%u count=%u bytes=%zu",
            (unsigned)index, draw->index_format, draw->vertex_count,
            draw->index_data_size);
        goto cleanup_cpu;
      }
      uint32_t min_vertex = UINT32_MAX, max_vertex = 0;
      for (uint32_t i = 0; i < draw->vertex_count; i++) {
        uint32_t raw_index;
        if (index_size == 2) {
          uint16_t short_index;
          memcpy(&short_index, (const uint8_t *)draw->indices +
              (size_t)i * index_size, sizeof short_index);
          raw_index = short_index;
        } else {
          memcpy(&raw_index, (const uint8_t *)draw->indices +
              (size_t)i * index_size, sizeof raw_index);
        }
        int64_t effective = (int64_t)raw_index + draw->base_vertex;
        if (effective < 0 || effective > UINT32_MAX) {
          LG("draw %u rejected: index %u plus base vertex %d is out of range",
              (unsigned)index, raw_index, draw->base_vertex);
          goto cleanup_cpu;
        }
        if ((uint32_t)effective < min_vertex) min_vertex = (uint32_t)effective;
        if ((uint32_t)effective > max_vertex) max_vertex = (uint32_t)effective;
      }
      vertices_start = min_vertex;
      vertices_end = (uint64_t)max_vertex + 1;
      offsets[index].indices = index_total;
      index_total += draw->index_data_size;
    } else if (draw->indices || draw->index_data_size) {
      LG("draw %u rejected: non-indexed draw contains index data", (unsigned)index);
      goto cleanup_cpu;
    }
    for (uint32_t slot = 0; slot < AOT_GFX_MAX_VERTEX_STREAMS; slot++) {
      const AOT_GFX_VERTEX_STREAM *stream = &draw->vertex_streams[slot];
      offsets[index].strides[slot] = stream->stride;
      if (!stream->data_size) continue;
      if (!stream->data || !stream->stride ||
          vertices_end > SIZE_MAX / stream->stride ||
          stream->offset > stream->data_size ||
          vertices_start > vertices_end ||
          vertices_end * stream->stride >
              stream->data_size - stream->offset ||
          vertex_total > SIZE_MAX - stream->data_size) {
        LG("draw %u vertex stream %u rejected: data=%p bytes=%zu stride=%u offset=%u first=%u count=%u",
            (unsigned)index, slot, stream->data, stream->data_size,
            stream->stride, stream->offset, (unsigned)vertices_start,
            draw->vertex_count);
        goto cleanup_cpu;
      }
      offsets[index].vertex[slot] = vertex_total;
      vertex_total += stream->data_size;
    }
    for (uint32_t attribute = 0; attribute < draw->attribute_count; attribute++) {
      const AOT_PRESENT_VERTEX_ATTRIBUTE *item = &draw->attributes[attribute];
      if (item->binding >= AOT_GFX_MAX_VERTEX_STREAMS ||
          !offsets[index].strides[item->binding] ||
          !vertex_format_bytes(item->format) ||
          item->offset > offsets[index].strides[item->binding] ||
          vertex_format_bytes(item->format) >
              offsets[index].strides[item->binding] - item->offset) {
        LG("draw %u attribute %u rejected: binding=%u format=%u offset=%u stride=%u",
            (unsigned)index, attribute, item->binding, item->format,
            item->offset, item->binding < AOT_GFX_MAX_VERTEX_STREAMS
                ? offsets[index].strides[item->binding] : 0);
        goto cleanup_cpu;
      }
    }
    for (uint32_t slot = 0; slot < AOT_GFX_MAX_TEXTURES; slot++)
      if ((draw->textures[slot] == NULL) != (draw->samplers[slot] == NULL)) {
        LG("draw %u texture slot %u rejected: texture and sampler must be bound together",
            (unsigned)index, slot);
        goto cleanup_cpu;
      }
    for (uint32_t slot = 0; slot < AOT_GFX_MAX_CONSTANT_BUFFERS; slot++) {
      const AOT_GFX_CONSTANT_DATA *constant = &draw->constants[slot];
      if (!constant->data_size) continue;
      if (!constant->data || constant->data_size > max_uniform_range ||
          constant->data_size > 65536 || (constant->data_size & 15u) ||
          aligned_size(constants_total, (size_t)uniform_alignment, &constants_total) ||
          constants_total > SIZE_MAX - constant->data_size) {
        LG("draw %u constant buffer b%u rejected: data=%p bytes=%zu max_range=%u alignment=%llu",
            (unsigned)index, slot, constant->data, constant->data_size,
            max_uniform_range, (unsigned long long)uniform_alignment);
        goto cleanup_cpu;
      }
      offsets[index].constants[slot] = constants_total;
      constants_total += constant->data_size;
      constants_used += constant->data_size;
    }
    if (get_pipeline(draw->vertex_spirv, draw->vertex_spirv_size,
        draw->pixel_spirv, draw->pixel_spirv_size, offsets[index].strides,
        draw->attributes, draw->attribute_count, draw->alpha_blend,
        draw->cull_mode, draw->front_counter_clockwise,
        &pipelines[index])) {
      LG("failed to create Vulkan pipeline for draw %u", (unsigned)index);
      goto cleanup_cpu;
    }
  }

  uint8_t *vertex_blob = vertex_total ? malloc(vertex_total) : NULL;
  uint8_t *index_blob = index_total ? malloc(index_total) : NULL;
  uint8_t *constants_blob = constants_total ? calloc(1, constants_total) : NULL;
  if ((vertex_total && !vertex_blob) || (index_total && !index_blob) ||
      (constants_total && !constants_blob)) {
    LG("draw batch staging allocation failed: vertex=%zu index=%zu constant=%zu",
        vertex_total, index_total, constants_total);
    free(vertex_blob); free(index_blob); free(constants_blob);
    goto cleanup_cpu;
  }
  for (size_t index = 0; index < draw_count; index++) {
    const AOT_GFX_DRAW_CALL *draw = aot_gfx_draw_batch_get(batch, index);
    for (uint32_t slot = 0; slot < AOT_GFX_MAX_VERTEX_STREAMS; slot++)
      if (draw->vertex_streams[slot].data_size)
        memcpy(vertex_blob + offsets[index].vertex[slot],
            draw->vertex_streams[slot].data, draw->vertex_streams[slot].data_size);
    if (draw->index_data_size)
      memcpy(index_blob + offsets[index].indices, draw->indices,
          draw->index_data_size);
    for (uint32_t slot = 0; slot < AOT_GFX_MAX_CONSTANT_BUFFERS; slot++)
      if (draw->constants[slot].data_size)
        memcpy(constants_blob + offsets[index].constants[slot],
            draw->constants[slot].data, draw->constants[slot].data_size);
  }

  VkBuffer vertex_buffer = VK_NULL_HANDLE, index_buffer = VK_NULL_HANDLE,
      constants_buffer = VK_NULL_HANDLE;
  VkDeviceMemory vertex_memory = VK_NULL_HANDLE, index_memory = VK_NULL_HANDLE,
      constants_memory = VK_NULL_HANDLE;
  uint32_t descriptor_count = 0;
  for (size_t index = 0; index < draw_count; index++)
    if (draw_needs_descriptors(aot_gfx_draw_batch_get(batch, index)))
      descriptor_count++;
  int queue_submitted = 0, acquired = 0, fence_reset = 0;
  if (vertex_total && upload_buffer(vertex_blob, vertex_total,
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vertex_buffer, &vertex_memory)) {
    LG("failed to upload batched D3D11 vertex buffers");
    goto cleanup;
  }
  if (index_total && upload_buffer(index_blob, index_total,
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &index_buffer, &index_memory)) {
    LG("failed to upload batched D3D11 index buffers");
    goto cleanup;
  }
  if (constants_total && upload_buffer(constants_blob, constants_total,
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &constants_buffer, &constants_memory)) {
    LG("failed to upload batched D3D11 constant buffers");
    goto cleanup;
  }
  free(vertex_blob); vertex_blob = NULL;
  free(index_blob); index_blob = NULL;
  free(constants_blob); constants_blob = NULL;

  if (descriptor_count) {
    VkDescriptorSetLayout *layouts = malloc(descriptor_count * sizeof *layouts);
    VkDescriptorSet *allocated = malloc(descriptor_count * sizeof *allocated);
    if (!layouts || !allocated) { free(layouts); free(allocated); goto cleanup; }
    for (uint32_t i = 0; i < descriptor_count; i++) layouts[i] = resource_layout;
    VkDescriptorSetAllocateInfo allocate = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocate.descriptorPool = descriptor_pool;
    allocate.descriptorSetCount = descriptor_count;
    allocate.pSetLayouts = layouts;
    VkResult allocate_result = vkAllocateDescriptorSets(dev, &allocate, allocated);
    free(layouts);
    if (allocate_result != VK_SUCCESS) {
      free(allocated);
      LG("failed to allocate descriptor sets for %u draws: %d",
          descriptor_count, (int)allocate_result);
      goto cleanup;
    }
    uint32_t set_index = 0;
    for (size_t index = 0; index < draw_count; index++) {
      const AOT_GFX_DRAW_CALL *draw = aot_gfx_draw_batch_get(batch, index);
      if (!draw_needs_descriptors(draw)) continue;
      descriptor_sets[index] = allocated[set_index++];
      VkDescriptorImageInfo images[AOT_GFX_MAX_TEXTURES * 2];
      VkDescriptorBufferInfo buffers[AOT_GFX_MAX_CONSTANT_BUFFERS];
      VkWriteDescriptorSet writes[AOT_GFX_MAX_TEXTURES * 2 +
                                  AOT_GFX_MAX_CONSTANT_BUFFERS];
      uint32_t write_count = 0;
      for (uint32_t slot = 0; slot < AOT_GFX_MAX_TEXTURES; slot++) {
        if (!draw->textures[slot]) continue;
        images[slot] = (VkDescriptorImageInfo){ VK_NULL_HANDLE,
          draw->textures[slot]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        writes[write_count] = (VkWriteDescriptorSet){
          VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[write_count].dstSet = descriptor_sets[index];
        writes[write_count].dstBinding = slot;
        writes[write_count].descriptorCount = 1;
        writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[write_count++].pImageInfo = &images[slot];
        images[AOT_GFX_MAX_TEXTURES + slot] = (VkDescriptorImageInfo){
          draw->samplers[slot]->sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        writes[write_count] = (VkWriteDescriptorSet){
          VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[write_count].dstSet = descriptor_sets[index];
        writes[write_count].dstBinding = 4 + slot;
        writes[write_count].descriptorCount = 1;
        writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[write_count++].pImageInfo = &images[AOT_GFX_MAX_TEXTURES + slot];
      }
      for (uint32_t slot = 0; slot < AOT_GFX_MAX_CONSTANT_BUFFERS; slot++) {
        const AOT_GFX_CONSTANT_DATA *constant = &draw->constants[slot];
        if (!constant->data_size) continue;
        buffers[slot] = (VkDescriptorBufferInfo){ constants_buffer,
          offsets[index].constants[slot], constant->data_size };
        writes[write_count] = (VkWriteDescriptorSet){
          VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[write_count].dstSet = descriptor_sets[index];
        writes[write_count].dstBinding = 8 + slot;
        writes[write_count].descriptorCount = 1;
        writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[write_count++].pBufferInfo = &buffers[slot];
      }
      vkUpdateDescriptorSets(dev, write_count, writes, 0, NULL);
    }
    free(allocated);
  }

  VkResult result = vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
  if (result != VK_SUCCESS) {
    LG("vkWaitForFences before frame failed: %d", (int)result);
    goto cleanup;
  }
  uint32_t image_index = 0;
  result = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acq,
      VK_NULL_HANDLE, &image_index);
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    LG("vkAcquireNextImageKHR returned OUT_OF_DATE; recreating swapchain");
    if (vkDeviceWaitIdle(dev) != VK_SUCCESS || make_sc())
      LG("swapchain recreation after acquire failed");
    goto cleanup;
  }
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    LG("vkAcquireNextImageKHR failed: %d", (int)result);
    goto cleanup;
  }
  acquired = 1;
  result = vkResetCommandBuffer(cb, 0);
  if (result != VK_SUCCESS) {
    LG("vkResetCommandBuffer failed: %d", (int)result);
    goto cleanup;
  }
  VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  result = vkBeginCommandBuffer(cb, &begin);
  if (result != VK_SUCCESS) {
    LG("vkBeginCommandBuffer failed: %d", (int)result);
    goto cleanup;
  }
  VkClearValue clear[2] = { { 0 }, { 0 } };
  memcpy(clear[0].color.float32, c4, 16);
  clear[1].depthStencil.depth = 1.0f;
  VkRenderPassBeginInfo render = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
  render.renderPass = render_pass;
  render.framebuffer = fb[image_index];
  render.renderArea = (VkRect2D){ { 0, 0 }, ext };
  render.clearValueCount = 2;
  render.pClearValues = clear;
  vkCmdBeginRenderPass(cb, &render, VK_SUBPASS_CONTENTS_INLINE);
  for (size_t index = 0; index < draw_count; index++) {
    const AOT_GFX_DRAW_CALL *draw = aot_gfx_draw_batch_get(batch, index);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[index]);
    VkViewport viewport = {
      0.0f, (float)ext.height, (float)ext.width, -(float)ext.height,
      0.0f, 1.0f
    };
    if (draw->has_viewport) {
      viewport = (VkViewport){
        draw->viewport.x, draw->viewport.y + draw->viewport.height,
        draw->viewport.width, -draw->viewport.height, draw->viewport.min_depth,
        draw->viewport.max_depth
      };
    }
    vkCmdSetViewport(cb, 0, 1, &viewport);
    VkRect2D scissor = { { 0, 0 }, ext };
    if (draw->has_scissor) {
      int64_t left = draw->scissor.left;
      int64_t top = draw->scissor.top;
      int64_t right = draw->scissor.right;
      int64_t bottom = draw->scissor.bottom;
      if (left < 0) left = 0;
      if (top < 0) top = 0;
      if (right < 0) right = 0;
      if (bottom < 0) bottom = 0;
      if ((uint64_t)left > ext.width) left = ext.width;
      if ((uint64_t)top > ext.height) top = ext.height;
      if ((uint64_t)right > ext.width) right = ext.width;
      if ((uint64_t)bottom > ext.height) bottom = ext.height;
      scissor.offset.x = (int32_t)left;
      scissor.offset.y = (int32_t)top;
      scissor.extent.width = (uint32_t)(right > left ? right - left : 0);
      scissor.extent.height = (uint32_t)(bottom > top ? bottom - top : 0);
    }
    vkCmdSetScissor(cb, 0, 1, &scissor);
    for (uint32_t slot = 0; slot < AOT_GFX_MAX_VERTEX_STREAMS; slot++) {
      if (!draw->vertex_streams[slot].data_size) continue;
      VkBuffer buffer = vertex_buffer;
      VkDeviceSize offset = offsets[index].vertex[slot] +
          draw->vertex_streams[slot].offset;
      vkCmdBindVertexBuffers(cb, slot, 1, &buffer, &offset);
    }
    if (descriptor_sets[index])
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
          pipeline_layout, 0, 1, &descriptor_sets[index], 0, NULL);
    if (draw->indexed) {
      vkCmdBindIndexBuffer(cb, index_buffer, offsets[index].indices,
          draw->index_format == 57u ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(cb, draw->vertex_count, 1, 0, draw->base_vertex, 0);
    } else {
      vkCmdDraw(cb, draw->vertex_count, 1, draw->first_vertex, 0);
    }
  }
  vkCmdEndRenderPass(cb);
  result = vkEndCommandBuffer(cb);
  if (result != VK_SUCCESS) {
    LG("vkEndCommandBuffer failed: %d", (int)result);
    goto cleanup;
  }
  result = vkResetFences(dev, 1, &fence);
  if (result != VK_SUCCESS) {
    LG("vkResetFences failed: %d", (int)result);
    goto cleanup;
  }
  fence_reset = 1;
  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &acq;
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cb;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &rel[image_index];
  result = vkQueueSubmit(q, 1, &submit, fence);
  if (result != VK_SUCCESS) {
    LG("vkQueueSubmit failed: %d", (int)result);
    goto cleanup;
  }
  queue_submitted = 1;
  if (stats) {
    stats->draw_calls = (uint32_t)draw_count;
    stats->descriptor_sets = descriptor_count;
    stats->vertex_bytes = vertex_total;
    stats->index_bytes = index_total;
    stats->constant_bytes = constants_used;
  }
  result = vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
  if (result != VK_SUCCESS) {
    LG("frame fence wait failed: %d", (int)result);
    goto cleanup;
  }
  VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &rel[image_index];
  present.swapchainCount = 1;
  present.pSwapchains = &sc;
  present.pImageIndices = &image_index;
  result = vkQueuePresentKHR(q, &present);
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    LG("vkQueuePresentKHR returned OUT_OF_DATE; recreating swapchain");
    if (vkDeviceWaitIdle(dev) != VK_SUCCESS || make_sc())
      LG("swapchain recreation after present failed");
    goto cleanup;
  }
  if (result == VK_SUBOPTIMAL_KHR) {
    vkDeviceWaitIdle(dev);
    if (make_sc()) goto cleanup;
    goto cleanup_success;
  }
  if (result != VK_SUCCESS) {
    LG("vkQueuePresentKHR failed: %d", (int)result);
    goto cleanup;
  }

cleanup_success:
  if (descriptor_count) {
    for (size_t i = 0; i < draw_count; i++)
      if (descriptor_sets[i])
        vkFreeDescriptorSets(dev, descriptor_pool, 1, &descriptor_sets[i]);
  }
  if (vertex_buffer) vkDestroyBuffer(dev, vertex_buffer, NULL);
  if (vertex_memory) vkFreeMemory(dev, vertex_memory, NULL);
  if (index_buffer) vkDestroyBuffer(dev, index_buffer, NULL);
  if (index_memory) vkFreeMemory(dev, index_memory, NULL);
  if (constants_buffer) vkDestroyBuffer(dev, constants_buffer, NULL);
  if (constants_memory) vkFreeMemory(dev, constants_memory, NULL);
  free(offsets); free(pipelines); free(descriptor_sets);
  return 0;

cleanup:
  free(vertex_blob);
  free(index_blob);
  free(constants_blob);
  if (queue_submitted || acquired || fence_reset) {
    if (reset_frame_sync(fence_reset && !queue_submitted)) {
      ok = 0;
      LG("failed to restore Vulkan frame synchronization after draw failure");
    }
  }
  if (descriptor_count) {
    for (size_t i = 0; i < draw_count; i++)
      if (descriptor_sets[i])
        vkFreeDescriptorSets(dev, descriptor_pool, 1, &descriptor_sets[i]);
  }
  if (vertex_buffer) vkDestroyBuffer(dev, vertex_buffer, NULL);
  if (vertex_memory) vkFreeMemory(dev, vertex_memory, NULL);
  if (index_buffer) vkDestroyBuffer(dev, index_buffer, NULL);
  if (index_memory) vkFreeMemory(dev, index_memory, NULL);
  if (constants_buffer) vkDestroyBuffer(dev, constants_buffer, NULL);
  if (constants_memory) vkFreeMemory(dev, constants_memory, NULL);
  free(offsets); free(pipelines); free(descriptor_sets);
  return -1;

cleanup_cpu:
  free(offsets); free(pipelines); free(descriptor_sets);
  return -1;
}

int aot_present_draw(const float c4[4], const uint32_t *vs, size_t vs_size,
                     const uint32_t *ps, size_t ps_size, const void *vertices,
                     uint32_t stride, const AOT_PRESENT_VERTEX_ATTRIBUTE *attrs,
                     uint32_t attr_count, uint32_t vertex_count, uint32_t first_vertex,
                     const AOT_PRESENT_TEXTURE *texture, const AOT_PRESENT_SAMPLER *sampler,
                     const void *vertex_constants, size_t vertex_constants_size) {
  AOT_GFX_DRAW_BATCH *batch = aot_gfx_draw_batch_create();
  if (!batch) return -1;
  int result = 0;
  if (vertex_count) {
    if (!stride || !vertices || first_vertex > SIZE_MAX / stride ||
        vertex_count > SIZE_MAX / stride - first_vertex) {
      aot_gfx_draw_batch_destroy(batch);
      return -1;
    }
    size_t data_size = ((size_t)first_vertex + vertex_count) * stride;
    AOT_GFX_DRAW_CALL draw;
    memset(&draw, 0, sizeof draw);
    draw.vertex_spirv = vs; draw.vertex_spirv_size = vs_size;
    draw.pixel_spirv = ps; draw.pixel_spirv_size = ps_size;
    draw.vertex_streams[0] = (AOT_GFX_VERTEX_STREAM){ vertices, data_size,
      stride, 0 };
    draw.attributes = attrs; draw.attribute_count = attr_count;
    draw.vertex_count = vertex_count; draw.first_vertex = first_vertex;
    draw.textures[0] = texture; draw.samplers[0] = sampler;
    draw.constants[0] = (AOT_GFX_CONSTANT_DATA){ vertex_constants,
      vertex_constants_size };
    if (aot_gfx_draw_batch_append(batch, &draw, NULL, 0, NULL, NULL))
      result = -1;
  }
  AOT_PRESENT_FRAME_STATS stats;
  if (!result) result = aot_present_draw_batch(c4, batch, &stats);
  aot_gfx_draw_batch_destroy(batch);
  return result;
}
void aot_present_clear(const float c4[4]) {
  (void)aot_present_draw(c4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
#endif
