#include "infra/scal/gen2_arch_common/scal_names.h"
#include "platform/gaudi2/hcl_packets.h"

#include <ext/alloc_traits.h>                         // for __alloc_traits<...
#include <cstring>                                    // for memcpy, size_t
#include <algorithm>                                  // for max
#include <cstdint>                                    // for uint32_t, uint16_t
#include <memory>                                     // for __shared_ptr_ac...
#include <utility>                                    // for pair, move

#include "hcl_utils.h"                                // for VERIFY
#include "infra/scal/gen2_arch_common/scal_names.h"   // for SchedulersIndex
#include "infra/scal/gen2_arch_common/scal_stream.h"  // for ScalStreamBase
#include "hcl_log_manager.h"                          // for LOG_*
#include "platform/gaudi2/nic_passthrough_handler.h"  // for RecordWithMetadata
#include "sched_pkts.h"                               // for g2fw
#include "platform/gen2_arch_common/types.h"          // for REDUCTION_OP_AD...
#include "scal.h"                                     // for SCAL_NIC_RECEIV...
#include "hccl_types.h"                               // for hcclRedOp_t
#include "define_synapse_common.hpp"                  // for pdma context id
#include "synapse_profiler_api.hpp"                   // for pdma context id
#include "platform/gaudi2/nic_passthrough_handler.h"  // for pRecordWithMetadata
#include "platform/gen2_arch_common/hcl_packets_utils.h"
#include "hcl_math_utils.h"

void SchedArcCommandsGaudi2::serializeNopCommand(hcl::ScalStreamBase& scalStream, unsigned schedIdx, uint32_t padding)
{
    g2fw::sched_arc_cmd_nop_t* command =
        reinterpret_cast<g2fw::sched_arc_cmd_nop_t*>(scalStream.getNextPtr(sizeof(g2fw::sched_arc_cmd_nop_t)));
    memset(command, 0, sizeof(g2fw::sched_arc_cmd_nop_t));

    static const unsigned opcodes[(unsigned)hcl::SchedulersIndex::count] = {g2fw::SCHED_GC_REDUCTION_ARC_CMD_NOP,
                                                                            g2fw::SCHED_SCALEUP_SEND_ARC_CMD_NOP,
                                                                            g2fw::SCHED_SCALEUP_RECV_ARC_CMD_NOP,
                                                                            g2fw::SCHED_SCALEOUT_SEND_ARC_CMD_NOP,
                                                                            g2fw::SCHED_SCALEOUT_RECV_ARC_CMD_NOP};
    command->opcode                                                      = opcodes[schedIdx];
    command->padding_count = (uint32_t)((padding - sizeof(g2fw::sched_arc_cmd_nop_t)) / sizeof(uint32_t));
}

void SchedArcCommandsGaudi2::serializeAllocBarrierCommand(hcl::ScalStreamBase& scalStream,
                                                          unsigned             schedIdx,
                                                          uint32_t             completionGroupIndex,
                                                          uint32_t             requiredSobs)
{
    g2fw::sched_arc_cmd_alloc_nic_barrier_t* command = reinterpret_cast<g2fw::sched_arc_cmd_alloc_nic_barrier_t*>(
        scalStream.getNextPtr(sizeof(g2fw::sched_arc_cmd_alloc_nic_barrier_t)));
    memset(command, 0, sizeof(g2fw::sched_arc_cmd_alloc_nic_barrier_t));

    static const unsigned opcodes[(unsigned)hcl::SchedulersIndex::count] = {
        g2fw::SCHED_GC_REDUCTION_ARC_CMD_ALLOC_NIC_BARRIER,
        g2fw::SCHED_SCALEUP_SEND_ARC_CMD_ALLOC_NIC_BARRIER,
        g2fw::SCHED_SCALEUP_RECV_ARC_CMD_ALLOC_NIC_BARRIER,
        g2fw::SCHED_SCALEOUT_SEND_ARC_CMD_ALLOC_NIC_BARRIER,
        g2fw::SCHED_SCALEOUT_RECV_ARC_CMD_ALLOC_NIC_BARRIER};
    command->opcode           = opcodes[schedIdx];
    command->comp_group_index = completionGroupIndex;
    command->required_sobs    = requiredSobs;

    LOG_TRACE(
        HCL_SUBMIT,
        "Packets | serializeAllocBarrierCommand sched:{}, opcode:{}, comp_group_index:{}, required_sobs:{}, "
        "on stream:{}",
        schedIdx,
        command->opcode,
        (uint32_t)command->comp_group_index,
        (uint32_t)command->required_sobs,
        *(scalStream.getStreamName()));
}

void SchedArcCommandsGaudi2::serializeFenceCommand(hcl::ScalStreamBase& scalStream,
                                                   unsigned             schedIdx,
                                                   uint32_t             fenceIndex,
                                                   uint32_t             target)
{
    g2fw::sched_arc_cmd_fence_wait_t* command = reinterpret_cast<g2fw::sched_arc_cmd_fence_wait_t*>(
        scalStream.getNextPtr(sizeof(g2fw::sched_arc_cmd_fence_wait_t)));
    memset(command, 0, sizeof(g2fw::sched_arc_cmd_fence_wait_t));

    static const unsigned opcodes[(unsigned)hcl::SchedulersIndex::count] = {
        g2fw::SCHED_GC_REDUCTION_ARC_CMD_FENCE_WAIT,
        g2fw::SCHED_SCALEUP_SEND_ARC_CMD_FENCE_WAIT,
        g2fw::SCHED_SCALEUP_RECV_ARC_CMD_FENCE_WAIT,
        g2fw::SCHED_SCALEOUT_SEND_ARC_CMD_FENCE_WAIT,
        g2fw::SCHED_SCALEOUT_RECV_ARC_CMD_FENCE_WAIT};
    command->opcode   = opcodes[schedIdx];
    command->fence_id = fenceIndex;
    command->target   = target;

    LOG_TRACE(
        HCL_SUBMIT,
        "Packets | serializeFenceCommand sched:{}, opcode:{}, target:{}, fence_id:{} on stream:{}",
        schedIdx,
        command->opcode,
        (uint32_t)command->target,
        (uint32_t)command->fence_id,
        *(scalStream.getStreamName()));
}

void SchedArcCommandsGaudi2::serializeFenceIncCommand(hcl::ScalStreamBase& scalStream,
                                                      unsigned             schedIdx,
                                                      uint32_t             fenceIndex,
                                                      uint32_t             target)
{
    g2fw::sched_arc_cmd_fence_inc_immediate_t* command = reinterpret_cast<g2fw::sched_arc_cmd_fence_inc_immediate_t*>(
        scalStream.getNextPtr(sizeof(g2fw::sched_arc_cmd_fence_inc_immediate_t)));
    memset(command, 0, sizeof(g2fw::sched_arc_cmd_fence_inc_immediate_t));

    static const unsigned opcodes[(unsigned)hcl::SchedulersIndex::count] = {
        g2fw::SCHED_GC_REDUCTION_ARC_CMD_FENCE_INC_IMMEDIATE,
        g2fw::SCHED_SCALEUP_SEND_ARC_CMD_FENCE_INC_IMMEDIATE,
        g2fw::SCHED_SCALEUP_RECV_ARC_CMD_FENCE_INC_IMMEDIATE,
        g2fw::SCHED_SCALEOUT_SEND_ARC_CMD_FENCE_INC_IMMEDIATE,
        g2fw::SCHED_SCALEOUT_RECV_ARC_CMD_FENCE_INC_IMMEDIATE};
    command->opcode      = opcodes[schedIdx];
    command->fence_index = fenceIndex;
    LOG_TRACE(
        HCL_SUBMIT,
        "Packets | serializeFenceIncCommand sched:{}, opcode:{} ,fence_id:{} on stream:{}",
        schedIdx,
        command->opcode,
        (uint32_t)command->fence_index,
        *(scalStream.getStreamName()));
}

void SchedArcCommandsGaudi2::serializeLbwWriteCommand(hcl::ScalStreamBase& scalStream,
                                                      unsigned             schedIdx,
                                                      uint32_t             destination,
                                                      uint32_t             data,
                                                      bool                 blockUntilCompletion)
{
    g2fw::sched_arc_cmd_lbw_write_t* command = reinterpret_cast<g2fw::sched_arc_cmd_lbw_write_t*>(
        scalStream.getNextPtr(sizeof(g2fw::sched_arc_cmd_lbw_write_t)));
    memset(command, 0, sizeof(g2fw::sched_arc_cmd_lbw_write_t));

    static const unsigned opcodes[(unsigned)hcl::SchedulersIndex::count] = {
        g2fw::SCHED_GC_REDUCTION_ARC_CMD_LBW_WRITE,
        g2fw::SCHED_SCALEUP_SEND_ARC_CMD_LBW_WRITE,
        g2fw::SCHED_SCALEUP_RECV_ARC_CMD_LBW_WRITE,
        g2fw::SCHED_SCALEOUT_SEND_ARC_CMD_LBW_WRITE,
        g2fw::SCHED_SCALEOUT_RECV_ARC_CMD_LBW_WRITE};
    command->opcode     = opcodes[schedIdx];
    command->block_next = blockUntilCompletion;
    command->dst_addr   = destination;
    command->src_data   = data;

    LOG_TRACE(
        HCL_SUBMIT,
        "Packets | serializeLbwWriteCommand schedIdx:{}, opcode:{} , block_next:{}, dst_addr:0x{:x}, "
        "src_data:0x{:x}, wait_for_completion:{} on stream:{}",
        schedIdx,
        command->opcode,
        (uint32_t)command->block_next,
        (uint64_t)command->dst_addr,
        (uint64_t)command->src_data,
        (uint32_t)command->wait_for_completion,
        *(scalStream.getStreamName()));
}

void SchedArcCommandsGaudi2::serializeDmaCommandV2(hcl::ScalStreamBase& scalStream,
                                                   unsigned             schedIdx,
                                                   uint32_t             dmaType,
                                                   uint32_t             soAddressLSB,
                                                   uint32_t             soAddressLSB2,
                                                   uint32_t             size,
                                                   uint64_t             destAddress,
                                                   uint64_t             srcAddress,
                                                   hcclRedOp_t          reduceOp,
                                                   bool                 isReduction,
                                                   bool                 reductionSignalToCg,
                                                   hcclDataType_t       dataType,
                                                   uint32_t             poolId,
                                                   bool                 isReproReduction,
                                                   bool                 useSibo,
                                                   uint32_t             numberOfRanks,
                                                   uint32_t             numberOfReproBuffers,
                                                   uint32_t             indexOfReproBuffer,
                                                   bool                 is16BitMemcpy,
                                                   bool                 isGDRMemcpy)
{
    static bool shuffleIndex = false;
    size_t      sizeInBytes  = sizeof(g2fw::sched_arc_cmd_nic_edma_ops_t);

    if (dmaType == g2fw::NIC_EDMA_CMD_CAST_DOWN_CLEAR)
    {
        sizeInBytes += sizeof(g2fw::arc_cmd_nic_edma_ops_cdc_t);
    }
    else
    {
        sizeInBytes += sizeof(g2fw::arc_cmd_nic_edma_ops_v3_t);
    }

    g2fw::sched_arc_cmd_nic_edma_ops_t* command =
        reinterpret_cast<g2fw::sched_arc_cmd_nic_edma_ops_t*>(scalStream.getNextPtr(sizeInBytes));
    memset(command, 0, sizeInBytes);

    static const unsigned opcodes[(unsigned)hcl::SchedulersIndex::count] = {
        g2fw::SCHED_GC_REDUCTION_ARC_CMD_NIC_EDMA_OPS,
        g2fw::SCHED_SCALEUP_SEND_ARC_CMD_NIC_EDMA_OPS,
    };

    static unsigned groupEngineInOrder[(unsigned)hcl::SchedulersIndex::count][g2fw::NIC_EDMA_COUNT] = {
        {
            0,
            0,
            0,
            0,
            ScalNetworkGarbageCollectorAndReductionGroups::SCAL_EDMA_NETWORK_GC_REDUCTION_GROUP0, /* memcppy */ // 4
            ScalNetworkGarbageCollectorAndReductionGroups::
                SCAL_EDMA_NETWORK_GC_REDUCTION_GROUP1, /* cast_down_and_memset
                                                        */
            ScalNetworkGarbageCollectorAndReductionGroups::SCAL_EDMA_NETWORK_GC_REDUCTION_GROUP0, /*cast_up_batch*/
            0,
            0,
            ScalNetworkGarbageCollectorAndReductionGroups::SCAL_EDMA_NETWORK_GC_REDUCTION_GROUP0, /* memcopy V3*/
            ScalNetworkGarbageCollectorAndReductionGroups::SCAL_EDMA_NETWORK_GC_REDUCTION_GROUP0 /*cast_up_batchV3*/
        },                                                                                        // dma scheduler
        {
            0,
            0,
            ScalNetworkScaleUpSendGroups::SCAL_EDMA_NETWORK_SCALE_UP_SEND_GROUP0, /* cast up */
            ScalNetworkScaleUpSendGroups::SCAL_EDMA_NETWORK_SCALE_UP_SEND_GROUP0, /* memcppy */
            ScalNetworkScaleUpSendGroups::SCAL_EDMA_NETWORK_SCALE_UP_SEND_GROUP0, /* NIC_EDMA_CMD_MEMCPY_V2 */
            0,
            ScalNetworkScaleUpSendGroups::SCAL_EDMA_NETWORK_SCALE_UP_SEND_GROUP0, /* cast-up */
            0,
            0,
            ScalNetworkScaleUpSendGroups::SCAL_EDMA_NETWORK_SCALE_UP_SEND_GROUP0, /* NIC_EDMA_CMD_MEMCPY_V3 */
            ScalNetworkScaleUpSendGroups::SCAL_EDMA_NETWORK_SCALE_UP_SEND_GROUP0, /* cast-upV3 */
        },                                                                        // scaleup send scheduler
        {
            0,
            0,
            ScalNetworkScaleUpReceiveGroups::SCAL_EDMA_NETWORK_SCALE_UP_RECV_GROUP0, /* cast up */
            ScalNetworkScaleUpReceiveGroups::SCAL_EDMA_NETWORK_SCALE_UP_RECV_GROUP0, /* memcppy */
            ScalNetworkScaleUpReceiveGroups::SCAL_EDMA_NETWORK_SCALE_UP_RECV_GROUP1  /* cast_down_and_memset */
        },                                                                           // scaleup recv scheduler
        {0, 0, 0, 0},
        {0, 0, 0, 0}};

    auto groupEngine = std::ref(groupEngineInOrder);

    command->opcode = opcodes[schedIdx];
    if (dmaType == g2fw::NIC_EDMA_CMD_CAST_DOWN_CLEAR)
    {
        command->cmd_size = sizeof(g2fw::arc_cmd_nic_edma_ops_cdc_t) + sizeof(uint32_t);
    }
    else
    {
        command->cmd_size = sizeof(g2fw::arc_cmd_nic_edma_ops_v3_t) + sizeof(uint32_t);
    }

    command->engine_group_type = groupEngine[schedIdx][dmaType];

    VERIFY(command->engine_group_type != 0, "unsupported dmaType [{}] for {}", dmaType, __func__);

    if (dmaType == static_cast<uint32_t>(g2fw::NIC_EDMA_CMD_CAST_DOWN_CLEAR))
    {
        struct g2fw::arc_cmd_nic_edma_ops_cdc_t* edma_ops =
            (struct g2fw::arc_cmd_nic_edma_ops_cdc_t*)&command->edma_cdc;

        shuffleIndex = !shuffleIndex;

        edma_ops->reduction_op          = isReproReduction ? REDUCTION_OP_ADDITION : getReductionOp(reduceOp);
        edma_ops->shuffle_index         = shuffleIndex;
        edma_ops->use_sibo_index_as_src = useSibo ? 0b1 : 0;
        edma_ops->sibo_index            = indexOfReproBuffer;
        edma_ops->rank_offset_in_sibo   = 0;
        edma_ops->rank_count            = useSibo ? numberOfRanks : 0;

        edma_ops->sob_address   = soAddressLSB & 0x7ffffff;
        edma_ops->sob_address2  = soAddressLSB2 & 0x7ffffff;
        edma_ops->opcode        = g2fw::NIC_EDMA_CMD_CAST_DOWN_CLEAR;
        edma_ops->fp16          = dataType == hcclFloat16;
        edma_ops->transfer_size = size;
        edma_ops->pool_id       = poolId;
        edma_ops->dst_addr_lo   = destAddress & 0xffffffff;
        edma_ops->dst_addr_hi   = destAddress >> 32;
        edma_ops->src_addr_lo   = srcAddress & 0xffffffff;
        edma_ops->src_addr_hi   = (srcAddress >> 32) & 0xffffff;
        edma_ops->reduction_ind = 0;

        edma_ops->reduction_dtype = REDUCTION_FP32;

        if (edma_ops->reduction_op == hcclMax)
        {
            edma_ops->memset_op = eMaxMemsetOp;
        }
        else if (edma_ops->reduction_op == hcclMin)
        {
            edma_ops->memset_op = eMinMemsetOp;
        }
        else
        {
            edma_ops->memset_op = eSumMemsetOp;
        }

        LOG_TRACE(
            HCL_SUBMIT,
            "Packets | Serializing sched_arc_cmd_nic_edma_ops_t command with arc_cmd_nic_edma_ops_cdc_t. "
            "sched: {}, Command[0-3]: 0x{:x}, 0x{:x}, 0x{:x}, sched: command address: 0x{:x}, sched_opcode: {}, "
            "cmd_size:{} "
            "engine_group_type:{}, "
            "engine: shuffle_index:{}, opcode:{}, use_sibo_index_as_src:{}, sibo_index:{}, rank_offset_in_sibo:{}, "
            "rank_count:{}, sob_address:0x{:x}, fp16:{}, transfer_size:{}, pool_id:{}, "
            "srcAddr:0x{:x}, dstAddr:0x{:x}, dst_addr_lo:0x{:x}, dst_addr_hi:0x{:x}, src_addr_lo:0x{:x}, "
            "src_addr_hi:0x{:x}, "
            "reduction_ind:{}, reduction_dtype:{}, reduction_op:{} on stream:{}",
            schedIdx,
            *((uint32_t*)(command)),
            *((uint32_t*)(command) + 1),
            *((uint32_t*)(command) + 2),
            (uint64_t)command,
            command->opcode,
            command->cmd_size,
            command->engine_group_type,
            (uint32_t)edma_ops->shuffle_index,
            (uint32_t)edma_ops->opcode,
            (uint32_t)edma_ops->use_sibo_index_as_src,
            (uint32_t)edma_ops->sibo_index,
            (uint32_t)edma_ops->rank_offset_in_sibo,
            (uint32_t)edma_ops->rank_count,
            (uint64_t)edma_ops->sob_address,
            (uint32_t)edma_ops->fp16,
            (uint32_t)edma_ops->transfer_size,
            (uint32_t)edma_ops->pool_id,
            (uint64_t)srcAddress,
            (uint64_t)destAddress,
            (uint64_t)edma_ops->dst_addr_lo,
            (uint64_t)edma_ops->dst_addr_hi,
            (uint64_t)edma_ops->src_addr_lo,
            (uint64_t)edma_ops->src_addr_hi,
            (uint32_t)edma_ops->reduction_ind,
            (uint32_t)edma_ops->reduction_dtype,
            (uint32_t)edma_ops->reduction_op,
            *(scalStream.getStreamName()));
    }
    else
    {
        struct g2fw::arc_cmd_nic_edma_ops_v3_t* edma_ops =
            (struct g2fw::arc_cmd_nic_edma_ops_v3_t*)&command->edma_ops_v3;
        bool isCastUp = (dmaType == static_cast<uint32_t>(g2fw::NIC_EDMA_CMD_CAST_UP_BATCH_V3));

        shuffleIndex = !shuffleIndex;

        edma_ops->reduction_op =
            (isCastUp && isReproReduction && !isGDRMemcpy) ? REDUCTION_OP_ADDITION : getReductionOp(reduceOp);
        edma_ops->shuffle_index         = shuffleIndex;
        edma_ops->use_sibo_index_as_src = useSibo ? 0b1 : 0;
        edma_ops->sibo_index              = indexOfReproBuffer * numberOfReproBuffers;

        if (useSibo)
        {
            edma_ops->rank_count          = numberOfRanks - 1;
            edma_ops->rank_offset_in_sibo = 1;  // Always 1, as there's another memcpy to copy index 0
        }
        else
        {
            edma_ops->rank_count          = 0;
            edma_ops->rank_offset_in_sibo = 0;
        }

        edma_ops->sob_address     = soAddressLSB & 0x7ffffff;
        edma_ops->opcode          = dmaType;
        edma_ops->fp16            = dataType == hcclFloat16;
        edma_ops->transfer_size   = size;
        edma_ops->pool_id         = poolId;
        edma_ops->dst_addr_lo     = destAddress & 0xffffffff;
        edma_ops->dst_addr_hi     = destAddress >> 32;
        edma_ops->src_addr_lo     = srcAddress & 0xffffffff;
        edma_ops->src_addr_hi     = (srcAddress >> 32) & 0xffffff;
        edma_ops->reduction_ind   = ((useSibo && isReduction) || isCastUp || isGDRMemcpy ||
                                   ((g2fw::edma_eng_arc_cmd_t)dmaType == g2fw::NIC_EDMA_CMD_MEMCPY_V3 && isReduction &&
                                    !reductionSignalToCg && !isReproReduction))
                                        ? 1
                                        : 0;

        edma_ops->reduction_dtype = getReductionDataType(isCastUp, dataType);
        /*
            1. FP32 (no cast-up) -> opcode=NIC_EDMA_CMD_MEMCPY_V3, use_sibo_index_as_src=1, reduction_dtype =
           REDUCTION_FP32, reduction_ind=1
            2. BF16 (cast-up) -> opcode=NIC_EDMA_CMD_CAST_UP_BATCH_V3, use_sibo_index_as_src=1,
           reduction_dtype=REDUCTION_UPSCALING_BF16
            3. FP16 (cast-up) -> opcode=NIC_EDMA_CMD_CAST_UP_BATCH_V3, use_sibo_index_as_src=1,
           reduction_dtype=REDUCTION_UPSCALING_FP16

        */
        LOG_TRACE(
            HCL_SUBMIT,
            "Packets | Serializing sched_arc_cmd_nic_edma_ops_t command with arc_cmd_nic_edma_ops_v3_t. "
            "sched: {}, Command[0-3]: 0x{:x}, 0x{:x}, 0x{:x}, sched: command address: 0x{:x}, sched_opcode: {}, "
            "cmd_size:{} "
            "engine_group_type:{}, "
            "engine: shuffle_index:{}, opcode:{}, use_sibo_index_as_src:{}, sibo_index:{}, rank_offset_in_sibo:{}, "
            "rank_count:{}, sob_address:0x{:x}, fp16:{}, transfer_size:{}, pool_id:{}, "
            "srcAddr:0x{:x}, dstAddr:0x{:x}, dst_addr_lo:0x{:x}, dst_addr_hi:0x{:x}, src_addr_lo:0x{:x}, "
            "src_addr_hi:0x{:x}, "
            "reduction_ind:{}, reduction_dtype:{}, reduction_op:{} on stream:{}",
            schedIdx,
            *((uint32_t*)(command)),
            *((uint32_t*)(command) + 1),
            *((uint32_t*)(command) + 2),
            (uint64_t)command,
            command->opcode,
            command->cmd_size,
            command->engine_group_type,
            (uint32_t)edma_ops->shuffle_index,
            (uint32_t)edma_ops->opcode,
            (uint32_t)edma_ops->use_sibo_index_as_src,
            (uint32_t)edma_ops->sibo_index,
            (uint32_t)edma_ops->rank_offset_in_sibo,
            (uint32_t)edma_ops->rank_count,
            (uint64_t)edma_ops->sob_address,
            (uint32_t)edma_ops->fp16,
            (uint32_t)edma_ops->transfer_size,
            (uint32_t)edma_ops->pool_id,
            (uint64_t)srcAddress,
            (uint64_t)destAddress,
            (uint64_t)edma_ops->dst_addr_lo,
            (uint64_t)edma_ops->dst_addr_hi,
            (uint64_t)edma_ops->src_addr_lo,
            (uint64_t)edma_ops->src_addr_hi,
            (uint32_t)edma_ops->reduction_ind,
            (uint32_t)edma_ops->reduction_dtype,
            (uint32_t)edma_ops->reduction_op,
             *(scalStream.getStreamName()));
    }
}

void SchedArcCommandsGaudi2::serializeDmaCommandV3(hcl::ScalStreamBase& scalStream,
                                                   unsigned             schedIdx,
                                                   uint32_t             dmaType,
                                                   uint32_t             soAddressLSB,
                                                   uint32_t             size,
                                                   uint64_t             destAddress,
                                                   uint64_t             srcAddress,
                                                   hcclRedOp_t          reduceOp,
                                                   uint8_t              streamCtxtID,
                                                   hcclDataType_t       dataType,
                                                   uint32_t             poolId,
                                                   bool                 isForScaleout,
                                                   bool                 useCasting,
                                                   uint32_t             numberOfRanks,
                                                   uint32_t             numberOfReproBuffers,
                                                   uint32_t             indexOfReproBuffer,
                                                   bool                 is16BitMemcpy,
                                                   uint32_t             secondSoAddress,
                                                   bool                 isBFloat,
						   bool                 useReductionInd)
{
    size_t sizeInBytes = sizeof(g2fw::sched_arc_cmd_nic_edma_ops_t);

    if (dmaType == g2fw::NIC_EDMA_CMD_SIBO_OPS_V3)
    {
        sizeInBytes += sizeof(g2fw::arc_cmd_nic_edma_sibo_ops_v3_t);
    }
    else if (dmaType == g2fw::NIC_EDMA_CMD_LIN_OPS_V3)
    {
        sizeInBytes += sizeof(g2fw::arc_cmd_nic_edma_lin_ops_v3_t);
    }
    else if (dmaType == g2fw::NIC_EDMA_CMD_SIBO_MEMSET_V3)
    {
        sizeInBytes += sizeof(g2fw::arc_cmd_nic_edma_sibo_memset_v3_t);
    }
    else  // (dmaType == g2fw::NIC_EDMA_CMD_LIN_MEMSET_V3)
    {
        sizeInBytes += sizeof(g2fw::arc_cmd_nic_edma_lin_memset_v3_t);
    }

    g2fw::sched_arc_cmd_nic_edma_ops_t* command =
        reinterpret_cast<g2fw::sched_arc_cmd_nic_edma_ops_t*>(scalStream.getNextPtr(sizeInBytes));
    memset(command, 0, sizeInBytes);

    static const unsigned opcodes[(unsigned)hcl::SchedulersIndex::count] = {
        g2fw::SCHED_GC_REDUCTION_ARC_CMD_NIC_EDMA_OPS,
        g2fw::SCHED_SCALEUP_SEND_ARC_CMD_NIC_EDMA_OPS,
    };

    static const unsigned groupEngine[(unsigned)hcl::SchedulersIndex::count][g2fw::NIC_EDMA_COUNT] = {
        {SCAL_EDMA_NETWORK_GC_REDUCTION_GROUP0,
         SCAL_EDMA_NETWORK_GC_REDUCTION_GROUP0,
         SCAL_EDMA_NETWORK_GC_REDUCTION_GROUP0,
         SCAL_EDMA_NETWORK_GC_REDUCTION_GROUP0,
         SCAL_EDMA_NETWORK_GC_REDUCTION_GROUP0},  // dma scheduler
        {SCAL_EDMA_NETWORK_SCALE_UP_SEND_GROUP0,
         SCAL_EDMA_NETWORK_SCALE_UP_SEND_GROUP0,
         0,
         0},  // scaleup send scheduler
        {SCAL_EDMA_NETWORK_SCALE_UP_RECV_GROUP0,
         SCAL_EDMA_NETWORK_SCALE_UP_RECV_GROUP0,
         0,
         0},  // scaleup recv scheduler
        {0, 0, 0, 0},
        {0, 0, 0, 0}};

    SET_FIELD(command->opcode, opcodes[schedIdx]);
    if (dmaType == g2fw::NIC_EDMA_CMD_SIBO_OPS_V3)
    {
        SET_FIELD(command->cmd_size, sizeof(g2fw::arc_cmd_nic_edma_sibo_ops_v3_t) + sizeof(uint32_t));
    }
    else if (dmaType == g2fw::NIC_EDMA_CMD_LIN_OPS_V3)
    {
        SET_FIELD(command->cmd_size, sizeof(g2fw::arc_cmd_nic_edma_lin_ops_v3_t) + sizeof(uint32_t));
    }
    else if (dmaType == g2fw::NIC_EDMA_CMD_SIBO_MEMSET_V3)
    {
        SET_FIELD(command->cmd_size, sizeof(g2fw::arc_cmd_nic_edma_sibo_memset_v3_t) + sizeof(uint32_t));
    }
    else  // (dmaType == g2fw::NIC_EDMA_CMD_LIN_MEMSET_V3)
    {
        SET_FIELD(command->cmd_size, sizeof(g2fw::arc_cmd_nic_edma_lin_memset_v3_t) + sizeof(uint32_t));
    }

    SET_FIELD(command->engine_group_type, groupEngine[schedIdx][dmaType]);

    VERIFY(command->engine_group_type != 0, "unsupported dmaType [{}] for {}", dmaType, __func__);

    if (dmaType == static_cast<uint32_t>(g2fw::NIC_EDMA_CMD_SIBO_OPS_V3))
    {
        LOG_TRACE(HCL, "SchedArcCommandsGaudi2::serializeDmaCommandV3 First address(0x{:x})", soAddressLSB);
        auto firstSoIdxBaseIdx        = getSoIdxBaseIdx(soAddressLSB);
        LOG_TRACE(HCL, "SchedArcCommandsGaudi2::serializeDmaCommandV3 Second address(0x{:x})", secondSoAddress);
        auto secondSoIdxBaseIdx       = getSoIdxBaseIdx(secondSoAddress);
        struct g2fw::arc_cmd_nic_edma_sibo_ops_v3_t* edma_ops =
            (struct g2fw::arc_cmd_nic_edma_sibo_ops_v3_t*)&command->sibo_ops_v3;

        SET_FIELD(edma_ops->reduction_op, getReductionOp(reduceOp));
        SET_FIELD(edma_ops->sibo_index, (indexOfReproBuffer * numberOfReproBuffers));
        SET_FIELD(edma_ops->rank_count, (numberOfRanks - 1));
        SET_FIELD(edma_ops->rank_offset_in_sibo, (isForScaleout ? 1 : 0));
        SET_FIELD(edma_ops->pool_id, poolId);
        SET_FIELD(edma_ops->opcode, dmaType);
        SET_FIELD(edma_ops->transfer_size, size);
        SET_FIELD(edma_ops->dst_addr_lo, (destAddress & 0xffffffff));
        SET_FIELD(edma_ops->signal_second, (secondSoAddress != 0));
        SET_FIELD(edma_ops->sob_base, (firstSoIdxBaseIdx.baseIdx & 0x7));
        SET_FIELD(edma_ops->sob_index, (firstSoIdxBaseIdx.soIdx & 0x3ff));
        SET_FIELD(edma_ops->second_sob_base, (secondSoIdxBaseIdx.baseIdx & 0x7));
        SET_FIELD(edma_ops->second_sob_index, (secondSoIdxBaseIdx.soIdx & 0x3ff));
        SET_FIELD(edma_ops->dst_addr_hi, (destAddress >> 32));
        SET_FIELD(edma_ops->src_addr_lo, (srcAddress & 0xffffffff));
        SET_FIELD(edma_ops->src_addr_hi, ((srcAddress >> 32) & 0xffffff));
        SET_FIELD(edma_ops->local_datasize, (is16BitMemcpy ? 1 : 2));  // 16bit / 32bit
        SET_FIELD(edma_ops->sibo_datasize, (is16BitMemcpy ? 1 : 2));   // 16bit / 32bit
        SET_FIELD(edma_ops->output_datasize,
                  (((is16BitMemcpy && !useCasting) || (!is16BitMemcpy && useCasting)) ? 1 : 2));
        SET_FIELD(edma_ops->dtype, (((is16BitMemcpy || useCasting) && isBFloat) ? 3 : 2));  // BF / FP (16bit or 32bit)
        SET_FIELD(edma_ops->reduction_ind, 1);
        SET_FIELD(edma_ops->context_id, streamCtxtID);

        LOG_TRACE(
            HCL_SUBMIT,
            "Packets | Serializing sched_arc_cmd_nic_edma_ops_t command with arc_cmd_nic_edma_sibo_ops_v3_t. "
            "sched: {}, Command[0-3]: 0x{:x}, 0x{:x}, 0x{:x}, sched: command address: 0x{:x}, sched_opcode: {}, "
            "cmd_size:{} "
            "engine_group_type:{}, "
            "opcode:{}, sibo_index:{}, rank_offset_in_sibo:{}, "
            "rank_count:{}, signal_second:{}, "
            "sob_base:{}, sob_index:0x{:x}, (soAddressLSB:0x{:x}), "
            "second_sob_base:{}, second_sob_index:0x{:x}, (secondSoAddress:0x{:x}), "
            "transfer_size:{}, pool_id:{}, "
            "srcAddr:0x{:x}, dstAddr:0x{:x}, dst_addr_lo:0x{:x}, dst_addr_hi:0x{:x}, src_addr_lo:0x{:x}, "
            "src_addr_hi:0x{:x}, "
            "reduction_ind:{}, reduction_op:{}, local_datasize:{}, sibo_datasize:{}, "
            "output_datasize:{}, dtype:{}, on stream:{}",
            schedIdx,
            *((uint32_t*)(command)),
            *((uint32_t*)(command) + 1),
            *((uint32_t*)(command) + 2),
            (uint64_t)command,
            command->opcode,
            command->cmd_size,
            command->engine_group_type,
            (uint32_t)edma_ops->opcode,
            (uint32_t)edma_ops->sibo_index,
            (uint32_t)edma_ops->rank_offset_in_sibo,
            (uint32_t)edma_ops->rank_count,
            (bool)edma_ops->signal_second,
            (uint32_t)edma_ops->sob_base,
            (uint32_t)edma_ops->sob_index,
            (uint32_t)soAddressLSB,
            (uint32_t)edma_ops->second_sob_base,
            (uint32_t)edma_ops->second_sob_index,
            (uint32_t)secondSoAddress,
            (uint32_t)edma_ops->transfer_size,
            (uint32_t)edma_ops->pool_id,
            (uint64_t)srcAddress,
            (uint64_t)destAddress,
            (uint64_t)edma_ops->dst_addr_lo,
            (uint64_t)edma_ops->dst_addr_hi,
            (uint64_t)edma_ops->src_addr_lo,
            (uint64_t)edma_ops->src_addr_hi,
            (uint32_t)edma_ops->reduction_ind,
            (uint32_t)edma_ops->reduction_op,
            (uint32_t)edma_ops->local_datasize,
            (uint32_t)edma_ops->sibo_datasize,
            (uint32_t)edma_ops->output_datasize,
            (uint32_t)edma_ops->dtype,
             *(scalStream.getStreamName()));
    }
    else if (dmaType == static_cast<uint32_t>(g2fw::NIC_EDMA_CMD_LIN_OPS_V3))
    {
        struct g2fw::arc_cmd_nic_edma_lin_ops_v3_t* edma_ops =
            (struct g2fw::arc_cmd_nic_edma_lin_ops_v3_t*)&command->lin_ops_v3;

        SET_FIELD(edma_ops->reduction_op, getReductionOp(reduceOp));
        SET_FIELD(edma_ops->sob_address, (soAddressLSB & 0x7ffffff));
        SET_FIELD(edma_ops->opcode, dmaType);
        SET_FIELD(edma_ops->transfer_size, size);
        SET_FIELD(edma_ops->dst_addr_lo, (destAddress & 0xffffffff));
        SET_FIELD(edma_ops->dst_addr_hi, (destAddress >> 32));
        SET_FIELD(edma_ops->src_addr_lo, (srcAddress & 0xffffffff));
        SET_FIELD(edma_ops->src_addr_hi, ((srcAddress >> 32) & 0xffffff));
        SET_FIELD(edma_ops->input_datasize, (is16BitMemcpy ? 1 : 2));                    // 16bit / 32bit
        SET_FIELD(edma_ops->output_datasize, ((is16BitMemcpy && !useCasting) ? 1 : 2));  // 16bit / 32bit
        SET_FIELD(edma_ops->dtype, (((is16BitMemcpy || useCasting) && isBFloat) ? 3 : 2));
        SET_FIELD(edma_ops->reduction_ind, (useReductionInd ? 1 : 0));
        SET_FIELD(edma_ops->context_id, streamCtxtID);

        LOG_TRACE(HCL_SUBMIT,
                  "Packets | Serializing sched_arc_cmd_nic_edma_ops_t command with arc_cmd_nic_edma_lin_ops_v3_t. "
                  "sched: {}, Command[0-3]: 0x{:x}, 0x{:x}, 0x{:x}, sched: command address: 0x{:x}, sched_opcode: {}, "
                  "cmd_size:{} "
                  "engine_group_type:{}, "
                  "opcode:{}, "
                  "sob_address:0x{:x}, transfer_size:{}, "
                  "srcAddr:0x{:x}, dstAddr:0x{:x}, dst_addr_lo:0x{:x}, dst_addr_hi:0x{:x}, src_addr_lo:0x{:x}, "
                  "src_addr_hi:0x{:x}, "
                  "reduction_ind:{}, reduction_op:{}, input_datasize:{}, output_datasize:{}, data_type:{}, "
                  "on stream:{}",
                  schedIdx,
                  *((uint32_t*)(command)),
                  *((uint32_t*)(command) + 1),
                  *((uint32_t*)(command) + 2),
                  (uint64_t)command,
                  command->opcode,
                  command->cmd_size,
                  command->engine_group_type,
                  (uint32_t)edma_ops->opcode,
                  (uint64_t)edma_ops->sob_address,
                  (uint32_t)edma_ops->transfer_size,
                  (uint64_t)srcAddress,
                  (uint64_t)destAddress,
                  (uint64_t)edma_ops->dst_addr_lo,
                  (uint64_t)edma_ops->dst_addr_hi,
                  (uint64_t)edma_ops->src_addr_lo,
                  (uint64_t)edma_ops->src_addr_hi,
                  (uint32_t)edma_ops->reduction_ind,
                  (uint32_t)edma_ops->reduction_op,
                  (uint32_t)edma_ops->input_datasize,
                  (uint32_t)edma_ops->output_datasize,
                  (uint32_t)edma_ops->dtype,
                  *(scalStream.getStreamName()));
    }
    else if (dmaType == static_cast<uint32_t>(g2fw::NIC_EDMA_CMD_SIBO_MEMSET_V3))
    {
        struct g2fw::arc_cmd_nic_edma_sibo_memset_v3_t* edma_ops =
            (struct g2fw::arc_cmd_nic_edma_sibo_memset_v3_t*)&command->sibo_memset_v3;

        SET_FIELD(edma_ops->sob_address, (soAddressLSB & 0x7ffffff));
        SET_FIELD(edma_ops->opcode, dmaType);
        SET_FIELD(edma_ops->transfer_size, size);
        SET_FIELD(edma_ops->sibo_index, (indexOfReproBuffer * numberOfReproBuffers));
        SET_FIELD(edma_ops->rank_count, numberOfRanks);
        SET_FIELD(edma_ops->rank_offset_in_sibo, 0);
        SET_FIELD(edma_ops->pool_id, poolId);
        SET_FIELD(edma_ops->context_id, streamCtxtID);
        SET_FIELD(edma_ops->memset_value, 0);

        LOG_TRACE(HCL_SUBMIT,
                  "Packets | Serializing sched_arc_cmd_nic_edma_ops_t command with arc_cmd_nic_edma_sibo_memset_v3_t. "
                  "sched: {}, Command[0-3]: 0x{:x}, 0x{:x}, 0x{:x}, sched: command address: 0x{:x}, sched_opcode: {}, "
                  "cmd_size:{} "
                  "engine_group_type:{}, "
                  "opcode:{}, sibo_index:{}, rank_offset_in_sibo:{}, pool_id: {} , "
                  "rank_count:{}, sob_address:0x{:x}, transfer_size:{}, memset_value:{} on stream:{}",
                  schedIdx,
                  *((uint32_t*)(command)),
                  *((uint32_t*)(command) + 1),
                  *((uint32_t*)(command) + 2),
                  (uint64_t)command,
                  command->opcode,
                  command->cmd_size,
                  command->engine_group_type,
                  (uint32_t)edma_ops->opcode,
                  (uint32_t)edma_ops->sibo_index,
                  (uint32_t)edma_ops->rank_offset_in_sibo,
                  (uint32_t)edma_ops->pool_id,
                  (uint32_t)edma_ops->rank_count,
                  (uint64_t)edma_ops->sob_address,
                  (uint32_t)edma_ops->transfer_size,
                  (uint32_t)edma_ops->memset_value,
                   *(scalStream.getStreamName()));
    }
    else  //(dmaType == static_cast<uint32_t>(g2fw::NIC_EDMA_CMD_LIN_MEMSET_V3))
    {
        struct g2fw::arc_cmd_nic_edma_lin_memset_v3_t* edma_ops =
            (struct g2fw::arc_cmd_nic_edma_lin_memset_v3_t*)&command->edma_lin_memset;

        SET_FIELD(edma_ops->sob_address, (soAddressLSB & 0x7ffffff));
        SET_FIELD(edma_ops->opcode, dmaType);
        SET_FIELD(edma_ops->transfer_size, size);
        SET_FIELD(edma_ops->dst_addr_lo, (destAddress & 0xffffffff));
        SET_FIELD(edma_ops->dst_addr_hi, (destAddress >> 32));
        SET_FIELD(edma_ops->context_id, streamCtxtID);
        SET_FIELD(edma_ops->memset_value, 0);
        LOG_TRACE(HCL_SUBMIT,
                  "Packets | Serializing sched_arc_cmd_nic_edma_ops_t command with arc_cmd_nic_edma_lin_memset_v3_t. "
                  "sched: {}, Command[0-3]: 0x{:x}, 0x{:x}, 0x{:x}, sched: command address: 0x{:x}, sched_opcode: {}, "
                  "cmd_size:{} "
                  "engine_group_type:{}, "
                  "opcode:{}, "
                  "sob_address:0x{:x}, transfer_size:{}, "
                  "dstAddr:0x{:x}, dst_addr_lo:0x{:x}, dst_addr_hi:0x{:x}, memset_value:{} on stream:{}",
                  schedIdx,
                  *((uint32_t*)(command)),
                  *((uint32_t*)(command) + 1),
                  *((uint32_t*)(command) + 2),
                  (uint64_t)command,
                  command->opcode,
                  command->cmd_size,
                  command->engine_group_type,
                  (uint32_t)edma_ops->opcode,
                  (uint64_t)edma_ops->sob_address,
                  (uint32_t)edma_ops->transfer_size,
                  (uint64_t)destAddress,
                  (uint64_t)edma_ops->dst_addr_lo,
                  (uint64_t)edma_ops->dst_addr_hi,
                  (uint32_t)edma_ops->memset_value,
                   *(scalStream.getStreamName()));
    }
}

void SchedArcCommandsGaudi2::serializePdmaCommand(hcl::ScalStreamBase& scalStream,
                                                  unsigned             schedIdx,
                                                  bool                 isDownload,
                                                  uint64_t             hostAddress,
                                                  uint64_t             deviceAddress,
                                                  uint32_t             size,
                                                  bool                 isReduction,
                                                  hcclRedOp_t          reduceOp,
                                                  bool                 isCastUp,
                                                  uint8_t              apiId,
                                                  unsigned             streamIndex,
                                                  hcclDataType_t       dataType,
                                                  uint32_t             sobAddr)
{
    static const unsigned opcodes[(unsigned)hcl::SchedulersIndex::count] = {
        g2fw::SCHED_GC_REDUCTION_ARC_CMD_COUNT,
        g2fw::SCHED_SCALEUP_SEND_ARC_CMD_COUNT,
        g2fw::SCHED_SCALEUP_RECV_ARC_CMD_COUNT,
        g2fw::SCHED_SCALEOUT_SEND_ARC_CMD_PDMA_BATCH_TRANSFER,
        g2fw::SCHED_SCALEOUT_RECV_ARC_CMD_PDMA_BATCH_TRANSFER};

    uint8_t batchCount = 1;  // HCL use only single transfer mode
    size_t  cmdSize =
        sizeof(g2fw::sched_arc_cmd_pdma_batch_transfer_t) + batchCount * sizeof(g2fw::sched_arc_pdma_commands_params_t);

    g2fw::sched_arc_cmd_pdma_batch_transfer_t* command =
        reinterpret_cast<g2fw::sched_arc_cmd_pdma_batch_transfer_t*>(scalStream.getNextPtr(cmdSize));
    memset(command, 0, cmdSize);

    VERIFY(schedIdx == (unsigned)hcl::SchedulersIndex::sendScaleOut ||
           schedIdx == (unsigned)hcl::SchedulersIndex::recvScaleOut);

    if (isDownload)
    {
        SET_FIELD(command->engine_group_type, (unsigned)SCAL_PDMA_NETWORK_SCALE_OUT_RECV_GROUP);
        SET_FIELD(command->workload_type,
                  (isCastUp ? g2fw::ENG_PDMA_ARC_CMD_BATCH_WITH_FRAGMENTATION_CASTUP
                            : g2fw::ENG_PDMA_ARC_CMD_BATCH_WITH_FRAGMENTATION));
        SET_FIELD(command->batch_params->src_addr, hostAddress);
        SET_FIELD(command->batch_params->dst_addr, deviceAddress);
    }
    else  // upload
    {
        VERIFY(!isCastUp, "upload cannot require cast up");
        SET_FIELD(command->engine_group_type, (unsigned)SCAL_PDMA_NETWORK_SCALE_OUT_SEND_GROUP);
        SET_FIELD(command->workload_type, g2fw::ENG_PDMA_ARC_CMD_BATCH_NO_FRAGMENTATION);
        SET_FIELD(command->batch_params->src_addr, deviceAddress);
        SET_FIELD(command->batch_params->dst_addr, hostAddress);
    }

    SET_FIELD(command->opcode, opcodes[schedIdx]);
    SET_FIELD(command->watch_dog_sig_value, 0);
    SET_FIELD(command->has_payload, 1);
    SET_FIELD(command->signal_to_cg, 0);
    SET_FIELD(command->reduction_ind, isReduction);
    SET_FIELD(command->reduction_op, getReductionOp(reduceOp));
    SET_FIELD(command->reduction_dtype, getReductionDataType(isCastUp, dataType));
    SET_FIELD(command->pay_data, 0x80000001);
    SET_FIELD(command->pay_addr, sobAddr);  // should also indicate 4 bit for cg index
    SET_FIELD(command->batch_params->transfer_size, size);
    SET_FIELD(command->batch_count, batchCount);
    SET_FIELD(command->api_id, apiId);
    SET_FIELD(command->stream_ctxt_id, getPdmaCtxtId(isDownload, streamIndex));

    if (command->has_payload)
    {
        VERIFY(!command->signal_to_cg, "both cannot be used at the same time");
    }
}

uint8_t SchedArcCommandsGaudi2::getPdmaCtxtId(bool isDownload, unsigned streamIndex)
{
    PdmaDirCtx         direction  = isDownload ? PdmaDirCtx::DOWN : PdmaDirCtx::UP;
    internalStreamType streamType = internalStreamType::INTERNAL_STREAM_TYPE_COLLECTIVE_NETWORK;

    return (((((uint8_t)direction) & ContextEncoding::DIR_MASK) << ContextEncoding::DIR_OFFSET) |
            (((uint8_t)streamType) & ContextEncoding::TYPE_MASK) << ContextEncoding::TYPE_OFFSET) |
           ((((uint8_t)streamIndex) & ContextEncoding::STREAM_MASK) << ContextEncoding::STREAM_OFFSET);
}

void SchedArcCommandsGaudi2::serializeGlobalDmaCommandV2(hcl::ScalStreamBase&                  scalStream,
                                                         uint32_t                              soAddressLSB,
                                                         const std::vector<sibAddressAndSize>& sibAddressesAndSizes,
                                                         uint32_t                              engineType)
{
    size_t sizeInBytes = sizeof(g2fw::sched_arc_cmd_nic_edma_ops_t) +
                         8 * sizeof(uint32_t);  // sched_arc_cmd_nic_edma_ops_t with arc_cmd_update_edma_nic_ctxt_v3_t
                                                // and edma_nic_glbl_ctxt_v3_t

    g2fw::sched_arc_cmd_nic_edma_ops_t* command =
        reinterpret_cast<g2fw::sched_arc_cmd_nic_edma_ops_t*>(scalStream.getNextPtr(sizeInBytes));
    memset(command, 0, sizeInBytes);

    command->opcode            = g2fw::SCHED_GC_REDUCTION_ARC_CMD_NIC_EDMA_OPS;
    command->cmd_size          = sizeInBytes;
    command->engine_group_type = engineType;

    struct g2fw::arc_cmd_update_edma_nic_ctxt_v3_t* edma_ops =
        (struct g2fw::arc_cmd_update_edma_nic_ctxt_v3_t*)&command->edma_ctxt_v3;

    edma_ops->opcode        = g2fw::NIC_EDMA_CMD_UPDATE_GLBL_CTXT_V3;
    edma_ops->update_bitmap = 0x3F;
    edma_ops->num_dwords    = 6;
    edma_ops->sob_address   = 0;

    struct g2fw::edma_nic_glbl_ctxt_v3_t* edma_ctxt = (struct g2fw::edma_nic_glbl_ctxt_v3_t*)&edma_ops->data;

    edma_ctxt->sib_base_addr[0]    = sibAddressesAndSizes.at(0).sibBaseAddr;
    edma_ctxt->sib_base_addr[1]    = sibAddressesAndSizes.at(1).sibBaseAddr;
    edma_ctxt->sibo_rank_stride[0] = sibAddressesAndSizes.at(0).sibSize;
    edma_ctxt->sibo_rank_stride[1] = sibAddressesAndSizes.at(1).sibSize;

    LOG_TRACE(HCL_SUBMIT,
              "Packets | serializeGlobalDmaCommand sched_arc_cmd_nic_edma_ops_t  |  command->opcode:{}, "
              " command->engine_group_type:{}, command->cmd_size:{} "
              "arc_cmd_update_edma_nic_ctxt_v3_t | opcode:{}, update_bitmap:{}, num_dwords:{} "
              "edma_nic_glbl_ctxt_v3_t | baseAddress[0]:0x{:x}, sibo_rank_stride[0]:{}, baseAddress[1]:0x{:x}, "
              "sibo_rank_stride[1]:{} on stream:{}",
              command->opcode,
              command->engine_group_type,
              command->cmd_size,
              edma_ops->opcode,
              edma_ops->update_bitmap,
              edma_ops->num_dwords,
              ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->sib_base_addr[0],
              ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->sibo_rank_stride[0],
              ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->sib_base_addr[1],
              ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->sibo_rank_stride[1],
               *(scalStream.getStreamName()));
}

void SchedArcCommandsGaudi2::serializeGlobalDmaCommandV3(hcl::ScalStreamBase&                  scalStream,
                                                         uint32_t                              soAddressLSB,
                                                         const std::vector<sibAddressAndSize>& sibAddressesAndSizes,
                                                         uint32_t                              fwStrideSize,
                                                         uint64_t                              fwBaseAddress,
                                                         uint32_t                              engineType)
{
    const unsigned numDwords            = div(sizeof(g2fw::edma_nic_glbl_ctxt_v3_t), sizeof(uint32_t));
    const unsigned activateAllDwordsMap = (1 << numDwords) - 1;
    // sched_arc_cmd_nic_edma_ops_t with arc_cmd_update_edma_nic_ctxt_v3_t
    // and edma_nic_glbl_ctxt_v3_t
    const size_t   sizeInBytes          = sizeof(g2fw::sched_arc_cmd_nic_edma_ops_t) +
                                          sizeof(g2fw::arc_cmd_update_edma_nic_ctxt_v3_t) +
                                          (numDwords * sizeof(uint32_t));

    g2fw::sched_arc_cmd_nic_edma_ops_t* command =
        reinterpret_cast<g2fw::sched_arc_cmd_nic_edma_ops_t*>(scalStream.getNextPtr(sizeInBytes));
    memset(command, 0, sizeInBytes);

    SET_FIELD(command->opcode, g2fw::SCHED_GC_REDUCTION_ARC_CMD_NIC_EDMA_OPS);
    SET_FIELD(command->cmd_size, sizeInBytes);
    SET_FIELD(command->engine_group_type, engineType);

    struct g2fw::arc_cmd_update_edma_nic_ctxt_v3_t* edma_ops =
        (struct g2fw::arc_cmd_update_edma_nic_ctxt_v3_t*)&command->edma_ctxt_v3;

    SET_FIELD(edma_ops->opcode, g2fw::NIC_EDMA_CMD_UPDATE_GLBL_CTXT_V3);
    SET_FIELD(edma_ops->update_bitmap, activateAllDwordsMap);
    SET_FIELD(edma_ops->num_dwords, numDwords);
    SET_FIELD(edma_ops->sob_address, (soAddressLSB & 0x7ffffff));

    struct g2fw::edma_nic_glbl_ctxt_v3_t* edma_ctxt = (struct g2fw::edma_nic_glbl_ctxt_v3_t*)&edma_ops->data;

    SET_FIELD(edma_ctxt->sib_base_addr[0], sibAddressesAndSizes.at(0).sibBaseAddr);
    SET_FIELD(edma_ctxt->sib_base_addr[1], sibAddressesAndSizes.at(1).sibBaseAddr);
    SET_FIELD(edma_ctxt->sibo_rank_stride[0], sibAddressesAndSizes.at(0).sibSize);
    SET_FIELD(edma_ctxt->sibo_rank_stride[1], sibAddressesAndSizes.at(1).sibSize);
    SET_FIELD(edma_ctxt->sirb_base_addr, fwBaseAddress);
    SET_FIELD(edma_ctxt->sirb_size, fwStrideSize);

    for (int i = 0; i < 8; i++)
    {
        SET_FIELD(edma_ctxt->comp_cfg[i], getCompCfg()[i].m_base);
    }

    LOG_TRACE(
        HCL_SUBMIT,
        "Packets | serializeGlobalDmaCommand sched_arc_cmd_nic_edma_ops_t  |  command->opcode:{}, "
        " command->engine_group_type:{}, command->cmd_size:{} "
        "arc_cmd_update_edma_nic_ctxt_v3_t | opcode:{}, update_bitmap:{}, num_dwords:{} "
        "edma_nic_glbl_ctxt_v3_t | baseAddress[0]:0x{:x}, sibo_rank_stride[0]:{}, baseAddress[1]:0x{:x}, "
        "sibo_rank_stride[1]:{}, fwBaseAddress:0x{:x}, sirb_size:{}, "
        "comp_cfg: [0]:0x{:x}, [1]:0x{:x}, [2]:0x{:x}, [3]:0x{:x}, [4]:0x{:x}, [5]:0x{:x}, "
        "[6]:0x{:x}, [7]:0x{:x}, on stream {}",
        command->opcode,
        command->engine_group_type,
        command->cmd_size,
        edma_ops->opcode,
        edma_ops->update_bitmap,
        edma_ops->num_dwords,
        ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->sib_base_addr[0],
        ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->sibo_rank_stride[0],
        ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->sib_base_addr[1],
        ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->sibo_rank_stride[1],
        ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->sirb_base_addr,
        ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->sirb_size,
        ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->comp_cfg[0],
        ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->comp_cfg[1],
        ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->comp_cfg[2],
        ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->comp_cfg[3],
        ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->comp_cfg[4],
        ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->comp_cfg[5],
        ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->comp_cfg[6],
        ((struct g2fw::edma_nic_glbl_ctxt_v3_t*)(command->edma_ctxt_v3->data))->comp_cfg[7],
        *(scalStream.getStreamName()));
}

void SchedArcCommandsGaudi2::serializeUpdateGlobalContextCommand(hcl::ScalStreamBase&                scalStream,
                                                                 uint32_t                            soAddressLSB,
                                                                 std::vector<g2fw::nic_glbl_ctxt_t>& contexts,
                                                                 uint64_t sib_order_base_addr,
                                                                 uint64_t sib_acc_base_addr,
                                                                 uint32_t sibo_rank_stride,
                                                                 uint32_t siba_stride)
{
    size_t                           dwords = 3 + contexts.size();
    size_t                           size   = dwords * sizeof(uint32_t);
    struct g2fw::nic_glbl_ctxt_t*    glbl_ctxt;
    struct g2fw::nic_glbl_ctxt_v2_t* glbl_ctxt_v2;

    // Use RR flow as default in order to enable RR and non RR mode to be able to work simultaneously
    size += sizeof(g2fw::nic_glbl_ctxt_v2_t);

    g2fw::sched_arc_cmd_update_nic_glbl_ctxt_t* command =
        reinterpret_cast<g2fw::sched_arc_cmd_update_nic_glbl_ctxt_t*>(scalStream.getNextPtr(size));
    memset(command, 0, size);

    SET_FIELD(command->opcode, g2fw::SCHED_SCALEUP_RECV_ARC_CMD_UPDATE_NIC_GLBL_CTXT);
    SET_FIELD(command->engine_group_type, SCAL_NIC_RECEIVE_SCALE_UP_GROUP);
    SET_FIELD(command->cmd_size, size);
    SET_FIELD(command->cmd_update_glbl_ctxt.nic_opcode, g2fw::NIC_CMD_UPDATE_GLBL_CTXT);
    SET_FIELD(command->cmd_update_glbl_ctxt.num_glbl_ctxt, contexts.size());

    // Use RR flow as default in order to enable RR and non RR mode to be able to work simultaneously
    SET_FIELD(command->cmd_update_glbl_ctxt.update_bitmap, 0x3F);  // all 6 dwords involved for RR
    SET_FIELD(command->so_lbw_address, soAddressLSB);

    glbl_ctxt = (struct g2fw::nic_glbl_ctxt_t*)&command->glbl_ctxt;

    for (unsigned i = 0; i < contexts.size(); i++)
    {
        memcpy(glbl_ctxt, &contexts[i], sizeof(contexts[i]));
        glbl_ctxt++;
    }

    // Use RR flow as default in order to enable RR and non RR mode to be able to work simultaneously
    glbl_ctxt_v2 = (struct g2fw::nic_glbl_ctxt_v2_t*)(glbl_ctxt);  // starting from the point that glbl_ctxt finished

    SET_FIELD(glbl_ctxt_v2->sib_order_base_addr, sib_order_base_addr);
    SET_FIELD(glbl_ctxt_v2->sib_acc_base_addr, sib_acc_base_addr);
    SET_FIELD(glbl_ctxt_v2->sibo_rank_stride, sibo_rank_stride);
    SET_FIELD(glbl_ctxt_v2->siba_stride, siba_stride);

    LOG_TRACE(HCL_SUBMIT,
              "Packets | serializeUpdateGlobalContextCommand  sched_arc_cmd_update_nic_glbl_ctxt_t  |  "
              "command->opcode:{}, "
              " command->engine_group_type:{}, command->cmd_size:{}, command->so_lbw_address:0x{:x}, "
              "update_bitmap:0x{:x} "
              "nic_glbl_ctxt_v2_t | sib_order_base_addr:0x{:x}, sib_acc_base_addr:0x{:x}, sibo_rank_stride:{}, "
              "siba_stride:{} on stream:{}",
              command->opcode,
              command->engine_group_type,
              command->cmd_size,
              (uint64_t)command->cmd_update_glbl_ctxt.update_bitmap,
              (uint64_t)command->so_lbw_address,
              glbl_ctxt_v2->sib_order_base_addr,
              glbl_ctxt_v2->sib_acc_base_addr,
              glbl_ctxt_v2->sibo_rank_stride,
              glbl_ctxt_v2->siba_stride,
               *(scalStream.getStreamName()));
}

void SchedArcCommandsGaudi2::serializeUpdateGlobalContextScaleOutCommand(hcl::ScalStreamBase& scalStream,
                                                                         uint32_t             soAddressLSB,
                                                                         std::vector<g2fw::nic_glbl_ctxt_t>& contexts,
                                                                         uint32_t start_nic_index)
{
    size_t                        dwords = 3 + contexts.size();
    size_t                        size   = dwords * sizeof(uint32_t);
    struct g2fw::nic_glbl_ctxt_t* glbl_ctxt;

    // Use RR flow as default in order to enable RR and non RR mode to be able to work simultaneously
    size += sizeof(g2fw::nic_glbl_ctxt_v2_t);

    g2fw::sched_arc_cmd_update_nic_glbl_ctxt_t* command =
        reinterpret_cast<g2fw::sched_arc_cmd_update_nic_glbl_ctxt_t*>(scalStream.getNextPtr(size));
    memset(command, 0, size);

    SET_FIELD(command->opcode, g2fw::SCHED_SCALEOUT_RECV_ARC_CMD_UPDATE_NIC_GLBL_CTXT);
    SET_FIELD(command->engine_group_type, SCAL_NIC_RECEIVE_SCALE_OUT_GROUP);
    SET_FIELD(command->cmd_size, size);

    // cmd_update_glbl_ctxt - > scaleout_cmd_update_glbl_ctxt
    SET_FIELD(command->scaleout_cmd_update_glbl_ctxt.nic_opcode, g2fw::NIC_SCALEOUT_CMD_UPDATE_GLBL_CTXT);
    SET_FIELD(command->scaleout_cmd_update_glbl_ctxt.num_glbl_ctxt, contexts.size());

    /* Port 8  -> start_nic_idx 0
     * Port 22 -> start_nic_idx 1
     * Port 23 -> start_nic_idx 2
     * Scaleout NIC index for which nic_glbl_ctxt_t are provided.
     * Updates will be made to global context structure starting
     * from start_nic_idx.
     * Valid values: 0/1/2
     * Example:
       +-------------------------------+---------------------------+-----------------------------+
       |         start_nic_idx         |      contexts.size()      |       active SO NICs        |
       +-------------------------------+---------------------------+-----------------------------+
       |                0              |             3             |           8,22,23           |
       +-------------------------------+---------------------------+-----------------------------+
       |                0              |             1             |              8              |
       +-------------------------------+---------------------------+-----------------------------+
       |                0              |             2             |             8,22            |
       +-------------------------------+---------------------------+-----------------------------+
       |                1              |             2             |            22,23            |
       +-------------------------------+---------------------------+-----------------------------+
       |                2              |             1             |              23             |
       +-------------------------------+---------------------------+-----------------------------+
    */

    SET_FIELD(command->scaleout_cmd_update_glbl_ctxt.start_nic_idx, start_nic_index);
    SET_FIELD(command->so_lbw_address, soAddressLSB);

    glbl_ctxt = (struct g2fw::nic_glbl_ctxt_t*)&command->glbl_ctxt;

    for (unsigned i = 0; i < contexts.size(); i++)
    {
        memcpy(glbl_ctxt, &contexts[i], sizeof(contexts[i]));
        glbl_ctxt++;
    }

    LOG_TRACE(HCL_SUBMIT,
              "Packets | serializeUpdateGlobalContextScaleOutCommand sched_arc_cmd_update_nic_glbl_ctxt_t  |  "
              "command->opcode:{}, "
              " command->engine_group_type:{}, command->cmd_size:{}, command->so_lbw_address:0x{:x} "
              " command->scaleout_cmd_update_glbl_ctxt.nic_opcode: {} "
              " command->scaleout_cmd_update_glbl_ctxt.num_glbl_ctxt: {} "
              " command->scaleout_cmd_update_glbl_ctxt.start_nic_idx: {} on stream:{}",
              command->opcode,
              command->engine_group_type,
              command->cmd_size,
              (uint64_t)command->so_lbw_address,
              command->scaleout_cmd_update_glbl_ctxt.nic_opcode,
              command->scaleout_cmd_update_glbl_ctxt.num_glbl_ctxt,
              command->scaleout_cmd_update_glbl_ctxt.start_nic_idx,
               *(scalStream.getStreamName()));
}

void SchedArcCommandsGaudi2::serializeUpdateCollectiveContextCommand(hcl::ScalStreamBase&           scalStream,
                                                                     bool                           isSend,
                                                                     unsigned                       collectiveContextIndex,
                                                                     unsigned                       commDescIndex,
                                                                     ContextManager::ContextValues& contextValues)
{
    size_t dwordsNumForUpdate = contextValues.second;
    size_t size               = (2 + dwordsNumForUpdate) * sizeof(uint32_t);
    g2fw::sched_arc_cmd_update_nic_coll_ctxt_t* command =
        reinterpret_cast<g2fw::sched_arc_cmd_update_nic_coll_ctxt_t*>(scalStream.getNextPtr(size));
    memset(command, 0, size);

    SET_FIELD(command->opcode,
              (isSend ? (int)g2fw::SCHED_SCALEUP_SEND_ARC_CMD_UPDATE_NIC_COLL_CTXT
                      : (int)g2fw::SCHED_SCALEUP_RECV_ARC_CMD_UPDATE_NIC_COLL_CTXT));
    SET_FIELD(command->engine_group_type,
              (isSend ? (uint32_t)SCAL_NIC_SEND_SCALE_UP_GROUP : (uint32_t)SCAL_NIC_RECEIVE_SCALE_UP_GROUP));
    SET_FIELD(command->cmd_update_coll_ctxt.nic_opcode, g2fw::NIC_CMD_UPDATE_COLL_CTXT);
    SET_FIELD(command->cmd_update_coll_ctxt.num_dwords, dwordsNumForUpdate);
    SET_FIELD(command->cmd_update_coll_ctxt.ctxt_id, collectiveContextIndex);
    SET_FIELD(command->cmd_update_coll_ctxt.comm_desc_index, commDescIndex);

    // Have to reset these fields as the ptr may contain garbage.
    SET_FIELD(command->cmd_update_coll_ctxt.update_qpn, 0);
    SET_FIELD(command->cmd_update_coll_ctxt.update_rri_ce, 0);
    SET_FIELD(command->cmd_update_coll_ctxt.update_bitmap, 0);

    LOG_INFO(HCL_SUBMIT,
             "Serializing a collective context update for collectiveContext = {}, "
             "(commDescIndex={}, {} dwords): on stream:{}",
             collectiveContextIndex,
             commDescIndex,
             dwordsNumForUpdate,
              *(scalStream.getStreamName()));

    int i = 0;
    for (size_t dword = 0; dword < contextValues.first.size(); dword++)
    {
        ContextManager::ContextValueUpdater& contextValueUpdater = (contextValues.first).at(dword);
        if (!contextValueUpdater.needUpdate) continue;

        LOG_DEBUG(HCL_SUBMIT, "    DW{} updating to value 0x{:x}", (eDWords)dword, contextValueUpdater.value);
        switch (dword)
        {
            case DW_COMM_QP:
                SET_FIELD(command->cmd_update_coll_ctxt.update_qpn, 1);
                break;
            case DW_REMOTE_RANK:
                SET_FIELD(command->cmd_update_coll_ctxt.update_rri_ce, 1);
                break;
            default:
                SET_FIELD(command->cmd_update_coll_ctxt.update_bitmap, (command->cmd_update_coll_ctxt.update_bitmap | (1 << (uint8_t)dword)));
                break;
        }
        SET_FIELD(command->dwords[i].dword_value, contextValueUpdater.value);
        i++;
    }
}

void SchedArcCommandsGaudi2::serializeCollectiveSendShortCommand(hcl::ScalStreamBase& scalStream,
                                                                 unsigned             collectiveContextIndex,
                                                                 unsigned             commDescIndex,
                                                                 bool                 isSend,
                                                                 bool                 hasBufferSize,
                                                                 uint32_t             bufferSize,
                                                                 unsigned             syncObjectAddressIndex,
                                                                 bool                 force_remote_rank_offset,
                                                                 uint32_t             cacheLineCount,
                                                                 uint32_t             cacheLineRemainder,
                                                                 uint8_t              elementRemainder,
                                                                 uint32_t             address,  // lsb
                                                                 bool                 notifyRndvAck,
                                                                 bool                 waitForRndvAcks)
{
    size_t                                 dwords = 3;
    struct g2fw::arc_cmd_coll_ops_short_t* cmd_coll_ops_short;

    if (hasBufferSize)
    {
        dwords++;
    }
    g2fw::sched_arc_cmd_nic_coll_ops_t* command =
        reinterpret_cast<g2fw::sched_arc_cmd_nic_coll_ops_t*>(scalStream.getNextPtr(dwords * sizeof(uint32_t)));
    memset(command, 0, dwords * sizeof(uint32_t));

    SET_FIELD(command->opcode,
              (isSend ? (int)g2fw::SCHED_SCALEUP_SEND_ARC_CMD_NIC_COLL_OPS
                      : (int)g2fw::SCHED_SCALEUP_RECV_ARC_CMD_NIC_COLL_OPS));
    SET_FIELD(command->engine_group_type,
              (isSend ? (uint32_t)SCAL_NIC_SEND_SCALE_UP_GROUP : (uint32_t)SCAL_NIC_RECEIVE_SCALE_UP_GROUP));
    SET_FIELD(command->cmd_size, (dwords * sizeof(uint32_t)));

    cmd_coll_ops_short = (struct g2fw::arc_cmd_coll_ops_short_t*)&command->cmd_coll_ops_short;

    SET_FIELD(cmd_coll_ops_short->cache_line_count, cacheLineCount);
    SET_FIELD(cmd_coll_ops_short->cache_line_remainder, cacheLineRemainder);
    SET_FIELD(cmd_coll_ops_short->element_remainder, elementRemainder);
    SET_FIELD(cmd_coll_ops_short->force_remote_rank_offset, force_remote_rank_offset);
    SET_FIELD(cmd_coll_ops_short->sob_index, syncObjectAddressIndex);
    SET_FIELD(cmd_coll_ops_short->has_size, hasBufferSize);
    SET_FIELD(cmd_coll_ops_short->notify_rndv_ack, notifyRndvAck);
    SET_FIELD(cmd_coll_ops_short->wait_for_rndv_acks, waitForRndvAcks);
    SET_FIELD(cmd_coll_ops_short->coll_ctxt_id, collectiveContextIndex);
    SET_FIELD(cmd_coll_ops_short->nic_opcode, 1);
    SET_FIELD(cmd_coll_ops_short->comm_desc_index, commDescIndex);
    SET_FIELD(cmd_coll_ops_short->buffer_addr_lsb, (address >> 4));

    if (hasBufferSize)
    {
        std::memcpy(&cmd_coll_ops_short->buffer_size, &bufferSize, sizeof(uint32_t));
    }

    LOG_TRACE(HCL_SUBMIT,
              "Packets | serializeCollectiveSendShortCommand sched_arc_cmd_nic_coll_ops_t  |  command->opcode:{}, "
              " command->engine_group_type:{}, command->cmd_size:{}, "
              " cache_line_count:{}, cache_line_remainder:{}, element_remainder:{}, "
              " sob_index:{}, has_size:{}, notify_rndv_ack:{}, wait_for_rndv_acks:{} coll_ctxt_id:{} nic_opcode:{}, "
              " comm_desc_index:{}, buffer_addr_lsb:0x{:x}, buffer_size:{} on stream:{}",
              command->opcode,
              command->engine_group_type,
              command->cmd_size,
              cmd_coll_ops_short->cache_line_count,
              cmd_coll_ops_short->cache_line_remainder,
              cmd_coll_ops_short->element_remainder,
              cmd_coll_ops_short->sob_index,
              cmd_coll_ops_short->has_size,
              cmd_coll_ops_short->notify_rndv_ack,
              cmd_coll_ops_short->wait_for_rndv_acks,
              cmd_coll_ops_short->coll_ctxt_id,
              cmd_coll_ops_short->nic_opcode,
              cmd_coll_ops_short->comm_desc_index,
              cmd_coll_ops_short->buffer_addr_lsb,
              bufferSize,
               *(scalStream.getStreamName()));
}

void SchedArcCommandsGaudi2::serializeCollectiveRecvShortInOrderCommand(hcl::ScalStreamBase& scalStream,
                                                                        unsigned             collectiveContextIndex,
                                                                        unsigned             commDescIndex,
                                                                        bool                 hasBufferSize,
                                                                        unsigned             syncObjectAddressIndex,
                                                                        uint32_t             cacheLineCount,
                                                                        uint32_t             currentRank,
                                                                        uint32_t             accuIndex,
                                                                        uint32_t             rrIndex,
                                                                        uint32_t             numOfRanks,
                                                                        uint8_t              nicsBitmap,
                                                                        uint32_t             poolId)
{
    size_t                                              dwords = 3;  // 1 for the sched_arc, 2 for the arc_cmd
    struct g2fw::arc_cmd_coll_ops_recv_short_inorder_v2_t* cmd_coll_ops_short;

    g2fw::sched_arc_cmd_nic_coll_ops_t* command =
        reinterpret_cast<g2fw::sched_arc_cmd_nic_coll_ops_t*>(scalStream.getNextPtr(dwords * sizeof(uint32_t)));
    memset(command, 0, dwords * sizeof(uint32_t));

    SET_FIELD(command->opcode, (int)g2fw::SCHED_SCALEUP_RECV_ARC_CMD_NIC_COLL_OPS);
    SET_FIELD(command->engine_group_type, (uint32_t)SCAL_NIC_RECEIVE_SCALE_UP_GROUP);
    SET_FIELD(command->cmd_size, (dwords * sizeof(uint32_t)));

    cmd_coll_ops_short =
        (struct g2fw::arc_cmd_coll_ops_recv_short_inorder_v2_t*)&command->cmd_coll_ops_short_inorder_v2;

    SET_FIELD(cmd_coll_ops_short->cache_line_count, cacheLineCount);
    SET_FIELD(cmd_coll_ops_short->sob_index, syncObjectAddressIndex);
    SET_FIELD(cmd_coll_ops_short->local_rank_index, (GCFG_HCL_USE_EDMA_COMMAND_V3.value() ? currentRank : 7));
    SET_FIELD(cmd_coll_ops_short->comm_desc_index, commDescIndex);
    SET_FIELD(cmd_coll_ops_short->nic_opcode, 5);  // NIC_CMD_COLL_OPS_RECV_INORDER_V2
    SET_FIELD(cmd_coll_ops_short->coll_ctxt_id, collectiveContextIndex);
    SET_FIELD(cmd_coll_ops_short->siba_index, accuIndex);  // TODORR: Change
    SET_FIELD(cmd_coll_ops_short->sibo_index, rrIndex);    // TODORR: Change
    SET_FIELD(cmd_coll_ops_short->num_ranks, 0);
    SET_FIELD(cmd_coll_ops_short->pool_id, poolId);

    // TODO: uncomment once supported on FW side. numOfRanks;  // This one means how many ranks need to send to
    // Accumulative buffer
    SET_FIELD(cmd_coll_ops_short->reduction_opcode, 0);
    //          TODO: uncomment once num_ranks is supported on FW side.
    //            numOfRanks > 0 ?
    //            ((1 << 0) | (REDUCTION_UPSCALING_BF16 << 1) | (REDUCTION_OP_ADDITION << 5) |
    //            (REDUCTION_ROUND_HALF_TO_NEAREST_EVEN << 7)) : 0;

    /**<
     * Reduction parameters to be used when accumulating data into
     * SIB Order buffer. For the rest of the NICs it uses Reduction
     * parameters from Coll Context
     * bit [0]:   Reduction indication
     * bit [4-1]: Reduction data type
     * bit [6-5]: Reduction operation
     * bit [8-7]: Reduction rounding mode
     * bit [9]:   Reduction Operation
     */

    LOG_TRACE(
        HCL_SUBMIT,
        "Packets | serializeCollectiveRecvShortInOrderCommand  sched_arc_cmd_nic_coll_ops_t  |  command->opcode:{}, "
        " command->engine_group_type:{}, command->cmd_size:{} "
        "arc_cmd_coll_ops_recv_short_inorder_v2_t | cache_line_count:{}, sob_index:{}, "
        "local_rank_index:{}, comm_desc_index:{}, nic_opcode:{}, pool_id:{}, "
        "coll_ctxt_id:{}, siba_index:{}, sibo_index:{}, num_ranks:{}, reduction_opcode:{} on stream:{}",
        command->opcode,
        command->engine_group_type,
        command->cmd_size,
        cmd_coll_ops_short->cache_line_count,
        cmd_coll_ops_short->sob_index,
        cmd_coll_ops_short->local_rank_index,
        cmd_coll_ops_short->comm_desc_index,
        cmd_coll_ops_short->nic_opcode,
        cmd_coll_ops_short->pool_id,
        cmd_coll_ops_short->coll_ctxt_id,
        cmd_coll_ops_short->siba_index,
        cmd_coll_ops_short->sibo_index,
        cmd_coll_ops_short->num_ranks,
        cmd_coll_ops_short->reduction_opcode,
         *(scalStream.getStreamName()));
}

void SchedArcCommandsGaudi2::serializeCollectiveSendLongCommand(hcl::ScalStreamBase& scalStream,
                                                                unsigned             collectiveContextIndex,
                                                                unsigned             commDescIndex,
                                                                bool                 isSend,
                                                                bool                 hasBufferSize,
                                                                uint32_t             bufferSize,
                                                                unsigned             syncObjectAddressIndex,
                                                                bool                 force_remote_rank_offset,
                                                                uint32_t             cacheLineCount,
                                                                uint32_t             cacheLineRemainder,
                                                                uint8_t              elementRemainder,
                                                                uint64_t             address,
                                                                bool                 notifyRndvAck,
                                                                bool                 waitForRndvAcks)
{
    size_t                                dwords = 4;
    struct g2fw::arc_cmd_coll_ops_long_t* cmd_coll_ops_long;

    if (hasBufferSize)
    {
        dwords++;
    }
    g2fw::sched_arc_cmd_nic_coll_ops_t* command =
        reinterpret_cast<g2fw::sched_arc_cmd_nic_coll_ops_t*>(scalStream.getNextPtr(dwords * sizeof(uint32_t)));
    memset(command, 0, dwords * sizeof(uint32_t));

    SET_FIELD(command->opcode,
              (isSend ? (int)g2fw::SCHED_SCALEUP_SEND_ARC_CMD_NIC_COLL_OPS
                      : (int)g2fw::SCHED_SCALEUP_RECV_ARC_CMD_NIC_COLL_OPS));
    SET_FIELD(command->engine_group_type,
              (isSend ? (uint32_t)SCAL_NIC_SEND_SCALE_UP_GROUP : (uint32_t)SCAL_NIC_RECEIVE_SCALE_UP_GROUP));
    SET_FIELD(command->cmd_size, (dwords * sizeof(uint32_t)));

    cmd_coll_ops_long = (struct g2fw::arc_cmd_coll_ops_long_t*)&command->cmd_coll_ops_long;
    SET_FIELD(cmd_coll_ops_long->cache_line_count, cacheLineCount);
    SET_FIELD(cmd_coll_ops_long->cache_line_remainder, cacheLineRemainder);
    SET_FIELD(cmd_coll_ops_long->force_remote_rank_offset, force_remote_rank_offset);
    SET_FIELD(cmd_coll_ops_long->element_remainder, elementRemainder);
    SET_FIELD(cmd_coll_ops_long->sob_index, syncObjectAddressIndex);
    SET_FIELD(cmd_coll_ops_long->has_size, hasBufferSize);
    SET_FIELD(cmd_coll_ops_long->notify_rndv_ack, notifyRndvAck);
    SET_FIELD(cmd_coll_ops_long->wait_for_rndv_acks, waitForRndvAcks);
    SET_FIELD(cmd_coll_ops_long->coll_ctxt_id, collectiveContextIndex);
    SET_FIELD(cmd_coll_ops_long->nic_opcode, g2fw::NIC_CMD_COLL_OPS_LONG);
    SET_FIELD(cmd_coll_ops_long->comm_desc_index, commDescIndex);
    SET_FIELD(cmd_coll_ops_long->buffer_addr_lsb, (address & 0xffffffff));  // 32 bits
    SET_FIELD(cmd_coll_ops_long->addr_msb, ((address >> 32) & 0x1fff));     // 13 bits

    if (hasBufferSize)
    {
        std::memcpy(&cmd_coll_ops_long->buffer_size, &bufferSize, sizeof(uint32_t));
    }

    LOG_TRACE(HCL_SUBMIT,
              "Packets | serializeCollectiveSendLongCommand sched_arc_cmd_nic_coll_ops_t  |  command->opcode:{}, "
              " command->engine_group_type:{}, command->cmd_size:{}, "
              " cache_line_count:{}, cache_line_remainder:{}, element_remainder:{}, "
              " sob_index:{}, has_size:{}, notify_rndv_ack:{}, wait_for_rndv_acks:{} coll_ctxt_id:{} nic_opcode:{}, "
              " comm_desc_index:{}, buffer_addr_lsb:0x{:x}, addr_msb:0x{:x} buffer_size:{} on stream:{}",
              command->opcode,
              command->engine_group_type,
              command->cmd_size,
              cmd_coll_ops_long->cache_line_count,
              cmd_coll_ops_long->cache_line_remainder,
              cmd_coll_ops_long->element_remainder,
              cmd_coll_ops_long->sob_index,
              cmd_coll_ops_long->has_size,
              cmd_coll_ops_long->notify_rndv_ack,
              cmd_coll_ops_long->wait_for_rndv_acks,
              cmd_coll_ops_long->coll_ctxt_id,
              cmd_coll_ops_long->nic_opcode,
              cmd_coll_ops_long->comm_desc_index,
              cmd_coll_ops_long->buffer_addr_lsb,
              cmd_coll_ops_long->addr_msb,
              bufferSize,
               *(scalStream.getStreamName()));
}

void SchedArcCommandsGaudi2::serializeCollectiveSendScaleOutCommand(hcl::ScalStreamBase&           scalStream,
                                                                    unsigned                       collectiveContextIndex,
                                                                    bool                           isSend,
                                                                    bool                           hasBufferSize,
                                                                    uint32_t                       bufferSize,
                                                                    unsigned                       syncObjectAddressIndex,
                                                                    uint32_t                       cacheLineCount,
                                                                    uint32_t                       cacheLineRemainder,
                                                                    uint8_t                        elementRemainder,
                                                                    uint64_t                       address,
                                                                    ContextManager::ContextValues& contextValues,
                                                                    std::array<uint16_t, 4>&       qpnDesc,
                                                                    bool                           notifyRndvAck,
                                                                    bool                           waitForRndvAcks)
{
    size_t dwordsNumForUpdate = contextValues.second;
    size_t dwords             = 1 + 3 + (hasBufferSize ? 1 : 0) + dwordsNumForUpdate + 2;
    size_t size               = dwords * sizeof(uint32_t);

    g2fw::sched_arc_cmd_nic_coll_ops_scaleout_t* command =
        reinterpret_cast<g2fw::sched_arc_cmd_nic_coll_ops_scaleout_t*>(scalStream.getNextPtr(size));
    memset(command, 0, size);
    SET_FIELD(command->opcode,
              (isSend ? (int)g2fw::SCHED_SCALEOUT_SEND_ARC_CMD_NIC_COLL_OPS
                      : (int)g2fw::SCHED_SCALEOUT_RECV_ARC_CMD_NIC_COLL_OPS));
    SET_FIELD(command->engine_group_type,
              (isSend ? (uint32_t)SCAL_NIC_SEND_SCALE_OUT_GROUP : (uint32_t)SCAL_NIC_RECEIVE_SCALE_OUT_GROUP));
    SET_FIELD(command->cmd_size, size);

    // Amount of data in multiples of cache line size that each NIC needs to send.
    SET_FIELD(command->cmd_coll_ops_scaleout.cache_line_count, cacheLineCount);

    // Remainder to be subtracted from cache_line_count value to calculate the size of the data to be sent by NIC.
    SET_FIELD(command->cmd_coll_ops_scaleout.cache_line_remainder, cacheLineRemainder);

    // remainder in terms of number of elements when the data is not integer multiple of cache line. Typically used by
    // the last nic in the sub group.
    SET_FIELD(command->cmd_coll_ops_scaleout.element_remainder, elementRemainder);

    // Sync Object to be used by this command
    SET_FIELD(command->cmd_coll_ops_scaleout.sob_index, syncObjectAddressIndex);

    // Flag to indicate if the size field is present
    SET_FIELD(command->cmd_coll_ops_scaleout.has_size, hasBufferSize);

    // Collective context ID to be used
    SET_FIELD(command->cmd_coll_ops_scaleout.coll_ctxt_id, collectiveContextIndex);

    // NIC opcode
    SET_FIELD(command->cmd_coll_ops_scaleout.nic_opcode, 0);  // NIC_SCALEOUT_CMD_COLL_OPS

    // LSB address to send to. MSB is taken from collective context
    SET_FIELD(command->cmd_coll_ops_scaleout.buffer_addr_lsb, (address & 0xffffffff));

    // Count of QPN descriptors received as a part of command, incremented in the loop below
    SET_FIELD(command->cmd_coll_ops_scaleout.qpn_desc_count, 1);

    // we use the iterator pointer to deal with the zero-length arrays in the struct (all have the same address)
    uint32_t* dwordIter = (uint32_t*)&command->cmd_coll_ops_scaleout + 3;

    // iter->cmd_coll_ops_scaleout.buffer_size
    if (hasBufferSize)
    {
        // send_address’s size in bytes. The ARCs should not try to access a buffer past send_address + buffer_size.
        // If not present, it’s inferred to be cell_size * pod_size.
        std::memcpy(&command->cmd_coll_ops_scaleout.buffer_size, &bufferSize, sizeof(uint32_t));
        dwordIter++;
    }

    // Number of dwords to be updated by this bitmask
    SET_FIELD(command->cmd_coll_ops_scaleout.num_dwords_bitmask, dwordsNumForUpdate);

    LOG_INFO(HCL_SUBMIT,
             "Serializing a scaleout collective context update for collectiveContext = {}, ({} dwords):",
             collectiveContextIndex,
             dwordsNumForUpdate);
    // iter->cmd_coll_ops_scaleout.dword_value

    for (size_t dword = 0; dword < contextValues.first.size(); dword++)
    {
        ContextManager::ContextValueUpdater& contextValueUpdater = (contextValues.first).at(dword);
        if (!contextValueUpdater.needUpdate) continue;

        LOG_DEBUG(HCL_SUBMIT, "    DW{} updating to value 0x{:x}", (eDWords)dword, contextValueUpdater.value);
        // Bitmap of the DWORDs, which needs to be updated in the collective ctxt
        // Bit 0 to 4 - Used for updating dwords 0 to 4 of collective ctxt
        SET_FIELD(command->cmd_coll_ops_scaleout.update_bitmask,
                  command->cmd_coll_ops_scaleout.update_bitmask | (1 << (uint8_t)dword));
        *dwordIter = contextValueUpdater.value;
        dwordIter++;
    }

    // QPs to be used to communicate with a remote rank
    // for each remote rank we use three QPs, one for each nic
    uint16_t* innerIter = (uint16_t*)dwordIter;

    // remote_scaleout_index;
    *innerIter = qpnDesc[0];
    innerIter++;

    // qpn_subnic_0;
    *innerIter = qpnDesc[1];
    innerIter++;

    // qpn_subnic_1
    *innerIter = qpnDesc[2];
    innerIter++;

    // qpn_subnic_2
    *innerIter = qpnDesc[3];
    innerIter++;
    dwordIter += 2;

    SET_FIELD(command->cmd_coll_ops_scaleout.notify_rndv_ack, notifyRndvAck);
    SET_FIELD(command->cmd_coll_ops_scaleout.wait_for_rndv_acks, waitForRndvAcks);

    LOG_TRACE(HCL_SUBMIT,
              "Packets | serializeCollectiveSendScaleOutCommand sched_arc_cmd_nic_coll_ops_scaleout_t  |  "
              "command->opcode:{}, "
              " command->engine_group_type:{}, command->cmd_size:{}, qpn_desc_count:{}, "
              " cache_line_count:{}, cache_line_remainder:{}, element_remainder:{}, "
              " sob_index:{}, has_size:{}, notify_rndv_ack:{}, wait_for_rndv_acks:{} coll_ctxt_id:{} nic_opcode:{}, "
              " buffer_addr_lsb:0x{:x}, buffer_size:{}, "
              " num_dwords_bitmask:{} update_bitmask:0x{:x} on stream:{}",
              command->opcode,
              command->engine_group_type,
              command->cmd_size,
              command->cmd_coll_ops_scaleout.qpn_desc_count,
              command->cmd_coll_ops_scaleout.cache_line_count,
              command->cmd_coll_ops_scaleout.cache_line_remainder,
              command->cmd_coll_ops_scaleout.element_remainder,
              command->cmd_coll_ops_scaleout.sob_index,
              command->cmd_coll_ops_scaleout.has_size,
              command->cmd_coll_ops_scaleout.notify_rndv_ack,
              command->cmd_coll_ops_scaleout.wait_for_rndv_acks,
              command->cmd_coll_ops_scaleout.coll_ctxt_id,
              command->cmd_coll_ops_scaleout.nic_opcode,
              command->cmd_coll_ops_scaleout.buffer_addr_lsb,
              bufferSize,
              command->cmd_coll_ops_scaleout.num_dwords_bitmask,
              command->cmd_coll_ops_scaleout.update_bitmask,
               *(scalStream.getStreamName()));
}

void SchedArcCommandsGaudi2::serializeUserSendCommand(std::vector<uint32_t>& out,
                                                      unsigned               collectiveContextIndex,
                                                      unsigned               commDescIndex,
                                                      unsigned               syncObjectAddressIndex,
                                                      uint32_t               cacheLineCount,
                                                      uint32_t               cacheLineRemainder,
                                                      uint8_t                elementRemainder,
                                                      hcclDataType_t         dataType,
                                                      uint64_t               address,
                                                      bool                   isLastInGroup,
                                                      bool                   notifyRndvAck,
                                                      bool                   waitForRndvAcks)
{
    g2fw::arc_cmd_send_recv_short_t command = {0};

    SET_FIELD(command.nic_opcode, 0x01);  // NIC_CMD_SEND_RECV
    SET_FIELD(command.coll_ctxt_id, collectiveContextIndex);
    SET_FIELD(command.sob_index, syncObjectAddressIndex);
    SET_FIELD(command.sob_increment, (isLastInGroup ? 1 : 0));
    SET_FIELD(command.addr_lsb, (address & 0xffffffff));
    SET_FIELD(command.addr_msb, ((address >> 32) & 0xffffff));
    SET_FIELD(command.comm_desc_index, commDescIndex);
    SET_FIELD(command.cache_line_count, cacheLineCount);
    SET_FIELD(command.cache_line_remainder, cacheLineRemainder);
    SET_FIELD(command.element_remainder, elementRemainder);
    SET_FIELD(command.notify_rndv_ack, notifyRndvAck);
    SET_FIELD(command.wait_for_rndv_acks, waitForRndvAcks);

    LOG_TRACE(HCL_SUBMIT,
              "Packets | serializeUserSendCommand arc_cmd_send_recv_short_t  |  nic_opcode:{}, "
              "coll_ctxt_id = {}, sob_index = {}, sob_increment = {}, addr_msb = 0x{:x}"
              "addr_lsb = 0x{:x}, commDescIndex = {}"
              "cache_line_count = 0x{:x} cache_line_remainder:0x{:x}, element_remainder :{}"
              "notifyRndvAck = {}, waitForRndvAcks = {}",
              command.nic_opcode,
              command.coll_ctxt_id,
              command.sob_index,
              command.sob_increment,
              command.addr_msb,
              command.addr_lsb,
              command.comm_desc_index,
              command.cache_line_count,
              command.cache_line_remainder,
              command.element_remainder,
              command.notify_rndv_ack,
              command.wait_for_rndv_acks);

    switch (dataTypeSizeInBytes(dataType))
    {
        case 1:
            command.datatype_size = 0;
            break;
        case 2:
            command.datatype_size = 1;
            break;
        case 4:
            command.datatype_size = 2;
            break;
        default:
            VERIFY(false, "Invalid datatype {}", dataType);
            break;
    }

    out.clear();
    out.resize(sizeof(g2fw::arc_cmd_send_recv_short_t) / sizeof(uint32_t));
    memcpy(out.data(), &command, sizeof(command));
}

void SchedArcCommandsGaudi2::serializeNicNopCommand(pRecordWithMetadata& record,
                                                    unsigned             collectiveContextIndex,
                                                    uint32_t             dupMask,
                                                    size_t               requiredCredits,
                                                    unsigned             syncObjectAddressIndex,
                                                    bool                 incSOB)
{
    g2fw::arc_cmd_nic_send_recv_nop_t command = {0};

    SET_FIELD(command.nic_opcode, g2fw::NIC_CMD_SEND_RECV_NOP);
    SET_FIELD(command.coll_ctxt_id, collectiveContextIndex);
    SET_FIELD(command.sob_index, syncObjectAddressIndex);
    SET_FIELD(command.sob_increment, (incSOB ? 1 : 0));
    SET_FIELD(command.queue_credits_bytes, requiredCredits);

    record->graphIndex = -1;
    record->next       = nullptr;

    record->data.dup_mask           = dupMask;
    record->data.is_last_config     = 1;
    record->data.is_nop             = 1;
    record->data.num_payload_dwords = 0;
    memcpy(&record->data.payload0, &command, sizeof(command));
}

size_t SchedArcCommandsGaudi2::recordsSizeInDwords(std::vector<pRecordWithMetadata>& records)
{
    size_t size = 0;
    for (pRecordWithMetadata& record : records)
    {
        size += record->data.num_payload_dwords == 0 ? 2 : 3;
    }
    return size;
}

void SchedArcCommandsGaudi2::serializeNicPassthroughCommand(hcl::ScalStreamBase&              scalStream,
                                                            std::vector<pRecordWithMetadata>& records,
                                                            size_t                            credits,
                                                            bool                              isSend)
{
    size_t dwords = 1 + recordsSizeInDwords(records);
    size_t size   = dwords * sizeof(uint32_t);

    g2fw::sched_arc_cmd_nic_passthrough_t* command =
        reinterpret_cast<g2fw::sched_arc_cmd_nic_passthrough_t*>(scalStream.getNextPtr(size));
    memset(command, 0, size);

    command->opcode = isSend ? (int)g2fw::SCHED_SCALEUP_SEND_ARC_CMD_NIC_PASSTHROUGH
                             : (int)g2fw::SCHED_SCALEUP_RECV_ARC_CMD_NIC_PASSTHROUGH;
    command->engine_group_type =
        isSend ? (uint32_t)SCAL_NIC_SEND_SCALE_UP_GROUP : (uint32_t)SCAL_NIC_RECEIVE_SCALE_UP_GROUP;
    command->cmd_dw_size                = dwords;
    command->required_q_credits_inbytes = credits;

    VERIFY(records.size() > 0, "Tried to serialize NIC_PASSTHROUGH command with no records!");
    LOG_INFO(HCL,
             "Adding {} records to nic passthrough command (size = {} dwords, credits = {}), "
             "on stream:{}",
             records.size(),
             dwords,
             credits,
              *(scalStream.getStreamName()));

    uint32_t* ptr = (uint32_t*)command->passthrough_data;
    for (size_t i = 0; i < records.size(); i++)
    {
        VERIFY(records[i] != nullptr);

        pRecordWithMetadata record = std::move(records[i]);  // record will be destroyed after scope
        LOG_DEBUG(HCL, "    {}: payload0: 0x{:0>8x}\t(mask=0x{:x})", i, record->data.payload0, record->data.dup_mask);
        if (record->data.num_payload_dwords == 1)
        {
            LOG_DEBUG(HCL,
                      "    {}: payload1: 0x{:0>8x}\t(mask=0x{:x})",
                      i,
                      record->data.payload1[0],
                      record->data.dup_mask);
        }

        if (i == records.size() - 1)
        {
            record->data.is_last_config = 1;
        }

        size_t recordSizeDwords = ((record->data.num_payload_dwords == 1) ? 3 : 2);
        memcpy((void*)ptr, (void*)&record->data, recordSizeDwords * sizeof(uint32_t));
        ptr += recordSizeDwords;
    }
}