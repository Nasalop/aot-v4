#include "dxbc_translate.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include "dxbc/dxbc_api.h"
#include "spirv/spirv_builder.h"

namespace {

class AotResourceMapping final : public dxbc_spv::spirv::ResourceMapping {
public:
  dxbc_spv::spirv::DescriptorBinding mapDescriptor(
      dxbc_spv::ir::ScalarType type, uint32_t reg_space, uint32_t reg_index) override {
    if (reg_space == 0 && reg_index < 4) {
      if (type == dxbc_spv::ir::ScalarType::eSrv)
        return { 0, reg_index };
      if (type == dxbc_spv::ir::ScalarType::eSampler)
        return { 0, 4 + reg_index };
      if (type == dxbc_spv::ir::ScalarType::eCbv)
        return { 0, 8 + reg_index };
    }
    return { 0, 31 };
  }

  uint32_t mapPushData(dxbc_spv::ir::ShaderStageMask stages) override {
    return m_fallback.mapPushData(stages);
  }

private:
  dxbc_spv::spirv::BasicResourceMapping m_fallback;
};

}

extern "C" int aot_dxbc_to_spirv(const void *data, size_t size, uint32_t **spirv, size_t *spirv_size) {
  if (!data || !size || !spirv || !spirv_size)
    return AOT_DXBC_INVALID_ARGUMENT;

  *spirv = nullptr;
  *spirv_size = 0;

  dxbc_spv::dxbc::Converter::Options convert_options = {};
  auto ir = dxbc_spv::dxbc::compileShaderToLegalizedIr(
      data, size, convert_options, dxbc_spv::ir::CompileOptions{});
  if (!ir)
    return AOT_DXBC_COMPILE_FAILED;

  AotResourceMapping mapping;
  dxbc_spv::spirv::SpirvBuilder::Options spirv_options = {};
  dxbc_spv::spirv::SpirvBuilder builder(*ir, mapping, spirv_options);
  builder.buildSpirvBinary();
  std::vector<uint32_t> binary = builder.getSpirvBinary();
  if (binary.size() < 5 || binary[0] != 0x07230203u)
    return AOT_DXBC_COMPILE_FAILED;
  if (binary.size() > SIZE_MAX / sizeof(uint32_t))
    return AOT_DXBC_OUT_OF_MEMORY;

  size_t byte_count = binary.size() * sizeof(uint32_t);
  uint32_t *copy = static_cast<uint32_t *>(std::malloc(byte_count));
  if (!copy)
    return AOT_DXBC_OUT_OF_MEMORY;
  std::memcpy(copy, binary.data(), byte_count);
  *spirv = copy;
  *spirv_size = byte_count;
  return AOT_DXBC_OK;
}

extern "C" void aot_dxbc_free_spirv(uint32_t *spirv) {
  std::free(spirv);
}
