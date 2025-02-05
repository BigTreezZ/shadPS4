// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace AmdGpu {

static const char* dcb_task_name{"DCB_TASK"};
static const char* ccb_task_name{"CCB_TASK"};
static const char* acb_task_name{"ACB_TASK"};

std::array<u8, 48_KB> Liverpool::ConstantEngine::constants_heap;

Liverpool::Liverpool() {
    process_thread = std::jthread{std::bind_front(&Liverpool::Process, this)};
}

Liverpool::~Liverpool() {
    process_thread.request_stop();
    process_thread.join();
}

void Liverpool::Process(std::stop_token stoken) {
    Common::SetCurrentThreadName("GPU_CommandProcessor");

    while (!stoken.stop_requested()) {
        {
            std::unique_lock lk{submit_mutex};
            submit_cv.wait(lk, stoken, [this] { return num_submits != 0; });
        }
        if (stoken.stop_requested()) {
            break;
        }

        int qid = -1;

        while (num_submits) {
            qid = (qid + 1) % NumTotalQueues;

            auto& queue = mapped_queues[qid];

            Task::Handle task{};
            {
                std::scoped_lock lock{queue.m_access};

                if (queue.submits.empty()) {
                    continue;
                }

                task = queue.submits.front();
            }
            task.resume();

            if (task.done()) {
                task.destroy();

                std::scoped_lock lock{queue.m_access};
                queue.submits.pop();

                --num_submits;
            }
        }

        if (submit_done) {
            std::scoped_lock lk{submit_mutex};
            submit_cv.notify_all();
            submit_done = false;
        }
    }
}

void Liverpool::WaitGpuIdle() {
    RENDERER_TRACE;

    std::unique_lock lk{submit_mutex};
    submit_cv.wait(lk, [this] { return num_submits == 0; });
}

Liverpool::Task Liverpool::ProcessCeUpdate(std::span<const u32> ccb) {
    TracyFiberEnter(ccb_task_name);

    while (!ccb.empty()) {
        const auto* header = reinterpret_cast<const PM4Header*>(ccb.data());
        const u32 type = header->type;
        if (type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", type);
        }

        const PM4ItOpcode opcode = header->type3.opcode;
        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::WriteConstRam: {
            const auto* write_const = reinterpret_cast<const PM4WriteConstRam*>(header);
            memcpy(cblock.constants_heap.data() + write_const->Offset(), &write_const->data,
                   write_const->Size());
            break;
        }
        case PM4ItOpcode::DumpConstRam: {
            const auto* dump_const = reinterpret_cast<const PM4DumpConstRam*>(header);
            memcpy(dump_const->Address<void*>(),
                   cblock.constants_heap.data() + dump_const->Offset(), dump_const->Size());
            break;
        }
        case PM4ItOpcode::IncrementCeCounter: {
            ++cblock.ce_count;
            break;
        }
        case PM4ItOpcode::WaitOnDeCounterDiff: {
            const auto diff = it_body[0];
            while ((cblock.de_count - cblock.ce_count) >= diff) {
                TracyFiberLeave;
                co_yield {};
                TracyFiberEnter(ccb_task_name);
            }
            break;
        }
        default:
            const u32 count = header->type3.NumWords();
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), count);
        }
        ccb = ccb.subspan(header->type3.NumWords() + 1);
    }

    TracyFiberLeave;
}

Liverpool::Task Liverpool::ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb) {
    TracyFiberEnter(dcb_task_name);

    cblock.Reset();

    // TODO: potentially, ASCs also can depend on CE and in this case the
    // CE task should be moved into more global scope
    Task ce_task{};

    if (!ccb.empty()) {
        // In case of CCB provided kick off CE asap to have the constant heap ready to use
        ce_task = ProcessCeUpdate(ccb);
        TracyFiberLeave;
        ce_task.handle.resume();
        TracyFiberEnter(dcb_task_name);
    }

    while (!dcb.empty()) {
        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data());
        const u32 type = header->type;
        if (type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", type);
        }

        const u32 count = header->type3.NumWords();
        const PM4ItOpcode opcode = header->type3.opcode;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            if (nop->header.count.Value() == 0) {
                break;
            }

            switch (nop->data_block[0]) {
            case PM4CmdNop::PayloadType::PatchedFlip: {
                // There is no evidence that GPU CP drives flip events by parsing
                // special NOP packets. For convenience lets assume that it does.
                Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
                break;
            }
            default:
                break;
            }
            break;
        }
        case PM4ItOpcode::ContextControl: {
            break;
        }
        case PM4ItOpcode::ClearState: {
            break;
        }
        case PM4ItOpcode::SetConfigReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const auto reg_addr = ConfigRegWordOffset + set_data->reg_offset;
            const auto* payload = reinterpret_cast<const u32*>(header + 2);
            std::memcpy(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32));
            break;
        }
        case PM4ItOpcode::SetContextReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const auto reg_addr = ContextRegWordOffset + set_data->reg_offset;
            const auto* payload = reinterpret_cast<const u32*>(header + 2);

            std::memcpy(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32));

            // In the case of HW, render target memory has alignment as color block operates on
            // tiles. There is no information of actual resource extents stored in CB context
            // regs, so any deduction of it from slices/pitch will lead to a larger surface created.
            // The same applies to the depth targets. Fortunatelly, the guest always sends
            // a trailing NOP packet right after the context regs setup, so we can use the heuristic
            // below and extract the hint to determine actual resource dims.

            switch (reg_addr) {
            case ContextRegs::CbColor0Base:
                [[fallthrough]];
            case ContextRegs::CbColor1Base:
                [[fallthrough]];
            case ContextRegs::CbColor2Base:
                [[fallthrough]];
            case ContextRegs::CbColor3Base:
                [[fallthrough]];
            case ContextRegs::CbColor4Base:
                [[fallthrough]];
            case ContextRegs::CbColor5Base:
                [[fallthrough]];
            case ContextRegs::CbColor6Base:
                [[fallthrough]];
            case ContextRegs::CbColor7Base: {
                const auto col_buf_id = (reg_addr - ContextRegs::CbColor0Base) /
                                        (ContextRegs::CbColor1Base - ContextRegs::CbColor0Base);
                ASSERT(col_buf_id < NumColorBuffers);

                const auto nop_offset = header->type3.count;
                if (nop_offset == 0x0e || nop_offset == 0x0d) {
                    ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                               "NOP hint is missing in CB setup sequence");
                    last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                } else {
                    last_cb_extent[col_buf_id].raw = 0;
                }
                break;
            }
            case ContextRegs::DbZInfo: {
                if (header->type3.count == 8) {
                    ASSERT_MSG(payload[20] == 0xc0001000,
                               "NOP hint is missing in DB setup sequence");
                    last_db_extent.raw = payload[21];
                } else {
                    last_db_extent.raw = 0;
                }
                break;
            }
            default:
                break;
            }
            break;
        }
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            std::memcpy(&regs.reg_array[ShRegWordOffset + set_data->reg_offset], header + 2,
                        (count - 1) * sizeof(u32));
            break;
        }
        case PM4ItOpcode::SetUconfigReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            std::memcpy(&regs.reg_array[UconfigRegWordOffset + set_data->reg_offset], header + 2,
                        (count - 1) * sizeof(u32));
            break;
        }
        case PM4ItOpcode::IndexType: {
            const auto* index_type = reinterpret_cast<const PM4CmdDrawIndexType*>(header);
            regs.index_buffer_type.raw = index_type->raw;
            break;
        }
        case PM4ItOpcode::DrawIndex2: {
            const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndex2*>(header);
            regs.max_index_size = draw_index->max_size;
            regs.index_base_address.base_addr_lo = draw_index->index_base_lo;
            regs.index_base_address.base_addr_hi.Assign(draw_index->index_base_hi);
            regs.num_indices = draw_index->index_count;
            regs.draw_initiator = draw_index->draw_initiator;
            if (rasterizer) {
                rasterizer->Draw(true);
            }
            break;
        }
        case PM4ItOpcode::DrawIndexOffset2: {
            const auto* draw_index_off = reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
            regs.max_index_size = draw_index_off->max_size;
            regs.num_indices = draw_index_off->index_count;
            regs.draw_initiator = draw_index_off->draw_initiator;
            if (rasterizer) {
                rasterizer->Draw(true, draw_index_off->index_offset);
            }
            break;
        }
        case PM4ItOpcode::DrawIndexAuto: {
            const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
            regs.num_indices = draw_index->index_count;
            regs.draw_initiator = draw_index->draw_initiator;
            if (rasterizer) {
                rasterizer->Draw(false);
            }
            break;
        }
        case PM4ItOpcode::DispatchDirect: {
            const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
            regs.cs_program.dim_x = dispatch_direct->dim_x;
            regs.cs_program.dim_y = dispatch_direct->dim_y;
            regs.cs_program.dim_z = dispatch_direct->dim_z;
            regs.cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
            if (rasterizer && (regs.cs_program.dispatch_initiator & 1)) {
                rasterizer->DispatchDirect();
            }
            break;
        }
        case PM4ItOpcode::NumInstances: {
            const auto* num_instances = reinterpret_cast<const PM4CmdDrawNumInstances*>(header);
            regs.num_instances.num_instances = num_instances->num_instances;
            break;
        }
        case PM4ItOpcode::IndexBase: {
            const auto* index_base = reinterpret_cast<const PM4CmdDrawIndexBase*>(header);
            regs.index_base_address.base_addr_lo = index_base->addr_lo;
            regs.index_base_address.base_addr_hi.Assign(index_base->addr_hi);
            break;
        }
        case PM4ItOpcode::IndexBufferSize: {
            const auto* index_size = reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header);
            regs.num_indices = index_size->num_indices;
            break;
        }
        case PM4ItOpcode::EventWrite: {
            // const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
            break;
        }
        case PM4ItOpcode::EventWriteEos: {
            const auto* event_eos = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
            event_eos->SignalFence();
            break;
        }
        case PM4ItOpcode::EventWriteEop: {
            const auto* event_eop = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
            event_eop->SignalFence();
            break;
        }
        case PM4ItOpcode::DmaData: {
            const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
            break;
        }
        case PM4ItOpcode::WriteData: {
            const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
            ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
            const u32 data_size = (header->type3.count.Value() - 2) * 4;
            if (!write_data->wr_one_addr.Value()) {
                std::memcpy(write_data->Address<void*>(), write_data->data, data_size);
            } else {
                UNREACHABLE();
            }
            break;
        }
        case PM4ItOpcode::AcquireMem: {
            // const auto* acquire_mem = reinterpret_cast<PM4CmdAcquireMem*>(header);
            break;
        }
        case PM4ItOpcode::WaitRegMem: {
            const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
            ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
            while (!wait_reg_mem->Test()) {
                TracyFiberLeave;
                co_yield {};
                TracyFiberEnter(dcb_task_name);
            }
            break;
        }
        case PM4ItOpcode::IncrementDeCounter: {
            ++cblock.de_count;
            break;
        }
        case PM4ItOpcode::WaitOnCeCounter: {
            while (cblock.ce_count <= cblock.de_count) {
                TracyFiberLeave;
                ce_task.handle.resume();
                TracyFiberEnter(dcb_task_name);
            }
            break;
        }
        default:
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), count);
        }
        dcb = dcb.subspan(header->type3.NumWords() + 1);
    }

    if (ce_task.handle) {
        ASSERT_MSG(ce_task.handle.done(), "Partially processed CCB");
        ce_task.handle.destroy();
    }

    TracyFiberLeave;
}

Liverpool::Task Liverpool::ProcessCompute(std::span<const u32> acb) {
    TracyFiberEnter(acb_task_name);

    while (!acb.empty()) {
        const auto* header = reinterpret_cast<const PM4Header*>(acb.data());
        const u32 type = header->type;
        if (type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", type);
        }

        const u32 count = header->type3.NumWords();
        const PM4ItOpcode opcode = header->type3.opcode;
        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task =
                ProcessCompute({indirect_buffer->Address<const u32>(), indirect_buffer->ib_size});
            while (!task.handle.done()) {
                task.handle.resume();

                TracyFiberLeave;
                co_yield {};
                TracyFiberEnter(acb_task_name);
            };
            break;
        }
        case PM4ItOpcode::AcquireMem: {
            break;
        }
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            std::memcpy(&regs.reg_array[ShRegWordOffset + set_data->reg_offset], header + 2,
                        (count - 1) * sizeof(u32));
            break;
        }
        case PM4ItOpcode::DispatchDirect: {
            const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
            regs.cs_program.dim_x = dispatch_direct->dim_x;
            regs.cs_program.dim_y = dispatch_direct->dim_y;
            regs.cs_program.dim_z = dispatch_direct->dim_z;
            regs.cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
            if (rasterizer && (regs.cs_program.dispatch_initiator & 1)) {
                rasterizer->DispatchDirect();
            }
            break;
        }
        case PM4ItOpcode::WriteData: {
            const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
            ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
            const u32 data_size = (header->type3.count.Value() - 2) * 4;
            if (!write_data->wr_one_addr.Value()) {
                std::memcpy(write_data->Address<void*>(), write_data->data, data_size);
            } else {
                UNREACHABLE();
            }
            break;
        }
        case PM4ItOpcode::WaitRegMem: {
            const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
            ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
            while (!wait_reg_mem->Test()) {
                TracyFiberLeave;
                co_yield {};
                TracyFiberEnter(acb_task_name);
            }
            break;
        }
        case PM4ItOpcode::ReleaseMem: {
            const auto* release_mem = reinterpret_cast<const PM4CmdReleaseMem*>(header);
            release_mem->SignalFence(Platform::InterruptId::Compute0RelMem); // <---
            break;
        }
        default:
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), count);
        }

        acb = acb.subspan(header->type3.NumWords() + 1);
    }

    TracyFiberLeave;
}

void Liverpool::SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb) {
    static constexpr u32 GfxQueueId = 0u;
    auto& queue = mapped_queues[GfxQueueId];

    auto task = ProcessGraphics(dcb, ccb);
    {
        std::unique_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    ++num_submits;
    submit_cv.notify_one();
}

void Liverpool::SubmitAsc(u32 vqid, std::span<const u32> acb) {
    ASSERT_MSG(vqid >= 0 && vqid < NumTotalQueues, "Invalid virtual ASC queue index");
    auto& queue = mapped_queues[vqid];

    const auto& task = ProcessCompute(acb);
    {
        std::unique_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    ++num_submits;
    submit_cv.notify_one();
}

} // namespace AmdGpu
