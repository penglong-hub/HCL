#pragma once
#include "platform/gen2_arch_common/device_buffer_manager.h"  // for RR_SCALEOUT_FACTOR

enum class SignalEvent
{
    FORCE_ORDER = 0,
    EDMA_CAST_UP,
    EDMA_CAST_DOWN,
    EDMA_CAST_DOWN_FOR_SCALEOUT,
    EDMA_MEMCOPY,
    EDMA_MEMCOPY_FOR_SCALEOUT,
    EDMA_BATCH,
    EDMA_BATCH_SCALEOUT,
    EDMA_MEMCOPY_RR,
    EDMA_MEMCOPY_RR_LAST_BOX,
    EDMA_MEMCOPY_GDR,
    EDMA_MEMSET,
    SCALEUP_SEND,  // cost = signalsSingleOp (21 most likely)
    SCALEUP_RECV,  // cost = signalsSingleOp (21 most likely)
    SCALEOUT_SEND,
    SCALEOUT_RECV,
    HNIC_SCALEOUT_SEND,
    HNIC_SCALEOUT_RECV,
    HNIC_PDMA,
    RR_SIGNAL_TO_LONGTERM,
    RR_SIGNAL_TO_CG,
    SIGNAL_EVENT_MAX
};

enum class WaitMethod
{
    GPSO_LONGTERM = 0,
    GPSO_LONGTERM_1,
    GPSO_LONGTERM_2,
    GPSO_LONGTERM_3,
    GPSO_LONGTERM_4,
    GPSO_LONGTERM_5,
    GPSO_LONGTERM_6,
    GPSO_LONGTERM_7,
    GPSO_LONGTERM_8,
    GPSO_0,
    GPSO_1,
    EXTERNAL_CG_SO,
    INTERNAL_CG_SO,
    WAIT_METHOD_MAX
};

enum class GpsoPool
{
    GPSO_LONGTERM,
    GPSO_LTU,
    GPSO_0,
    GPSO_1,
    COUNT
};

inline bool isLongTerm(WaitMethod waitMethod)
{
    return (unsigned)waitMethod <= (unsigned)WaitMethod::GPSO_LONGTERM_8;
}

inline GpsoPool waitMethodToGpsoPool(WaitMethod waitMethod)
{
    return isLongTerm(waitMethod) ? GpsoPool::GPSO_LONGTERM
                                  : (waitMethod == WaitMethod::GPSO_0 ? GpsoPool::GPSO_0 : GpsoPool::GPSO_1);
}

const unsigned WAIT_PHASE_MAX = 128;
const uint64_t MIN_PHASES     = 8;
typedef int    WaitPhase;

enum class WaitEvent
{
    GENERAL_COMPLETION_EVENT = 0,
    GENERAL_INTERNAL_COMPLETION_EVENT,

    // Scale-Out First = AG, Broadcast, Simple Broadcast
    SO_FIRST_SU_SEND_WAIT_FOR_SO_RECV,

    COMPLEX_BCAST_SO_SEND_WAIT_FOR_SO_RECV,
    COMPLEX_BCAST_AG_SU_SEND_WAIT_FOR_SCATTER_RECV,
    COMPLEX_BCAST_SO_SEND_AND_AG_SU_WAIT_FOR_SU_RECV,
    COMPLEX_BCAST_SO_SEND_AND_AG_SU_WAIT_FOR_SO_RECV,

    GDR_MEMCPY_WAIT_FOR_HNIC_RECV,

    RR_DMA_WAIT_FOR_RECV,
    RR_DMA_WAIT_FOR_SU_RECV,
    RR_FINAL_DMA_WAIT_FOR_EDMA,
    RR_SCALEOUT_SEND_WAIT_FOR_DMA,
    RR_DMA_BATCH_WAIT_FOR_SCALEOUT_RECV,
    RR_FINAL_SCALEOUT_DMA_WAIT_FOR_DMA_BATCH,
    RR_DMA_BATCH_WAIT_FOR_GDR_MEMCPY,
    RR_REDUCE_FINAL_SCALEOUT_DMA_WAIT_FOR_DMA_BATCH,
    RR_RS_SO_WAIT_FOR_ALL_RECV,
    RR_GATHER_OPS_WAIT_FOR_RS,
    RR_FIRST_BOX_FINAL_SIGNAL_WAIT_FOR_GPSO,
    RR_LTU_SIGNALING_WAIT_FOR_SCALEOUT_SEND,
    HNIC_SIGNAL_SPLIT_WAIT_FOR_GDR_MEMCPY,
    HNIC_SIGNAL_SPLIT_WAIT_FOR_PDMA,
    HNIC_SCALEOUT_RECV_PDMA_WAIT_FOR_RECV,
    ALL2ALL_SO_SEND_WAIT_FOR_RECV,

    // must be last
    // RR events range from base to max
    RR_RS_SO_RECV_WAIT_FOR_PREV_RECV_BASE,

    WAIT_EVENT_MAX = (RR_RS_SO_RECV_WAIT_FOR_PREV_RECV_BASE + RR_SCALEOUT_FACTOR),
};