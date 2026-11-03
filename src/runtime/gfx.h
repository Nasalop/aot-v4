#pragma once
#include <stddef.h>
#include <stdint.h>
#include "aot_runtime.h"

#ifdef __ANDROID__
#include <android/native_window.h>
#include <vulkan/vulkan.h>
#else
typedef struct ANativeWindow ANativeWindow;
typedef uint64_t VkDeviceSize;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t AOT_HRESULT;
#define AOT_OK 0
#define AOT_E_INVALIDARG (-1)
#define AOT_E_NOMEM (-2)
#define AOT_E_UNSUPPORTED (-3)
#define AOT_E_STATE (-4)

typedef enum { AOT_FORMAT_RGBA8 = 1, AOT_FORMAT_BGRA8 = 2, AOT_FORMAT_R32_FLOAT = 3 } AOT_FORMAT;
typedef enum { AOT_FORMAT_R32G32_FLOAT = 4, AOT_FORMAT_R32G32B32_FLOAT = 5, AOT_FORMAT_R32G32B32A32_FLOAT = 6,
               AOT_FORMAT_R32_UINT = 7, AOT_FORMAT_R32G32_UINT = 8, AOT_FORMAT_R8G8B8A8_UNORM = 9, AOT_INPUT_R32_FLOAT = 10 } AOT_INPUT_FORMAT;
typedef enum { AOT_USAGE_DEFAULT = 0, AOT_USAGE_DYNAMIC = 1, AOT_USAGE_STAGING = 2 } AOT_USAGE;
typedef enum { AOT_PER_VERTEX_DATA = 0, AOT_PER_INSTANCE_DATA = 1 } AOT_INPUT_CLASS;
typedef enum { AOT_TOPOLOGY_UNDEFINED = 0, AOT_TOPOLOGY_POINTLIST = 1, AOT_TOPOLOGY_LINELIST = 2,
               AOT_TOPOLOGY_LINESTRIP = 3, AOT_TOPOLOGY_TRIANGLELIST = 4, AOT_TOPOLOGY_TRIANGLESTRIP = 5,
               AOT_TOPOLOGY_LINELIST_ADJ = 10, AOT_TOPOLOGY_LINESTRIP_ADJ = 11,
               AOT_TOPOLOGY_TRIANGLELIST_ADJ = 12, AOT_TOPOLOGY_TRIANGLESTRIP_ADJ = 13 } AOT_TOPOLOGY;
typedef struct { uint32_t ByteWidth, BindFlags, CPUAccessFlags; AOT_USAGE Usage; } AOT_BUFFER_DESC;
typedef struct { uint32_t Width, Height, MipLevels, ArraySize, BindFlags; AOT_FORMAT Format; AOT_USAGE Usage; } AOT_TEXTURE2D_DESC;
typedef struct { const void *pSysMem; uint32_t SysMemPitch, SysMemSlicePitch; } AOT_SUBRESOURCE_DATA;
typedef struct { uint32_t FirstElement, NumElements; } AOT_BUFFER_VIEW_DESC;
typedef struct { uint32_t MipSlice; } AOT_TEXTURE_VIEW_DESC;
typedef struct { uint32_t ByteWidth, RowPitch; } AOT_MAPPED_SUBRESOURCE;
typedef struct { const char *SemanticName; uint32_t SemanticIndex; AOT_INPUT_FORMAT Format; uint32_t InputSlot, AlignedByteOffset; AOT_INPUT_CLASS InputSlotClass; uint32_t InstanceDataStepRate; } AOT_INPUT_ELEMENT_DESC;
typedef struct { uint64_t Offset, Range; const void *Buffer; } AOT_UNIFORM_BUFFER_INFO;
typedef struct { AOT_FORMAT Format; uint32_t MostDetailedMip, MipLevels; } AOT_SRV_DESC;
typedef struct { AOT_FORMAT Format; uint32_t MipSlice; } AOT_RTV_DESC;
typedef struct { AOT_FORMAT Format; uint32_t MipSlice; uint32_t ReadOnlyDepth, ReadOnlyStencil; } AOT_DSV_DESC;
typedef struct { uint32_t Filter, AddressU, AddressV, AddressW; float MipLODBias, MaxAnisotropy, MinLOD, MaxLOD; } AOT_SAMPLER_DESC;

typedef struct AOT_VK AOT_VK;
typedef struct AOT_BUFFER AOT_BUFFER;
typedef struct AOT_TEXTURE2D AOT_TEXTURE2D;
typedef struct AOT_VIEW AOT_VIEW;
typedef struct AOT_SHADER AOT_SHADER;
typedef struct AOT_INPUT_LAYOUT AOT_INPUT_LAYOUT;
typedef struct AOT_CBV AOT_CBV;
typedef struct AOT_SAMPLER AOT_SAMPLER;
typedef struct AOT_CONTEXT AOT_CONTEXT;

/* Android Vulkan foundation. window may be NULL for headless/offscreen use. */
AOT_HRESULT aot_vk_create(ANativeWindow *window, uint32_t width, uint32_t height, AOT_VK **out);
void aot_vk_destroy(AOT_VK *vk);
AOT_HRESULT aot_vk_begin_frame(AOT_VK *vk);
AOT_HRESULT aot_vk_end_frame(AOT_VK *vk);

/* Small D3D11-shaped compatibility subset used by translated imports. */
AOT_HRESULT aot_d3d11_create(AOT_VK *vk, AOT_CONTEXT **out);
void aot_d3d11_destroy(AOT_CONTEXT *ctx);
AOT_HRESULT aot_create_buffer(AOT_CONTEXT *, const AOT_BUFFER_DESC *, const AOT_SUBRESOURCE_DATA *, AOT_BUFFER **);
AOT_HRESULT aot_create_texture2d(AOT_CONTEXT *, const AOT_TEXTURE2D_DESC *, const AOT_SUBRESOURCE_DATA *, AOT_TEXTURE2D **);
AOT_HRESULT aot_create_buffer_view(AOT_CONTEXT *, AOT_BUFFER *, const AOT_BUFFER_VIEW_DESC *, AOT_VIEW **);
AOT_HRESULT aot_create_texture_view(AOT_CONTEXT *, AOT_TEXTURE2D *, const AOT_TEXTURE_VIEW_DESC *, AOT_VIEW **);
AOT_HRESULT aot_create_shader(AOT_CONTEXT *, const void *bytecode, size_t size, AOT_SHADER **);
AOT_HRESULT aot_create_cbv(AOT_CONTEXT *, AOT_BUFFER *, uint32_t first_constant, uint32_t num_constants, AOT_CBV **);
void aot_release_cbv(AOT_CBV *);
void aot_set_constant_buffer(AOT_CONTEXT *, uint32_t slot, AOT_CBV *);
AOT_CBV *aot_get_constant_buffer(const AOT_CONTEXT *, uint32_t slot);
AOT_HRESULT aot_update_constant_buffer(AOT_CONTEXT *, AOT_CBV *, const void *, size_t);
AOT_UNIFORM_BUFFER_INFO aot_uniform_buffer_info(const AOT_CBV *);
AOT_HRESULT aot_create_srv(AOT_CONTEXT *, AOT_TEXTURE2D *, const AOT_SRV_DESC *, AOT_VIEW **);
AOT_HRESULT aot_create_rtv(AOT_CONTEXT *, AOT_TEXTURE2D *, const AOT_RTV_DESC *, AOT_VIEW **);
AOT_HRESULT aot_create_dsv(AOT_CONTEXT *, AOT_TEXTURE2D *, const AOT_DSV_DESC *, AOT_VIEW **);
AOT_HRESULT aot_create_sampler(AOT_CONTEXT *, const AOT_SAMPLER_DESC *, AOT_SAMPLER **);
void aot_release_sampler(AOT_SAMPLER *);
void aot_set_shader_resources(AOT_CONTEXT *, uint32_t slot, AOT_VIEW *);
void aot_set_samplers(AOT_CONTEXT *, uint32_t slot, AOT_SAMPLER *);
void aot_set_render_targets(AOT_CONTEXT *, uint32_t count, AOT_VIEW *const *, AOT_VIEW *);
AOT_VIEW *aot_get_render_target(const AOT_CONTEXT *, uint32_t slot);
AOT_VIEW *aot_get_depth_stencil(const AOT_CONTEXT *);
AOT_VIEW *aot_get_shader_resource(const AOT_CONTEXT *, uint32_t slot);
AOT_SAMPLER *aot_get_sampler(const AOT_CONTEXT *, uint32_t slot);
AOT_HRESULT aot_create_input_layout(AOT_CONTEXT *, const AOT_INPUT_ELEMENT_DESC *, uint32_t count, AOT_INPUT_LAYOUT **);
void aot_release_input_layout(AOT_INPUT_LAYOUT *);
void aot_set_input_layout(AOT_CONTEXT *, AOT_INPUT_LAYOUT *);
void aot_set_primitive_topology(AOT_CONTEXT *, AOT_TOPOLOGY);
AOT_TOPOLOGY aot_primitive_topology(const AOT_CONTEXT *);
void aot_release_buffer(AOT_BUFFER *); void aot_release_texture2d(AOT_TEXTURE2D *);
void aot_release_view(AOT_VIEW *); void aot_release_shader(AOT_SHADER *);
AOT_HRESULT aot_map(AOT_CONTEXT *, AOT_BUFFER *, AOT_MAPPED_SUBRESOURCE *);
void aot_unmap(AOT_CONTEXT *, AOT_BUFFER *);
AOT_HRESULT aot_update_buffer(AOT_CONTEXT *, AOT_BUFFER *, const void *, size_t);
AOT_HRESULT aot_clear(AOT_CONTEXT *, AOT_VIEW *, float r, float g, float b, float a);
AOT_HRESULT aot_draw(AOT_CONTEXT *, uint32_t vertex_count, uint32_t first_vertex);
uint64_t aot_draw_count(const AOT_CONTEXT *);

/* Guest-callable COM objects. Vtable entries are AOT thunk addresses. */
uint64_t aot_d3d11_create_device_from_cpu(CPU *);

#ifdef __cplusplus
}
#endif
