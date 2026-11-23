#include <optional>
#include <utility>

#include <dxbc/dxbc_container.h>
#include <dxbc/dxbc_interface.h>
#include <dxbc/dxbc_parser.h>

#include "d3d11_device.h"
#include "d3d11_shader.h"

namespace dxvk {

  class D3D11ShaderConverter : public DxvkIrShaderConverter {

  public:

    D3D11ShaderConverter(
      const DxvkShaderHash&         ShaderKey,
      const DxvkIrShaderCreateInfo& ModuleInfo,
      const void*                   pShaderBytecode,
            size_t                  BytecodeLength,
            bool                    LowerIcb)
    : m_key(ShaderKey), m_info(ModuleInfo), m_lowerIcb(LowerIcb) {
      m_dxbc.resize(BytecodeLength);
      std::memcpy(m_dxbc.data(), pShaderBytecode, BytecodeLength);
    }

    ~D3D11ShaderConverter() { }

    void convertShader(
            dxbc_spv::ir::Builder&    builder) {
      auto debugName = m_key.toString();

      dxbc_spv::dxbc::Converter::Options options = { };
      options.name = debugName.c_str();
      options.includeDebugNames = true;
      options.boundCheckShaderIo = true;
      options.lowerIcb = m_lowerIcb;
      options.icbRegisterSpace = 0u;
      options.icbRegisterIndex = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
      options.classInstanceRegisterSpace = 0u;
      options.classInstanceRegisterIndex = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT + 1u;
      options.limitTessFactor = true;

      dxbc_spv::dxbc::Container container(m_dxbc.data(), m_dxbc.size());

      dxbc_spv::dxbc::ShaderInfo shaderInfo =
        dxbc_spv::dxbc::Parser(container.getCodeChunk()).getShaderInfo();

      dxbc_spv::dxbc::Converter converter(std::move(container), options);

      // Determine whether to create a regular shader or a pass-through GS
      auto dstIsGs = m_key.stage() == VK_SHADER_STAGE_GEOMETRY_BIT;
      auto srcIsGs = shaderInfo.getType() == dxbc_spv::dxbc::ShaderType::eGeometry;

      if (dstIsGs && !srcIsGs) {
        if (!converter.createPassthroughGs(builder))
          throw DxvkError(str::format("Failed to create pass-through geometry shader: ", m_key.toString()));
      } else {
        if (!converter.convertShader(builder))
          throw DxvkError(str::format("Failed to convert shader: ", m_key.toString()));

        lowerBuiltIns(builder);
        lowerBgraUavAccess(builder);
      }
    }

    uint32_t determineResourceIndex(
            dxbc_spv::ir::ShaderStage stage,
            dxbc_spv::ir::ScalarType  type,
            uint32_t                  regSpace,
            uint32_t                  regIndex) const {
      switch (type) {
        case dxbc_spv::ir::ScalarType::eSampler:
          return D3D11ShaderResourceMapping::computeSamplerBinding(stage, regIndex);
        case dxbc_spv::ir::ScalarType::eCbv:
          return D3D11ShaderResourceMapping::computeCbvBinding(stage, regIndex);
        case dxbc_spv::ir::ScalarType::eSrv:
          return D3D11ShaderResourceMapping::computeSrvBinding(stage, regIndex);
        case dxbc_spv::ir::ScalarType::eUav:
          return D3D11ShaderResourceMapping::computeUavBinding(stage, regIndex);
        case dxbc_spv::ir::ScalarType::eUavCounter:
          return D3D11ShaderResourceMapping::computeUavCounterBinding(stage, regIndex);
        default:
          return -1u;
      }
    }

    void dumpSource(const std::string& path) const {
      std::ofstream file(str::topath(str::format(path, "/", m_key.toString(), ".dxbc").c_str()).c_str(), std::ios_base::trunc | std::ios_base::binary);
      file.write(reinterpret_cast<const char*>(m_dxbc.data()), m_dxbc.size());
    }

    std::string getDebugName() const {
      return m_key.toString();
    }

  private:

    std::vector<uint8_t> m_dxbc;

    DxvkShaderHash          m_key;
    DxvkIrShaderCreateInfo  m_info;

    bool                    m_lowerIcb = false;

    struct BuiltInInfo {
      dxbc_spv::ir::BuiltIn builtIn;
      dxbc_spv::ir::BasicType type;
      const char* name;
    };

    // Vulkan storage views cannot have a component mapping. Specialize the
    // R/B exchange per bound UAV, leaving the underlying BGRA bytes intact.
    static void lowerBgraUavAccess(dxbc_spv::ir::Builder& builder) {
      using namespace dxbc_spv;
      auto entryPoint = findEntryPoint(builder).first;
      std::array<ir::SsaDef, 2u> masks = { };
      small_vector<ir::SsaDef, 32u> accesses;
      for (auto iter = builder.getCode().first; iter != builder.getCode().second; iter++) {
        if (iter->getOpCode() == ir::OpCode::eImageLoad
         || iter->getOpCode() == ir::OpCode::eImageStore)
          accesses.push_back(iter->getDef());
      }

      for (auto ref : accesses) {
        auto op = builder.getOp(ref);
        auto descriptor = builder.getOpForOperand(op, 0u);
        if (descriptor.getOpCode() != ir::OpCode::eDescriptorLoad)
          continue;
        auto decl = builder.getOpForOperand(descriptor, 0u);
        if (decl.getOpCode() != ir::OpCode::eDclUav)
          continue;

        uint32_t reg = uint32_t(decl.getOperand(2u));
        uint32_t count = uint32_t(decl.getOperand(3u));
        if (uint32_t(decl.getOperand(1u)) || reg >= 64u)
          continue;

        auto getMask = [&] (uint32_t word) {
          if (!masks[word])
            masks[word] = builder.add(ir::Op::DclSpecConstant(
              ir::ScalarType::eU32, entryPoint, 1u + word, 0u));
          return masks[word];
        };

        ir::SsaDef mask, bitIndex;
        if (count == 1u) {
          mask = getMask(reg / 32u);
          bitIndex = builder.makeConstant(reg % 32u);
        } else {
          auto index = builder.addBefore(ref, ir::Op::IAdd(ir::ScalarType::eU32,
            ir::SsaDef(descriptor.getOperand(1u)), builder.makeConstant(reg)));
          auto high = builder.addBefore(ref, ir::Op::UGe(ir::ScalarType::eBool,
            index, builder.makeConstant(32u)));
          mask = builder.addBefore(ref, ir::Op::Select(ir::ScalarType::eU32,
            high, getMask(1u), getMask(0u)));
          bitIndex = builder.addBefore(ref, ir::Op::IAnd(ir::ScalarType::eU32,
            index, builder.makeConstant(31u)));
        }
        auto bit = builder.addBefore(ref, ir::Op::UBitExtract(ir::ScalarType::eU32,
          mask, bitIndex, builder.makeConstant(1u)));
        auto enabled = builder.addBefore(ref, ir::Op::INe(ir::ScalarType::eBool,
          bit, builder.makeConstant(0u)));

        bool isLoad = op.getOpCode() == ir::OpCode::eImageLoad;
        auto value = isLoad ? builder.addBefore(ref, op) : ir::SsaDef(op.getOperand(3u));
        auto type = builder.getOp(value).getType();
        auto texelType = type.isStructType() ? type.getSubType(1u) : type;
        auto texel = value;
        if (type.isStructType())
          texel = builder.addBefore(ref, ir::Op::CompositeExtract(
            texelType, value, builder.makeConstant(1u)));

        auto scalar = texelType.getBaseType(0u).getBaseType();
        std::array<ir::SsaDef, 4u> components;
        for (uint32_t i = 0; i < 4u; i++)
          components[i] = builder.addBefore(ref, ir::Op::CompositeExtract(
            scalar, texel, builder.makeConstant(i)));
        auto red = builder.addBefore(ref, ir::Op::Select(
          scalar, enabled, components[2], components[0]));
        auto blue = builder.addBefore(ref, ir::Op::Select(
          scalar, enabled, components[0], components[2]));
        auto result = builder.addBefore(ref, ir::Op::CompositeConstruct(
          texelType, red, components[1], blue, components[3]));

        if (isLoad) {
          if (type.isStructType()) {
            auto feedback = builder.addBefore(ref, ir::Op::CompositeExtract(
              type.getSubType(0u), value, builder.makeConstant(0u)));
            result = builder.addBefore(ref, ir::Op::CompositeConstruct(type, feedback, result));
          }
          builder.rewriteDef(ref, result);
        } else {
          op.setOperand(3u, result);
          builder.rewriteOp(ref, std::move(op));
        }
      }
    }

    static void lowerBuiltIns(dxbc_spv::ir::Builder& builder) {
      using namespace dxbc_spv;

      // Nothing to do for compute, our speshul built-ins are
      // only used in graphics pipelines.
      auto [entryPoint, shaderStage] = findEntryPoint(builder);

      ir::SsaDef pushData = {};
      ir::SsaDef specSampleCount = {};

      switch (shaderStage) {
        case ir::ShaderStage::eHull: {
          pushData = builder.add(ir::Op::DclPushData(
            ir::ScalarType::eF32, entryPoint, 0u, shaderStage));
          builder.add(ir::Op::DebugName(pushData, "maxTessFactor"));
        } break;

        case ir::ShaderStage::ePixel: {
          specSampleCount = builder.add(ir::Op::DclSpecConstant(
            ir::ScalarType::eU32, entryPoint, 0u, 0u));
          builder.add(ir::Op::DebugName(specSampleCount, "vRasterizer"));
        } break;

        default:
          return;
      }

      // Gather built-in inputs
      small_vector<ir::SsaDef, 32u> inputs;

      for (auto iter = builder.getDeclarations().first;
                iter != builder.getDeclarations().second; iter++) {
        if (iter->getOpCode() == ir::OpCode::eDclInputBuiltIn)
          inputs.push_back(iter->getDef());
      }

      // Rewrite input loads as push data loads
      for (auto inputDef : inputs) {
        const auto& inputOp = builder.getOp(inputDef);

        auto builtIn = ir::BuiltIn(inputOp.getOperand(inputOp.getFirstLiteralOperandIndex()));

        switch (builtIn) {
          case ir::BuiltIn::eSampleCount: {
            dxbc_spv_assert(specSampleCount);
            rewriteBuiltIn(builder, inputDef, specSampleCount);
          } break;

          case ir::BuiltIn::eTessFactorLimit: {
            dxbc_spv_assert(pushData);
            rewriteBuiltIn(builder, inputDef, pushData);
          } break;

          default:
            break;
        }
      }
    }

    static void rewriteBuiltIn(dxbc_spv::ir::Builder& builder, dxbc_spv::ir::SsaDef oldDef, dxbc_spv::ir::SsaDef newDef) {
      using namespace dxbc_spv;

      small_vector<ir::SsaDef, 32u> uses;
      builder.getUses(oldDef, uses);

      const auto& newOp = builder.getOp(newDef);

      for (auto use : uses) {
        const auto& useOp = builder.getOp(use);

        if (useOp.getOpCode() == ir::OpCode::eInputLoad) {
          if (newOp.getOpCode() == ir::OpCode::eDclPushData) {
            auto loadType = useOp.getType().getBaseType(0u);
            builder.rewriteOp(use, ir::Op::PushDataLoad(loadType, newDef, ir::SsaDef()));
          } else {
            builder.rewriteDef(use, newDef);
          }
        }
      }
    }

    static std::pair<dxbc_spv::ir::SsaDef, dxbc_spv::ir::ShaderStage> findEntryPoint(dxbc_spv::ir::Builder& builder) {
      using namespace dxbc_spv;

      auto [a, b] = builder.getDeclarations();

      for (auto iter = a; iter != b; iter++) {
        if (iter->getOpCode() == ir::OpCode::eEntryPoint)
          return std::make_pair(iter->getDef(), ir::ShaderStage(iter->getOperand(iter->getFirstLiteralOperandIndex())));
      }

      return {};
    }

  };


  
  D3D11CommonShader:: D3D11CommonShader() { }
  D3D11CommonShader::~D3D11CommonShader() { }
  
  
  D3D11CommonShader::D3D11CommonShader(
          D3D11Device*            pDevice,
          D3D11ClassLinkage*      pLinkage,
    const DxvkShaderHash&         ShaderKey,
    const DxvkIrShaderCreateInfo& ModuleInfo,
    const void*                   pShaderBytecode,
          size_t                  BytecodeLength,
    const D3D11ShaderIcbInfo&     Icb,
    const D3D11BindingMask&       BindingMask)
  : m_bindings(BindingMask) {
    if (Logger::logLevel() <= LogLevel::Debug)
      Logger::debug(str::format("Compiling shader ", ShaderKey.toString()));

    if (pLinkage)
      GatherInterefaceInfo(pLinkage, pShaderBytecode, BytecodeLength);

    CreateIrShader(pDevice, ShaderKey, ModuleInfo, pShaderBytecode, BytecodeLength, Icb);
    pDevice->GetDXVKDevice()->registerShader(m_shader);
  }


  void D3D11CommonShader::CreateIrShader(
          D3D11Device*            pDevice,
    const DxvkShaderHash&         ShaderKey,
    const DxvkIrShaderCreateInfo& ModuleInfo,
    const void*                   pShaderBytecode,
          size_t                  BytecodeLength,
    const D3D11ShaderIcbInfo&     Icb) {
    constexpr size_t MaxEmbeddedIcbSize = 64u;

    // Create icb if lowering is required
    size_t icbSizeInBytes = Icb.size * sizeof(*Icb.data);

    if (ModuleInfo.options.flags.test(DxvkShaderCompileFlag::LowerConstantArrays) && icbSizeInBytes > MaxEmbeddedIcbSize) {
      DxvkBufferCreateInfo info = { };
      info.size   = align(icbSizeInBytes, 256u);
      info.usage  = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                  | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                  | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      info.stages = VK_PIPELINE_STAGE_2_TRANSFER_BIT
                  | util::pipelineStages(ShaderKey.stage());
      info.access = VK_ACCESS_UNIFORM_READ_BIT
                  | VK_ACCESS_TRANSFER_READ_BIT
                  | VK_ACCESS_TRANSFER_WRITE_BIT;
      info.debugName = "Icb";

      m_buffer = pDevice->GetDXVKDevice()->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

      pDevice->InitShaderIcb(this, icbSizeInBytes, Icb.data);
    }

    // Create actual shader converter
    m_shader = pDevice->GetDXVKDevice()->createCachedShader(
      ShaderKey.toString(), ModuleInfo, nullptr);

    if (!m_shader) {
      Rc<D3D11ShaderConverter> converter = new D3D11ShaderConverter(ShaderKey,
        ModuleInfo, pShaderBytecode, BytecodeLength, bool(m_buffer));

      m_shader = pDevice->GetDXVKDevice()->createCachedShader(
        ShaderKey.toString(), ModuleInfo, std::move(converter));
    }
  }


  void D3D11CommonShader::GatherInterefaceInfo(
          D3D11ClassLinkage*      pLinkage,
    const void*                   pShaderBytecode,
          size_t                  BytecodeLength) {
    dxbc_spv::dxbc::Container container(pShaderBytecode, BytecodeLength);
    dxbc_spv::util::ByteReader ifaceChunk(container.getInterfaceChunk());

    if (!ifaceChunk)
      return;

    dxbc_spv::dxbc::InterfaceChunk ifaceInfo(ifaceChunk);

    if (!ifaceInfo)
      return;

    auto typeInfos = ifaceInfo.getClassTypes();
    auto slotInfos = ifaceInfo.getInterfaceSlots();

    for (auto i = typeInfos.first; i != typeInfos.second; i++) {
      m_interfaces.AddType(i->id, i->name.c_str());
      pLinkage->AddType(i->name.c_str(), i->cbSize, i->srvCount, i->samplerCount);
    }

    for (auto i = slotInfos.first; i != slotInfos.second; i++) {
      for (const auto& e : i->entries)
        m_interfaces.AddSlotInfo(i->index, i->count, e.typeId, e.tableId);
    }

    auto instances = ifaceInfo.getClassInstances();

    for (auto i = instances.first; i != instances.second; i++) {
      D3D11_CLASS_INSTANCE_DESC desc = { };
      desc.ConstantBuffer = i->cbvIndex;
      desc.BaseConstantBufferOffset = i->cbvOffset;
      desc.BaseTexture = i->srvIndex & 0x7fu;
      desc.BaseSampler = i->samplerIndex & 0xfu;

      auto typeName = m_interfaces.GetTypeName(i->typeId);

      if (typeName)
        pLinkage->AddInstance(&desc, typeName, i->name.c_str());
    }
  }

  
  D3D11ShaderModuleSet:: D3D11ShaderModuleSet() { }
  D3D11ShaderModuleSet::~D3D11ShaderModuleSet() { }
  
  
  HRESULT D3D11ShaderModuleSet::GetShaderModule(
          D3D11Device*            pDevice,
          D3D11ClassLinkage*      pLinkage,
    const DxvkShaderHash&         ShaderKey,
    const DxvkIrShaderCreateInfo& ModuleInfo,
    const void*                   pShaderBytecode,
          size_t                  BytecodeLength,
    const D3D11ShaderIcbInfo&     Icb,
    const D3D11BindingMask&       BindingMask,
          D3D11CommonShader*      pShader) {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    auto entry = m_modules.find(ShaderKey);
    if (entry != m_modules.end()) {
      *pShader = entry->second;
      return S_OK;
    }

    D3D11CommonShader module;

    try {
      module = D3D11CommonShader(pDevice, pLinkage, ShaderKey,
        ModuleInfo, pShaderBytecode, BytecodeLength, Icb, BindingMask);
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      return E_INVALIDARG;
    }

    m_modules.insert({ ShaderKey, module });
    *pShader = std::move(module);
    return S_OK;
  }

}
