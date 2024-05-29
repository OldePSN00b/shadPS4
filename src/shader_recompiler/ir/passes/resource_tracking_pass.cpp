// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <optional>
#include <boost/container/small_vector.hpp>
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/resource.h"

namespace Shader::Optimization {
namespace {

struct SharpLocation {
    u32 sgpr_base;
    u32 dword_offset;

    auto operator<=>(const SharpLocation&) const = default;
};

bool IsBufferInstruction(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadBufferF32:
    case IR::Opcode::LoadBufferF32x2:
    case IR::Opcode::LoadBufferF32x3:
    case IR::Opcode::LoadBufferF32x4:
    case IR::Opcode::ReadConstBuffer:
        return true;
    default:
        return false;
    }
}

IR::Type BufferLoadType(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadBufferF32:
    case IR::Opcode::LoadBufferF32x2:
    case IR::Opcode::LoadBufferF32x3:
    case IR::Opcode::LoadBufferF32x4:
    case IR::Opcode::ReadConstBuffer:
        return IR::Type::F32;
    default:
        UNREACHABLE();
    }
}

bool IsImageInstruction(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::ImageSampleExplicitLod:
    case IR::Opcode::ImageSampleImplicitLod:
    case IR::Opcode::ImageSampleDrefExplicitLod:
    case IR::Opcode::ImageSampleDrefImplicitLod:
    case IR::Opcode::ImageFetch:
    case IR::Opcode::ImageGather:
    case IR::Opcode::ImageGatherDref:
    case IR::Opcode::ImageQueryDimensions:
    case IR::Opcode::ImageQueryLod:
    case IR::Opcode::ImageGradient:
    case IR::Opcode::ImageRead:
    case IR::Opcode::ImageWrite:
        return true;
    default:
        return false;
    }
}

class Descriptors {
public:
    explicit Descriptors(BufferResourceList& buffer_resources_, ImageResourceList& image_resources_,
                         SamplerResourceList& sampler_resources_)
        : buffer_resources{buffer_resources_}, image_resources{image_resources_},
          sampler_resources{sampler_resources_} {}

    u32 Add(const BufferResource& desc) {
        const u32 index{Add(buffer_resources, desc, [&desc](const auto& existing) {
            return desc.sgpr_base == existing.sgpr_base &&
                   desc.dword_offset == existing.dword_offset;
        })};
        auto& buffer = buffer_resources[index];
        ASSERT(buffer.stride == desc.stride && buffer.num_records == desc.num_records);
        buffer.is_storage |= desc.is_storage;
        buffer.used_types |= desc.used_types;
        return index;
    }

    u32 Add(const ImageResource& desc) {
        const u32 index{Add(image_resources, desc, [&desc](const auto& existing) {
            return desc.sgpr_base == existing.sgpr_base &&
                   desc.dword_offset == existing.dword_offset && desc.type == existing.type &&
                   desc.is_storage == existing.is_storage;
        })};
        return index;
    }

    u32 Add(const SamplerResource& desc) {
        const u32 index{Add(sampler_resources, desc, [&desc](const auto& existing) {
            return desc.sgpr_base == existing.sgpr_base &&
                   desc.dword_offset == existing.dword_offset;
        })};
        return index;
    }

private:
    template <typename Descriptors, typename Descriptor, typename Func>
    static u32 Add(Descriptors& descriptors, const Descriptor& desc, Func&& pred) {
        const auto it{std::ranges::find_if(descriptors, pred)};
        if (it != descriptors.end()) {
            return static_cast<u32>(std::distance(descriptors.begin(), it));
        }
        descriptors.push_back(desc);
        return static_cast<u32>(descriptors.size()) - 1;
    }

    BufferResourceList& buffer_resources;
    ImageResourceList& image_resources;
    SamplerResourceList& sampler_resources;
};

} // Anonymous namespace

SharpLocation TrackSharp(const IR::Inst* inst) {
    if (inst->GetOpcode() == IR::Opcode::GetUserData) {
        return SharpLocation{
            .sgpr_base = u32(IR::ScalarReg::Max),
            .dword_offset = u32(inst->Arg(0).ScalarReg()),
        };
    }
    ASSERT_MSG(inst->GetOpcode() == IR::Opcode::ReadConst, "Sharp load not from constant memory");

    // Retrieve offset from base.
    IR::Inst* addr = inst->Arg(0).InstRecursive();
    u32 dword_offset = addr->Arg(1).U32();
    addr = addr->Arg(0).InstRecursive();
    ASSERT_MSG(addr->Arg(1).IsImmediate(), "Bindless not supported");
    dword_offset += addr->Arg(1).U32() >> 2;

    // Retrieve SGPR that holds sbase
    inst = addr->Arg(0).InstRecursive()->Arg(0).InstRecursive();
    ASSERT_MSG(inst->GetOpcode() == IR::Opcode::GetUserData, "Nested resource loads not supported");
    const IR::ScalarReg base = inst->Arg(0).ScalarReg();

    // Return retrieved location.
    return SharpLocation{
        .sgpr_base = u32(base),
        .dword_offset = dword_offset,
    };
}

void PatchBufferInstruction(IR::Block& block, IR::Inst& inst, Info& info,
                            Descriptors& descriptors) {
    IR::Inst* producer = inst.Arg(0).InstRecursive();
    const auto sharp = TrackSharp(producer);
    const auto buffer = info.ReadUd<AmdGpu::Buffer>(sharp.sgpr_base, sharp.dword_offset);
    const u32 binding = descriptors.Add(BufferResource{
        .sgpr_base = sharp.sgpr_base,
        .dword_offset = sharp.dword_offset,
        .stride = u32(buffer.stride),
        .num_records = u32(buffer.num_records),
        .used_types = BufferLoadType(inst),
        .is_storage = /*buffer.base_address % 64 != 0*/ true,
    });
    const auto inst_info = inst.Flags<IR::BufferInstInfo>();
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    // Replace handle with binding index in buffer resource list.
    inst.SetArg(0, ir.Imm32(binding));
    ASSERT(!buffer.swizzle_enable && !buffer.add_tid_enable);
    if (inst_info.is_typed) {
        ASSERT(inst_info.nfmt == AmdGpu::NumberFormat::Float &&
               inst_info.dmft == AmdGpu::DataFormat::Format32_32_32_32);
    }
    if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer) {
        return;
    }
    // Calculate buffer address.
    const u32 dword_stride = buffer.stride / sizeof(u32);
    const u32 dword_offset = inst_info.inst_offset.Value() / sizeof(u32);
    IR::U32 address = ir.Imm32(dword_offset);
    if (inst_info.index_enable && inst_info.offset_enable) {
        UNREACHABLE();
    } else if (inst_info.index_enable) {
        const IR::U32 index{inst.Arg(1)};
        address = ir.IAdd(ir.IMul(index, ir.Imm32(dword_stride)), address);
    } else if (inst_info.offset_enable) {
        const IR::U32 offset{inst.Arg(1)};
    }
    inst.SetArg(1, address);
}

void PatchImageInstruction(IR::Block& block, IR::Inst& inst, Info& info, Descriptors& descriptors) {
    IR::Inst* producer = inst.Arg(0).InstRecursive();
    ASSERT(producer->GetOpcode() == IR::Opcode::CompositeConstructU32x2);

    // Read image sharp.
    const auto tsharp = TrackSharp(producer->Arg(0).InstRecursive());
    const auto image = info.ReadUd<AmdGpu::Image>(tsharp.sgpr_base, tsharp.dword_offset);
    const auto inst_info = inst.Flags<IR::TextureInstInfo>();
    const u32 image_binding = descriptors.Add(ImageResource{
        .sgpr_base = tsharp.sgpr_base,
        .dword_offset = tsharp.dword_offset,
        .type = image.type,
        .nfmt = static_cast<AmdGpu::NumberFormat>(image.num_format.Value()),
        .is_storage = false,
        .is_depth = bool(inst_info.is_depth),
    });

    // Read sampler sharp.
    const auto ssharp = TrackSharp(producer->Arg(1).InstRecursive());
    const u32 sampler_binding = descriptors.Add(SamplerResource{
        .sgpr_base = ssharp.sgpr_base,
        .dword_offset = ssharp.dword_offset,
    });

    // Patch image handle
    const u32 handle = image_binding | (sampler_binding << 16);
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    inst.SetArg(0, ir.Imm32(handle));

    // Now that we know the image type, adjust texture coordinate vector.
    const IR::Inst* body = inst.Arg(1).InstRecursive();
    const auto [coords, arg] = [&] -> std::pair<IR::Value, IR::Value> {
        switch (image.type) {
        case AmdGpu::ImageType::Color1D:
            return {body->Arg(0), body->Arg(1)};
        case AmdGpu::ImageType::Color1DArray:
        case AmdGpu::ImageType::Color2D:
            return {ir.CompositeConstruct(body->Arg(0), body->Arg(1)), body->Arg(2)};
        case AmdGpu::ImageType::Color2DArray:
        case AmdGpu::ImageType::Color3D:
        case AmdGpu::ImageType::Cube:
            return {ir.CompositeConstruct(body->Arg(0), body->Arg(1), body->Arg(2)), body->Arg(3)};
        default:
            UNREACHABLE();
        }
    }();
    inst.SetArg(1, coords);

    if (inst_info.has_lod_clamp) {
        // Final argument contains lod_clamp
        const u32 arg_pos = inst_info.is_depth ? 5 : 4;
        inst.SetArg(arg_pos, arg);
    }
}

void ResourceTrackingPass(IR::Program& program) {
    auto& info = program.info;
    Descriptors descriptors{info.buffers, info.images, info.samplers};
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (IsBufferInstruction(inst)) {
                PatchBufferInstruction(*block, inst, info, descriptors);
                continue;
            }
            if (IsImageInstruction(inst)) {
                PatchImageInstruction(*block, inst, info, descriptors);
            }
        }
    }
}

} // namespace Shader::Optimization