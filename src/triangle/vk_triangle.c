/* Standalone Vulkan bring-up for Android: surface -> device -> swapchain -> render pass -> pipeline -> rotating triangle. */
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vk_triangle.h"

static const uint32_t vs_spv[] =
#include "tri.vert.inc"
;
static const uint32_t fs_spv[] =
#include "tri.frag.inc"
;

#define FRAMES 2
#define MAXIMG 8
static FILE *g_log;
void tri_open_log(const char *path) { g_log = fopen(path, "w"); }
void tri_log(const char *fmt, ...) {
  char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  __android_log_write(ANDROID_LOG_INFO, "aottri", b); if (g_log) { fputs(b, g_log); fputc('\n', g_log); fflush(g_log); }
}
#define VKC(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { tri_log("FAILED %s -> %d (line %d)", #x, (int)_r, __LINE__); return -1; } } while (0)

struct TriCtx {
  ANativeWindow *win; VkInstance inst; VkSurfaceKHR surf; VkPhysicalDevice pd; VkDevice dev; uint32_t qf; VkQueue q;
  VkSwapchainKHR sc; VkFormat fmt; VkExtent2D ext; uint32_t n; VkImage img[MAXIMG]; VkImageView view[MAXIMG]; VkFramebuffer fb[MAXIMG]; VkSemaphore rel[MAXIMG];
  VkRenderPass rp; VkPipelineLayout pl; VkPipeline pipe; VkCommandPool pool; VkCommandBuffer cb[FRAMES]; VkSemaphore acq[FRAMES]; VkFence fence[FRAMES];
  uint32_t frame; int have_dev; float cs, sn, aspect;
};
TriCtx *tri_create(void) { return calloc(1, sizeof(TriCtx)); }

static void destroy_sc(TriCtx *c, int keep_sc) {
  for (uint32_t i = 0; i < c->n; i++) {
    if (c->fb[i]) vkDestroyFramebuffer(c->dev, c->fb[i], 0); if (c->view[i]) vkDestroyImageView(c->dev, c->view[i], 0); if (c->rel[i]) vkDestroySemaphore(c->dev, c->rel[i], 0);
    c->fb[i] = 0; c->view[i] = 0; c->rel[i] = 0;
  }
  c->n = 0; if (!keep_sc && c->sc) { vkDestroySwapchainKHR(c->dev, c->sc, 0); c->sc = 0; }
}

static int make_sc(TriCtx *c) {
  VkSurfaceCapabilitiesKHR cap; VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c->pd, c->surf, &cap));
  uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(c->pd, c->surf, &nf, 0); VkSurfaceFormatKHR fm[32]; if (nf > 32) nf = 32; VKC(vkGetPhysicalDeviceSurfaceFormatsKHR(c->pd, c->surf, &nf, fm));
  VkSurfaceFormatKHR pick = fm[0];
  for (uint32_t i = 0; i < nf; i++) if ((fm[i].format == VK_FORMAT_R8G8B8A8_UNORM || fm[i].format == VK_FORMAT_B8G8R8A8_UNORM) && fm[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { pick = fm[i]; break; }
  VkExtent2D e = cap.currentExtent;
  if (e.width == 0xFFFFFFFFu) { e.width = (uint32_t)ANativeWindow_getWidth(c->win); e.height = (uint32_t)ANativeWindow_getHeight(c->win); }
  VkSurfaceTransformFlagBitsKHR tf = cap.currentTransform;   /* pre-rotate in the shader instead of making the compositor rotate */
  float phi = 0.0f; int swap = 0;
  if (tf == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) { phi = -1.5707963f; swap = 1; }
  else if (tf == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) phi = 3.1415927f;
  else if (tf == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) { phi = 1.5707963f; swap = 1; }
  c->cs = cosf(phi); c->sn = sinf(phi);
  c->aspect = swap ? (float)e.width / (float)e.height : (float)e.height / (float)e.width;   /* display height / display width */
  uint32_t cnt = cap.minImageCount + 1; if (cap.maxImageCount && cnt > cap.maxImageCount) cnt = cap.maxImageCount; if (cnt > MAXIMG) cnt = MAXIMG;
  VkCompositeAlphaFlagBitsKHR ca = (cap.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  VkSwapchainKHR old = c->sc; destroy_sc(c, 1);
  VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
  sci.surface = c->surf; sci.minImageCount = cnt; sci.imageFormat = pick.format; sci.imageColorSpace = pick.colorSpace; sci.imageExtent = e; sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; sci.preTransform = tf; sci.compositeAlpha = ca;
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; sci.clipped = VK_TRUE; sci.oldSwapchain = old;
  VKC(vkCreateSwapchainKHR(c->dev, &sci, 0, &c->sc)); if (old) vkDestroySwapchainKHR(c->dev, old, 0);
  c->fmt = pick.format; c->ext = e; c->n = MAXIMG; VKC(vkGetSwapchainImagesKHR(c->dev, c->sc, &c->n, c->img));
  tri_log("swapchain: %ux%u, %u images, format %d, preTransform %d, display aspect %.3f", e.width, e.height, c->n, (int)pick.format, (int)tf, c->aspect);
  if (!c->rp) {   /* render pass depends only on the format; build once */
    VkAttachmentDescription ad = { 0, c->fmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 1, &ar };
    VkSubpassDependency dep = { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 };
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO }; rpi.attachmentCount = 1; rpi.pAttachments = &ad; rpi.subpassCount = 1; rpi.pSubpasses = &sp; rpi.dependencyCount = 1; rpi.pDependencies = &dep;
    VKC(vkCreateRenderPass(c->dev, &rpi, 0, &c->rp));
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO }; VkShaderModule vm, fmod;
    smi.codeSize = sizeof vs_spv; smi.pCode = vs_spv; VKC(vkCreateShaderModule(c->dev, &smi, 0, &vm));
    smi.codeSize = sizeof fs_spv; smi.pCode = fs_spv; VKC(vkCreateShaderModule(c->dev, &smi, 0, &fmod));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr; VKC(vkCreatePipelineLayout(c->dev, &pli, 0, &c->pl));
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main" }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fmod, "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO }; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO }; vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO }; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO }; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = { 0 }; ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO }; cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO }; ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO }; gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.pDynamicState = &ds; gp.layout = c->pl; gp.renderPass = c->rp;
    VKC(vkCreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &gp, 0, &c->pipe));
    vkDestroyShaderModule(c->dev, vm, 0); vkDestroyShaderModule(c->dev, fmod, 0);
  }
  for (uint32_t i = 0; i < c->n; i++) {
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO }; vi.image = c->img[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = c->fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1; VKC(vkCreateImageView(c->dev, &vi, 0, &c->view[i]));
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO }; fi.renderPass = c->rp; fi.attachmentCount = 1; fi.pAttachments = &c->view[i]; fi.width = e.width; fi.height = e.height; fi.layers = 1;
    VKC(vkCreateFramebuffer(c->dev, &fi, 0, &c->fb[i]));
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO }; VKC(vkCreateSemaphore(c->dev, &si, 0, &c->rel[i]));
  }
  return 0;
}

int tri_init(TriCtx *c, ANativeWindow *w) {
  c->win = w;
  const char *ie[2] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
  VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO }; ai.pApplicationName = "aottri"; ai.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ii.pApplicationInfo = &ai; ii.enabledExtensionCount = 2; ii.ppEnabledExtensionNames = ie;
  VKC(vkCreateInstance(&ii, 0, &c->inst));
  VkAndroidSurfaceCreateInfoKHR si = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR }; si.window = w; VKC(vkCreateAndroidSurfaceKHR(c->inst, &si, 0, &c->surf));
  uint32_t np = 0; VKC(vkEnumeratePhysicalDevices(c->inst, &np, 0)); if (!np) { tri_log("no Vulkan physical device"); return -1; }
  VkPhysicalDevice pds[8]; if (np > 8) np = 8; VKC(vkEnumeratePhysicalDevices(c->inst, &np, pds));
  for (uint32_t i = 0; i < np && !c->pd; i++) {
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, 0); VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qp);
    for (uint32_t q = 0; q < nq; q++) { VkBool32 ok = 0; vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, c->surf, &ok); if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ok) { c->pd = pds[i]; c->qf = q; break; } }
  }
  if (!c->pd) { tri_log("no graphics+present queue"); return -1; }
  VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(c->pd, &pp); tri_log("GPU: %s (Vulkan %u.%u)", pp.deviceName, VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion));
  float pr = 1.0f; VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO }; qi.queueFamilyIndex = c->qf; qi.queueCount = 1; qi.pQueuePriorities = &pr;
  const char *de[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
  VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO }; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = de;
  VKC(vkCreateDevice(c->pd, &di, 0, &c->dev)); c->have_dev = 1; vkGetDeviceQueue(c->dev, c->qf, 0, &c->q);
  VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = c->qf; VKC(vkCreateCommandPool(c->dev, &pi, 0, &c->pool));
  VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; cbi.commandPool = c->pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = FRAMES; VKC(vkAllocateCommandBuffers(c->dev, &cbi, c->cb));
  for (int i = 0; i < FRAMES; i++) {
    VkSemaphoreCreateInfo s = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO }; VKC(vkCreateSemaphore(c->dev, &s, 0, &c->acq[i]));
    VkFenceCreateInfo f = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; f.flags = VK_FENCE_CREATE_SIGNALED_BIT; VKC(vkCreateFence(c->dev, &f, 0, &c->fence[i]));
  }
  if (make_sc(c)) return -1;
  tri_log("Vulkan ready - drawing triangle"); return 0;
}

int tri_recreate(TriCtx *c) { vkDeviceWaitIdle(c->dev); return make_sc(c); }

int tri_draw(TriCtx *c, float t) {
  uint32_t f = c->frame; c->frame = (c->frame + 1) % FRAMES;
  VKC(vkWaitForFences(c->dev, 1, &c->fence[f], VK_TRUE, UINT64_MAX));
  uint32_t idx; VkResult r = vkAcquireNextImageKHR(c->dev, c->sc, UINT64_MAX, c->acq[f], VK_NULL_HANDLE, &idx);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) return 1; if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { tri_log("acquire failed %d", (int)r); return -1; }
  VKC(vkResetFences(c->dev, 1, &c->fence[f]));
  VkCommandBuffer cb = c->cb[f]; VKC(vkResetCommandBuffer(cb, 0));
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; VKC(vkBeginCommandBuffer(cb, &bi));
  VkClearValue cv; cv.color.float32[0] = 0.04f; cv.color.float32[1] = 0.05f; cv.color.float32[2] = 0.11f; cv.color.float32[3] = 1.0f;
  VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO }; rb.renderPass = c->rp; rb.framebuffer = c->fb[idx]; rb.renderArea.extent = c->ext; rb.clearValueCount = 1; rb.pClearValues = &cv;
  vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipe);
  VkViewport vp = { 0, 0, (float)c->ext.width, (float)c->ext.height, 0, 1 }; VkRect2D sc = { { 0, 0 }, c->ext }; vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
  float pc[4] = { t, c->aspect, c->cs, c->sn }; vkCmdPushConstants(cb, c->pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, pc);
  vkCmdDraw(cb, 6, 1, 0, 0); vkCmdEndRenderPass(cb); VKC(vkEndCommandBuffer(cb));
  VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo sub = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; sub.waitSemaphoreCount = 1; sub.pWaitSemaphores = &c->acq[f]; sub.pWaitDstStageMask = &ws; sub.commandBufferCount = 1; sub.pCommandBuffers = &cb; sub.signalSemaphoreCount = 1; sub.pSignalSemaphores = &c->rel[idx];
  VKC(vkQueueSubmit(c->q, 1, &sub, c->fence[f]));
  VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR }; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &c->rel[idx]; pi.swapchainCount = 1; pi.pSwapchains = &c->sc; pi.pImageIndices = &idx;
  r = vkQueuePresentKHR(c->q, &pi); if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) return 1;
  if (r != VK_SUCCESS) { tri_log("present failed %d", (int)r); return -1; }
  return 0;
}

void tri_destroy(TriCtx *c) {
  if (!c) return;
  if (c->have_dev) {
    vkDeviceWaitIdle(c->dev); destroy_sc(c, 0);
    for (int i = 0; i < FRAMES; i++) { if (c->acq[i]) vkDestroySemaphore(c->dev, c->acq[i], 0); if (c->fence[i]) vkDestroyFence(c->dev, c->fence[i], 0); }
    if (c->pipe) vkDestroyPipeline(c->dev, c->pipe, 0); if (c->pl) vkDestroyPipelineLayout(c->dev, c->pl, 0); if (c->rp) vkDestroyRenderPass(c->dev, c->rp, 0);
    if (c->pool) vkDestroyCommandPool(c->dev, c->pool, 0); vkDestroyDevice(c->dev, 0);
  }
  if (c->surf) vkDestroySurfaceKHR(c->inst, c->surf, 0); if (c->inst) vkDestroyInstance(c->inst, 0);
  free(c);
}
