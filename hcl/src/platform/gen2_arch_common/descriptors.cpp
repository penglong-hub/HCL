#include "platform/gen2_arch_common/descriptors.h"

#include <vector>

#include "hcl_api_types.h"  // for HCL_Rank
#include "platform/gen2_arch_common/hcl_collective_routines.h"
#include "platform/gen2_arch_common/hcl_packets.h"  // for HostSchedCommandsGen2Arch
#include "platform/gen2_arch_common/scaleout_provider.h"
#include "platform/gen2_arch_common/host_stream.h"
#include "infra/scal/gen2_arch_common/scal_utils.h"
#include "infra/scal/gen2_arch_common/scal_manager.h"
#include "platform/gaudi2/hcl_packets.h"
#include "hcl_utils.h"  // for LOG_HCL_DEBUG, UNUSED
#include "platform/gen2_arch_common/collective_states.h"
#include "platform/gen2_arch_common/host_buffer_manager.h"  // for HostBufferManager
#include "hcl_dynamic_communicator.h"                       // for HclDynamicCommunicator
#include "infra/scal/gen2_arch_common/scal_stream.h"        // for hcl::ScalStream
#include "llvm/small_vector.h"                              // for SmallVector
#include "platform/gen2_arch_common/types.h"                // for sob_info
#include "hcl_public_streams.h"                             // for syncInfo
#include "hcl_math_utils.h"
#include "platform/gen2_arch_common/signals/manager.h"
#include "platform/gen2_arch_common/hcl_device.h"  // for HclDeviceGen2Arch

Descriptor::Descriptor(HclCollectiveRoutinesGen2Arch& collectiveRoutines,
                       ScaleoutProvider&              scaleoutProvider,
                       hcl::ScalStream&               currentStream,
                       int                            archStreamIdx,
                       unsigned                       uarchStreamIdx,
                       unsigned                       schedIdx)
: m_collectiveRoutines(collectiveRoutines),
  m_scaleoutProvider(scaleoutProvider),
  m_currentStream(currentStream),
  m_archStreamIdx(archStreamIdx),
  m_uarchStreamIdx(uarchStreamIdx),
  m_schedIdx(schedIdx)
{
}

BarrierArbitratorDescriptor::BarrierArbitratorDescriptor(HclCollectiveRoutinesGen2Arch& collectiveRoutines,
                                                         ScaleoutProvider&              scaleoutProvider,
                                                         hcl::ScalStream&               currentStream,
                                                         hcl::ScalStream&               arbitratorStream,
                                                         int                            archStreamIdx,
                                                         unsigned                       uarchStreamIdx,
                                                         unsigned                       schedIdx,
                                                         unsigned                       requiredCredits,
                                                         hcl::syncInfo&                 longSo)
: Descriptor(collectiveRoutines, scaleoutProvider, currentStream, archStreamIdx, uarchStreamIdx, schedIdx),
  m_arbitratorStream(arbitratorStream),
  m_requiredCredits(requiredCredits),
  m_longSo(longSo)
{
}

void BarrierArbitratorDescriptor::run(SliceState& sliceState)
{
    m_collectiveRoutines.m_deviceController.waitForBarrierArm(m_currentStream);

    llvm_vecsmall::SmallVector<unsigned, MAX_STREAM_TO_INC> activeStreams = {m_uarchStreamIdx};
    m_collectiveRoutines.m_deviceController.addBarrierArm(m_arbitratorStream, false, m_requiredCredits, activeStreams);
}

void BarrierArbitratorDescriptor::run(NonCollectiveState& nonCollectiveState)
{
    LOG_HCL_TRACE(HCL,
                  "(NonCollectiveState): m_schedIdx={}, m_uarchStreamIdx={}, remoteRank={}, m_isSend={}",
                  this->m_schedIdx,
                  this->m_uarchStreamIdx,
                  nonCollectiveState.m_remoteRank,
                  nonCollectiveState.m_isSend);

    m_collectiveRoutines.m_deviceController.waitForBarrierArm(m_currentStream);

    llvm_vecsmall::SmallVector<unsigned, MAX_STREAM_TO_INC> activeStreams = {m_uarchStreamIdx};
    m_collectiveRoutines.m_deviceController.addBarrierArm(m_arbitratorStream, false, m_requiredCredits, activeStreams);
}

NativeScaleoutDescriptor::NativeScaleoutDescriptor(HclCollectiveRoutinesGen2Arch& collectiveRoutines,
                                                   ScaleoutProvider&              scaleoutProvider,
                                                   hcl::ScalStream&               currentStream,
                                                   int                            archStreamIdx,
                                                   unsigned                       uarchStreamIdx,
                                                   unsigned                       schedIdx)
: Descriptor(collectiveRoutines, scaleoutProvider, currentStream, archStreamIdx, uarchStreamIdx, schedIdx)
{
}

void NativeScaleoutDescriptor::run(SliceState& sliceState)
{
    const unsigned collectiveContextIndex = m_archStreamIdx * 2 + m_uarchStreamIdx;

    // prepare next remoteRank to receive data from
    HCL_Rank remoteRank = sliceState.m_dynamicComm.getPodToRankMap()[sliceState.m_boxNumInfo.m_boxNum];
    int      remoteRankToRsi =
        m_collectiveRoutines.getRemoteRankToRsi(sliceState, sliceState.m_isSend, remoteRank, m_uarchStreamIdx == 1);

    hcclDataType_t dataType = (!sliceState.m_isSend && sliceState.m_currentOp != eHCLAllGather &&
                               sliceState.m_currentOp != eHCLGather && sliceState.m_16BitReduction)
                                  ? hcclFloat32
                                  : sliceState.m_dataType;

    WqeWraparoundBits wraparoundBits = {false, false};
    bool              doReduction    = false;
    if  (!sliceState.m_isSend)
    {
        wraparoundBits = m_collectiveRoutines.getWraparoundBits(sliceState.m_dynamicComm,
                                                                sliceState.m_boxNumInfo.m_boxNum,
                                                                m_uarchStreamIdx == 0 ? QpType::ScaleOutReduceScatter
                                                                                      : QpType::ScaleOutAllGather);
    }
    else
    {
        unsigned boxIter =
            mod(sliceState.m_boxNumInfo.m_boxNum + sliceState.m_boxIterations - sliceState.m_dynamicComm.getMyPod(),
                sliceState.m_boxIterations);
        doReduction = sliceState.m_isReductionCollective && boxIter >= sliceState.m_reproScaleoutBuffersAmount;
    }

    LOG_TRACE(HCL_ECR,
              "Counts for Scaleout {}: op {}, box {}, slice {}, cellCount {}, stride {}, count {},"
              "address 0x{:X}, qpSet= {}, doReduction {}",
              sliceState.m_isSend,
              sliceState.m_currentOp,
              sliceState.m_boxNumInfo.m_boxNum,
              sliceState.m_sliceIter,
              sliceState.m_execution.m_cellCount,
              sliceState.m_execution.m_strideCount,
              sliceState.m_execution.m_deviceCount,
              sliceState.m_execution.m_deviceAddress,
              sliceState.getQpSet(),
              doReduction);

    ScaleOutCollectiveOp op {sliceState.m_dynamicComm.getMyPod(),
                             remoteRankToRsi,
                             sliceState.m_dynamicComm,
                             sliceState.m_currentOp,
                             sliceState.m_reduceOp,
                             collectiveContextIndex,
                             sliceState.m_execution.m_completionSoAddr,
                             sliceState.m_isSend,
                             sliceState.m_isReductionCollective,
                             sliceState.m_execution.m_deviceAddress,
                             sliceState.m_execution.m_deviceCount,
                             false,
                             dataType,
                             sliceState.m_execution.m_cellCount,
                             sliceState.m_execution.m_strideCount,
                             remoteRank,
                             sliceState.m_all2allIter,
                             wraparoundBits.notify_rndv_ack,
                             wraparoundBits.wait_for_rndv_acks,
                             doReduction,
                             sliceState.getQpSet()};

    m_collectiveRoutines.createScaleOutCollectiveOp(m_currentStream, op);
}

NativeNonCollectiveScaleoutDescriptor::NativeNonCollectiveScaleoutDescriptor(
    HclCollectiveRoutinesGen2Arch& collectiveRoutines,
    ScaleoutProvider&              scaleoutProvider,
    hcl::ScalStream&               currentStream,
    int                            archStreamIdx,
    unsigned                       uarchStreamIdx,
    unsigned                       schedIdx,
    const WqeWraparoundBits&       wraparoundBits)
: Descriptor(collectiveRoutines, scaleoutProvider, currentStream, archStreamIdx, uarchStreamIdx, schedIdx),
  m_wraparoundBits(wraparoundBits)
{
}

void NativeNonCollectiveScaleoutDescriptor::run(NonCollectiveState& nonCollectiveState)
{
    // prepare next remoteRank to receive data from
    const hcclDataType_t dataType   = nonCollectiveState.m_dataType;
    HCL_Rank             remoteRank = nonCollectiveState.m_remoteRank;

    ScaleOutCollectiveOp op {
        nonCollectiveState.m_dynamicComm.getMyPod(),  // myPod
        0,  // remoteRanksToRSI - offset of where to get data from for each remote rank. In case of send its always 0
        nonCollectiveState.m_dynamicComm,                   // comm
        eHCLNoCollective,                                   // collectiveOp
        hcclOpNone,                                         // reductionOp
        0,                                                  // collectiveContextIndex
        nonCollectiveState.m_execution.m_completionSoAddr,  // soAddress
        nonCollectiveState.m_isSend,                        // isSend
        false,                                              // bf16Reduction
        nonCollectiveState.m_execution.m_deviceAddress,     // baseAddress
        nonCollectiveState.m_execution.m_deviceCount,       // count
        false,                                              // hasBufferSize
        dataType,
        nonCollectiveState.m_execution.m_deviceCount,  // cellCount
        0,                                             // strideCount
        remoteRank,                                    // remoteRank
        0,                                             // remoteRankIteration
        m_wraparoundBits.notify_rndv_ack,              // notifyRndvAck
        m_wraparoundBits.wait_for_rndv_acks,           // waitForRndvAcks
        false,
        nonCollectiveState.getQpSet()};

    LOG_HCL_TRACE(
        HCL,
        "(NonCollectiveState): dataType={}, nonCollectiveState.m_remoteRank={}, nonCollectiveState.getQpSet()={}",
        dataType,
        nonCollectiveState.m_remoteRank,
        nonCollectiveState.getQpSet());
    m_collectiveRoutines.createScaleOutCollectiveOp(m_currentStream, op);
}

static void libfabricCompCallback(OfiCompCallbackParams* compParams)
{
    compParams->device->getScalManager().signalFromHost(compParams->smIdx, compParams->soIdx, compParams->value);
}

LibfabricScaleoutDescriptor::LibfabricScaleoutDescriptor(HclCollectiveRoutinesGen2Arch& collectiveRoutines,
                                                         ScaleoutProvider&              scaleoutProvider,
                                                         hcl::ScalStream&               currentStream,
                                                         int                            archStreamIdx,
                                                         unsigned                       uarchStreamIdx,
                                                         unsigned                       schedIdx,
                                                         HclCommandsGen2Arch&           commands)
: Descriptor(collectiveRoutines, scaleoutProvider, currentStream, archStreamIdx, uarchStreamIdx, schedIdx),
  m_commands(commands),
  m_utils(m_collectiveRoutines.getScalUtils())
{
    VERIFY(m_scaleoutProvider.isHostNic(), "Cannot use libfabric descriptor on a non-hostnic provider");
}

void LibfabricScaleoutDescriptor::streamAddWait(spHostStreamFifo hostStream, fence_info fence, const uint64_t srCount)
{
    LOG_HCL_TRACE(HCL,
                  "adding host fence on fenceIndex={} facading {}",
                  fence.index,
                  m_utils->printSOBInfo(fence.lbw.addr));

    HostSchedCommandsGen2Arch::serializeHostFenceCommand(hostStream, fence.index, srCount);
}

unsigned LibfabricScaleoutDescriptor::getHostUarchStreamIdx()
{
    return (GCFG_ENABLE_HNIC_MICRO_STREAMS.value() ? m_uarchStreamIdx : 0);
}

void LibfabricScaleoutDescriptor::run(SliceState& sliceState)
{
    LibfabricScaleoutProvider& provider          = dynamic_cast<LibfabricScaleoutProvider&>(m_scaleoutProvider);
    uint64_t                   hostMappedAddress = provider.getHostBufferManager(m_archStreamIdx)
                                     ->getCurrentMappedBuffer(sliceState.m_isSend ? HNIC_SEND_POOL : HNIC_RECV_POOL);
    uint64_t hostAddress = provider.getHostBufferManager(m_archStreamIdx)
                               ->getCurrentBuffer(sliceState.m_isSend ? HNIC_SEND_POOL : HNIC_RECV_POOL);
    HCL_Rank remoteRank         = sliceState.m_dynamicComm.getPodToRankMap()[sliceState.m_boxNumInfo.m_boxNum];
    unsigned hostUarchStreamIdx = getHostUarchStreamIdx();

    uint32_t remoteRankIteration = sliceState.m_all2allIter;
    uint32_t dataSize            = sliceState.m_execution.m_cellCount * sliceState.m_dataTypeSizeInBytes;
    uint32_t offsetForPdmaDown   = 0;
    uint32_t offsetForPdmaUp     = 0;

    if (sliceState.m_collectiveOp == eHCLAll2All && sliceState.m_all2allIterations > 1)
    {
        if (sliceState.m_isSlicing)
        {
            // PDMA-UP from continues buffer and PDMA-DOWN using stride
            offsetForPdmaDown =
                sliceState.m_execution.m_strideCount * remoteRankIteration * sliceState.m_dataTypeSizeInBytes;
            offsetForPdmaUp = dataSize * remoteRankIteration;
        }
        else
        {
            // PDMA-UP and PDMA-DOWN from/yo continues buffer
            offsetForPdmaDown =
                sliceState.m_all2allIterStrideCount * remoteRankIteration * sliceState.m_dataTypeSizeInBytes;
            offsetForPdmaUp = offsetForPdmaDown;
        }
    }

    if (sliceState.m_isSend)
    {
        HostStream* sendHostStream = provider.m_hostStreamVec[m_archStreamIdx][hostUarchStreamIdx][HOST_STREAM_SEND];
        HostStream* waitForCompHostStream =
            provider.m_hostStreamVec[m_archStreamIdx][hostUarchStreamIdx][HOST_STREAM_WAIT_FOR_SEND_COMP];

        fence_info fence = sliceState.m_execution.m_scaleoutFences[0];
        LOG_HCL_TRACE(HCL,
                      "scaleout send's pdma will signal to {}; move {} bytes of data from device addr 0x{:x} to 0x{:x} "
                      "(host 0x{:x})",
                      m_utils->printSOBInfo(fence.lbw.addr),
                      dataSize,
                      sliceState.m_execution.m_deviceAddress + offsetForPdmaUp,
                      hostMappedAddress,
                      hostAddress);

        m_commands.serializePdmaCommand(m_currentStream,
                                        m_schedIdx,
                                        false,
                                        hostMappedAddress,
                                        sliceState.m_execution.m_deviceAddress + offsetForPdmaUp,
                                        dataSize,
                                        0 /* isReduction */,
                                        hcclOpNone,
                                        0 /* isCastUp*/,
                                        sliceState.m_apiId,
                                        m_archStreamIdx,
                                        sliceState.m_dataType,
                                        fence.lbw.addr);

        const uint32_t soAddr = sliceState.m_execution.m_completionSoAddr;
        const sob_info sob    = m_utils->getSOBInfo(soAddr);

        sendHostStream->incSrCount();
        OfiCompCallbackParams compParams {sob.smIdx,
                                          sob.sobId,
                                          m_collectiveRoutines.getSoConfigValue(sliceState.signalToCost(SignalEvent::HNIC_SCALEOUT_SEND), true),
                                          m_collectiveRoutines.getDevice(),
                                          libfabricCompCallback};
        HostSchedCommandsGen2Arch::serializeHostScaleOutCommandWithFence(sendHostStream->getOuterQueue(),
                                                                         sliceState.m_isSend,
                                                                         hostAddress,
                                                                         remoteRank,
                                                                         dataSize,
                                                                         sliceState.m_comm,
                                                                         fence.index,
                                                                         compParams,
                                                                         sendHostStream->getSrCount());

        LOG_HCL_TRACE(HCL, "scaleout send's completion will signal to {}", m_utils->printSOBInfo(sob));
        HostSchedCommandsGen2Arch::serializeHostWaitForCompletionCommand(waitForCompHostStream->getOuterQueue(),
                                                                         sliceState.m_comm,
                                                                         sendHostStream->getSrCount());
    }
    else
    {
        HostStream* recvHostStream = provider.m_hostStreamVec[m_archStreamIdx][hostUarchStreamIdx][HOST_STREAM_RECV];
        HostStream* waitForCompHostStream =
            provider.m_hostStreamVec[m_archStreamIdx][hostUarchStreamIdx][HOST_STREAM_WAIT_FOR_RECV_COMP];

        fence_info fence = sliceState.m_execution.m_scaleoutFences[0];
        m_commands.serializeLbwWriteCommand(m_currentStream, m_schedIdx, fence.lbw.addr, fence.lbw.data);

        sob_info sob = sliceState.m_execution.m_scaleoutInternalSOBs[0];

        recvHostStream->incSrCount();
        OfiCompCallbackParams compParams {
            sob.smIdx,
            sob.sobId,
            m_collectiveRoutines.getSoConfigValue(sliceState.signalToCost(SignalEvent::HNIC_SCALEOUT_RECV), true),
            m_collectiveRoutines.getDevice(),
            libfabricCompCallback};
        HostSchedCommandsGen2Arch::serializeHostScaleOutCommandWithFence(recvHostStream->getOuterQueue(),
                                                                         sliceState.m_isSend,
                                                                         hostAddress,
                                                                         remoteRank,
                                                                         dataSize,
                                                                         sliceState.m_comm,
                                                                         fence.index,
                                                                         compParams,
                                                                         recvHostStream->getSrCount());

        LOG_HCL_TRACE(HCL, "scaleout recv's completion will signal to {}", m_utils->printSOBInfo(sob));
        HostSchedCommandsGen2Arch::serializeHostWaitForCompletionCommand(waitForCompHostStream->getOuterQueue(),
                                                                         sliceState.m_comm,
                                                                         recvHostStream->getSrCount());
        m_collectiveRoutines.getSignalsManager()->dequeueSoAddress(SignalEvent::HNIC_SCALEOUT_RECV);

        m_collectiveRoutines.streamAddSingleWaitIfNeeded(m_currentStream,
                                                         {WaitEvent::HNIC_SCALEOUT_RECV_PDMA_WAIT_FOR_RECV});
        uint64_t soAddr = sliceState.m_execution.m_completionSoAddr;

        LOG_HCL_TRACE(HCL,
                      "scaleout recv's pdma will signal to {}; move {} bytes of data from addr 0x{:x} (host 0x{:x}) to "
                      "device addr 0x{:x}",
                      m_utils->printSOBInfo(soAddr),
                      dataSize,
                      hostMappedAddress,
                      hostAddress,
                      sliceState.m_execution.m_deviceAddress + offsetForPdmaDown);

        m_commands.serializePdmaCommand(m_currentStream,
                                        m_schedIdx,
                                        true,
                                        hostMappedAddress,
                                        sliceState.m_execution.m_deviceAddress + offsetForPdmaDown,
                                        dataSize,
                                        sliceState.m_isReductionCollective && sliceState.m_currentOp != eHCLAllGather &&
                                            sliceState.m_currentOp != eHCLGather,
                                        sliceState.m_reduceOp,
                                        sliceState.m_16BitReduction && sliceState.m_currentOp != eHCLAllGather &&
                                            sliceState.m_currentOp != eHCLGather,
                                        sliceState.m_apiId,
                                        m_archStreamIdx,
                                        sliceState.m_dataType,
                                        soAddr,
                                        sliceState.m_boxIter < sliceState.m_reproScaleoutBuffersAmount);
    }

    provider.notifyHostScheduler(m_archStreamIdx);
}

LibfabricNonCollectiveScaleoutDescriptor::LibfabricNonCollectiveScaleoutDescriptor(
    HclCollectiveRoutinesGen2Arch& collectiveRoutines,
    ScaleoutProvider&              scaleoutProvider,
    hcl::ScalStream&               currentStream,
    const int                      archStreamIdx,
    const unsigned                 uarchStreamIdx,
    const unsigned                 schedIdx,
    const uint64_t                 targetValue,
    HclCommandsGen2Arch&           commands)
: Descriptor(collectiveRoutines, scaleoutProvider, currentStream, archStreamIdx, uarchStreamIdx, schedIdx),
  m_commands(commands),
  m_targetValue(targetValue)
{
    VERIFY(m_scaleoutProvider.isHostNic(), "Cannot use libfabric descriptor on a non-hostnic provider");
}

unsigned LibfabricNonCollectiveScaleoutDescriptor::getHostUarchStreamIdx()
{
    return (GCFG_ENABLE_HNIC_MICRO_STREAMS.value() ? m_uarchStreamIdx : 0);
}

void LibfabricNonCollectiveScaleoutDescriptor::run(NonCollectiveState& nonCollectiveState)
{
    LOG_HCL_TRACE(
        HCL,
        "(NonCollectiveState): m_schedIdx={},  m_archStreamIdx={}, m_uarchStreamIdx={}, remoteRank={}, m_isSend={}",
        this->m_schedIdx,
        this->m_archStreamIdx,
        this->m_uarchStreamIdx,
        nonCollectiveState.m_remoteRank,
        nonCollectiveState.m_isSend);

    LibfabricScaleoutProvider& provider = dynamic_cast<LibfabricScaleoutProvider&>(m_scaleoutProvider);
    const uint64_t             hostMappedAddress = nonCollectiveState.m_hostMappedAddr;
    const uint64_t             hostAddress       = nonCollectiveState.m_hostAddr;
    const HCL_Rank remoteRank        = nonCollectiveState.m_remoteRank;
    unsigned                   hostUarchStreamIdx = getHostUarchStreamIdx();

    const uint32_t size =
        nonCollectiveState.m_execution.m_deviceCount *
        dataTypeSizeInBytes(nonCollectiveState.m_dataType);  // TODO - not good - we implicitly mult 64bits * unsigned
                                                             // and store it in 32 bits w/o check.
    LOG_HCL_TRACE(HCL,
                  "(NonCollectiveState): hostMappedAddress=0x{:x}, hostAddress=0x{:x}, size={}, remoteRank={}, "
                  "m_recvFenceValue={}, m_isSend={}",
                  hostMappedAddress,
                  hostAddress,
                  size,
                  remoteRank,
                  nonCollectiveState.m_recvFenceValue,
                  nonCollectiveState.m_isSend);

    if (nonCollectiveState.m_isSend)
    {
        HostStream* sendHostStream = provider.m_hostStreamVec[m_archStreamIdx][hostUarchStreamIdx][HOST_STREAM_SEND];
        HostStream* waitForCompHostStream =
            provider.m_hostStreamVec[m_archStreamIdx][hostUarchStreamIdx][HOST_STREAM_WAIT_FOR_SEND_COMP];
        const fence_info fence(nonCollectiveState.m_execution.m_scaleoutFences[0]);

        LOG_HCL_TRACE(HCL,
                      "scaleout send's pdma will signal to {}; move {} bytes of data from device addr 0x{:x} to mapped "
                      "addr 0x{:x} (host 0x{:x})",
                      m_collectiveRoutines.getScalUtils()->printSOBInfo(fence.lbw.addr),
                      size,
                      nonCollectiveState.m_execution.m_deviceAddress,
                      hostMappedAddress,
                      hostAddress);

        m_commands.serializePdmaCommand(m_currentStream,
                                        m_schedIdx,
                                        false,  // isDownload
                                        hostMappedAddress,
                                        nonCollectiveState.m_execution.m_deviceAddress,
                                        size,
                                        false,  // isReduction
                                        hcclOpNone,
                                        false,  // isCastUp
                                        nonCollectiveState.m_apiId,
                                        m_archStreamIdx,
                                        nonCollectiveState.m_dataType,
                                        fence.lbw.addr);

        const uint32_t soAddr = nonCollectiveState.m_execution.m_completionSoAddr;
        const sob_info sob(m_collectiveRoutines.getScalUtils()->getSOBInfo(soAddr));
        LOG_HCL_TRACE(HCL,
                      "send, remoteRank={}, soAddr=0x{:x}, sob.sobId={}, fence.index={}",
                      remoteRank,
                      soAddr,
                      sob.sobId,
                      fence.index);

        sendHostStream->incSrCount();
        OfiCompCallbackParams compParams {
            sob.smIdx,
            sob.sobId,
            m_collectiveRoutines.getSoConfigValue(nonCollectiveState.signalToCost(SignalEvent::HNIC_SCALEOUT_SEND),
                                                  true),
            m_collectiveRoutines.getDevice(),
            libfabricCompCallback};
        HostSchedCommandsGen2Arch::serializeHostScaleOutCommandWithFence(sendHostStream->getOuterQueue(),
                                                                         nonCollectiveState.m_isSend,
                                                                         hostAddress,
                                                                         remoteRank,
                                                                         size,
                                                                         nonCollectiveState.m_comm,
                                                                         fence.index,
                                                                         compParams,
                                                                         sendHostStream->getSrCount());
        LOG_HCL_TRACE(HCL,
                      "scaleout send's completion will signal to {} [0x{:x}]",
                      m_collectiveRoutines.getScalUtils()->printSOBInfo(sob),
                      soAddr);
        HostSchedCommandsGen2Arch::serializeHostWaitForCompletionCommand(waitForCompHostStream->getOuterQueue(),
                                                                         nonCollectiveState.m_comm,
                                                                         sendHostStream->getSrCount());
    }
    else  // receive
    {
        HostStream* recvHostStream = provider.m_hostStreamVec[m_archStreamIdx][hostUarchStreamIdx][HOST_STREAM_RECV];
        HostStream* waitForCompHostStream =
            provider.m_hostStreamVec[m_archStreamIdx][hostUarchStreamIdx][HOST_STREAM_WAIT_FOR_RECV_COMP];
        if (nonCollectiveState.m_firstRank)
        {
            // Needs to be done once per arbitrator recv stream
            const fence_info fence = nonCollectiveState.m_execution.m_scaleoutFences[0];
            m_commands.serializeLbwWriteCommand(m_currentStream, m_schedIdx, fence.lbw.addr, fence.lbw.data);
            LOG_HCL_TRACE(HCL,
                          "recv's serializeLbwWriteCommand to {}",
                          m_collectiveRoutines.getScalUtils()->printSOBInfo(fence.lbw.addr));
            HostSchedCommandsGen2Arch::serializeHostFenceCommand(recvHostStream->getOuterQueue(),
                                                                 fence.index,
                                                                 recvHostStream->getSrCount());
        }

        const sob_info sob1(nonCollectiveState.m_execution
                                .m_scaleoutInternalSOBs[0]);  // we use single internal SOB for all stream recv
        LOG_HCL_TRACE(HCL, "recv, remoteRank={}, sob1.sobId={}, sob1.dcore={}", remoteRank, sob1.sobId, sob1.dcore);
        recvHostStream->incSrCount();
        OfiCompCallbackParams compParams {
            sob1.smIdx,
            sob1.sobId,
            m_collectiveRoutines.getSoConfigValue(nonCollectiveState.signalToCost(SignalEvent::HNIC_SCALEOUT_RECV),
                                                  true),
            m_collectiveRoutines.getDevice(),
            libfabricCompCallback};
        HostSchedCommandsGen2Arch::serializeHostSendScaleOutCommand(recvHostStream->getOuterQueue(),
                                                                    nonCollectiveState.m_isSend,
                                                                    hostAddress,
                                                                    remoteRank,
                                                                    size,
                                                                    nonCollectiveState.m_comm,
                                                                    compParams,
                                                                    recvHostStream->getSrCount());
        LOG_HCL_TRACE(HCL,
                      "scaleout recv's completion will signal to {}",
                      m_collectiveRoutines.getScalUtils()->printSOBInfo(sob1));
        HostSchedCommandsGen2Arch::serializeHostWaitForCompletionCommand(waitForCompHostStream->getOuterQueue(),
                                                                         nonCollectiveState.m_comm,
                                                                         recvHostStream->getSrCount());

        if (nonCollectiveState.m_firstRank)
        {
            // Needs to be done once per recv stream
            m_collectiveRoutines.m_deviceController.streamAddWait(m_currentStream,
                                                                  {sob1, nonCollectiveState.m_recvFenceValue});
        }
        const uint32_t soAddr = nonCollectiveState.m_execution.m_completionSoAddr;
        const sob_info sob2(m_collectiveRoutines.getScalUtils()->getSOBInfo(soAddr));
        LOG_HCL_TRACE(HCL, "recv remoteRank={}, soAddr=0x{:x}, sob2.sobId={}", remoteRank, soAddr, sob2.sobId);
        LOG_HCL_TRACE(HCL,
                      "scaleout recv's pdma will signal to {}; move {} bytes of data from mapped "
                      "addr 0x{:x} (host 0x{:x}) to device addr 0x{:x}",
                      m_collectiveRoutines.getScalUtils()->printSOBInfo(soAddr),
                      size,
                      hostMappedAddress,
                      hostAddress,
                      nonCollectiveState.m_execution.m_deviceAddress);
        m_commands.serializePdmaCommand(m_currentStream,
                                        m_schedIdx,
                                        true,  // isDownload
                                        hostMappedAddress,
                                        nonCollectiveState.m_execution.m_deviceAddress,
                                        size,
                                        false,  // isReduction
                                        hcclOpNone,
                                        false,  // isCastUp
                                        nonCollectiveState.m_apiId,
                                        m_archStreamIdx,
                                        nonCollectiveState.m_dataType,
                                        soAddr);
    }

    provider.notifyHostScheduler(m_archStreamIdx);
}

GaudiDirectScaleoutDescriptor::GaudiDirectScaleoutDescriptor(HclCollectiveRoutinesGen2Arch& collectiveRoutines,
                                                             ScaleoutProvider&              scaleoutProvider,
                                                             hcl::ScalStream&               currentStream,
                                                             int                            archStreamIdx,
                                                             unsigned                       uarchStreamIdx,
                                                             unsigned                       schedIdx,
                                                             HclCommandsGen2Arch&           commands)
: LibfabricScaleoutDescriptor(collectiveRoutines,
                              scaleoutProvider,
                              currentStream,
                              archStreamIdx,
                              uarchStreamIdx,
                              schedIdx,
                              commands),
  m_utils(m_collectiveRoutines.getScalUtils())
{
    VERIFY(m_scaleoutProvider.isHostNic(), "Cannot use gaudi-direct descriptor on a non-hostnic provider");
}
GaudiDirectNonCollectiveScaleoutDescriptor::GaudiDirectNonCollectiveScaleoutDescriptor(
    HclCollectiveRoutinesGen2Arch& collectiveRoutines,
    ScaleoutProvider&              scaleoutProvider,
    hcl::ScalStream&               currentStream,
    const int                      archStreamIdx,
    const unsigned                 uarchStreamIdx,
    const unsigned                 schedIdx,
    const uint64_t                 targetValue,
    HclCommandsGen2Arch&           commands)
: LibfabricNonCollectiveScaleoutDescriptor(collectiveRoutines,
                                           scaleoutProvider,
                                           currentStream,
                                           archStreamIdx,
                                           uarchStreamIdx,
                                           schedIdx,
                                           targetValue,
                                           commands)
{
    VERIFY(m_scaleoutProvider.isHostNic(), "Cannot use gaudi-direct descriptor on a non-hostnic provider");
}

void GaudiDirectScaleoutDescriptor::run(SliceState& sliceState)
{
    LibfabricScaleoutProvider& provider   = dynamic_cast<LibfabricScaleoutProvider&>(m_scaleoutProvider);
    HCL_Rank remoteRank = sliceState.m_dynamicComm.getPodToRankMap()[sliceState.m_boxNumInfo.m_boxNum];

    uint32_t remoteRankIteration = sliceState.m_all2allIter;
    uint32_t dataSize            = sliceState.m_execution.m_cellCount * sliceState.m_dataTypeSizeInBytes;
    unsigned hostUarchStreamIdx  = getHostUarchStreamIdx();
    uint32_t offsetForRecv       = 0;
    uint32_t offsetForSend       = 0;

    const uint32_t soAddr = sliceState.m_execution.m_completionSoAddr;
    const sob_info sob    = m_utils->getSOBInfo(soAddr);

    if (sliceState.m_collectiveOp == eHCLAll2All && sliceState.m_all2allIterations > 1)
    {
        if (sliceState.m_isSlicing)
        {
            offsetForRecv =
                sliceState.m_execution.m_strideCount * remoteRankIteration * sliceState.m_dataTypeSizeInBytes;
            offsetForSend = dataSize * remoteRankIteration;
        }
        else
        {
            offsetForRecv =
                sliceState.m_all2allIterStrideCount * remoteRankIteration * sliceState.m_dataTypeSizeInBytes;
            offsetForSend = offsetForRecv;
        }
    }

    if (sliceState.m_isSend)
    {
        HostStream* sendHostStream = provider.m_hostStreamVec[m_archStreamIdx][hostUarchStreamIdx][HOST_STREAM_SEND];
        HostStream* waitForCompHostStream =
            provider.m_hostStreamVec[m_archStreamIdx][hostUarchStreamIdx][HOST_STREAM_WAIT_FOR_SEND_COMP];
        uint64_t    sendAddr              = sliceState.m_execution.m_deviceAddress + offsetForSend;

        if (dataSize == 0)
        {
            m_commands.serializeLbwWriteCommand(
                m_currentStream,
                m_schedIdx,
                soAddr,
                m_collectiveRoutines.getSoConfigValue(sliceState.signalToCost(SignalEvent::HNIC_SCALEOUT_SEND), true));
            LOG_HCL_DEBUG(HCL, "dataSize = 0, do not perform scaleout send, signaling instead from scheduler");
        }
        else
        {
            fence_info fence = sliceState.m_execution.m_scaleoutFences[0];
            // a dummy signal to mimic PDMA operation to use the same HNIC graph
            m_commands.serializeLbwWriteCommand(
                m_currentStream,
                m_schedIdx,
                fence.lbw.addr,
                m_collectiveRoutines.getSoConfigValue(sliceState.signalToCost(SignalEvent::HNIC_PDMA), true));

            sendHostStream->incSrCount();
            OfiCompCallbackParams compParams {
                sob.smIdx,
                sob.sobId,
                m_collectiveRoutines.getSoConfigValue(sliceState.signalToCost(SignalEvent::HNIC_SCALEOUT_SEND), true),
                m_collectiveRoutines.getDevice(),
                libfabricCompCallback};
            HostSchedCommandsGen2Arch::serializeHostScaleOutCommandWithFence(sendHostStream->getOuterQueue(),
                                                                             sliceState.m_isSend,
                                                                             sendAddr,
                                                                             remoteRank,
                                                                             dataSize,
                                                                             sliceState.m_comm,
                                                                             fence.index,
                                                                             compParams,
                                                                             sendHostStream->getSrCount());

            LOG_HCL_TRACE(HCL, "scaleout send's completion will signal to {}", m_utils->printSOBInfo(sob));
            HostSchedCommandsGen2Arch::serializeHostWaitForCompletionCommand(waitForCompHostStream->getOuterQueue(),
                                                                             sliceState.m_comm,
                                                                             sendHostStream->getSrCount());
        }
    }
    else
    {
        HostStream* recvHostStream = provider.m_hostStreamVec[m_archStreamIdx][hostUarchStreamIdx][HOST_STREAM_RECV];
        HostStream* waitForCompHostStream =
            provider.m_hostStreamVec[m_archStreamIdx][hostUarchStreamIdx][HOST_STREAM_WAIT_FOR_RECV_COMP];
        uint64_t    recvAddr              = sliceState.m_execution.m_deviceAddress + offsetForRecv;

        fence_info fence = sliceState.m_execution.m_scaleoutFences[0];

        m_commands.serializeLbwWriteCommand(m_currentStream, m_schedIdx, fence.lbw.addr, fence.lbw.data);

        if (dataSize == 0)
        {
            streamAddWait(recvHostStream->getOuterQueue(), fence, recvHostStream->getSrCount());

            m_commands.serializeLbwWriteCommand(
                m_currentStream,
                m_schedIdx,
                soAddr,
                m_collectiveRoutines.getSoConfigValue(sliceState.signalToCost(SignalEvent::HNIC_SCALEOUT_RECV), true));
            LOG_HCL_DEBUG(HCL, "dataSize = 0, do not perform scaleout recv, signaling instead from scheduler");
        }
        else
        {
            recvHostStream->incSrCount();
            OfiCompCallbackParams compParams {
                sob.smIdx,
                sob.sobId,
                m_collectiveRoutines.getSoConfigValue(sliceState.signalToCost(SignalEvent::HNIC_SCALEOUT_RECV), true),
                m_collectiveRoutines.getDevice(),
                libfabricCompCallback};
            HostSchedCommandsGen2Arch::serializeHostScaleOutCommandWithFence(recvHostStream->getOuterQueue(),
                                                                             sliceState.m_isSend,
                                                                             recvAddr,
                                                                             remoteRank,
                                                                             dataSize,
                                                                             sliceState.m_comm,
                                                                             fence.index,
                                                                             compParams,
                                                                             recvHostStream->getSrCount());

            LOG_HCL_TRACE(HCL, "scaleout recv's completion will signal to {}", m_utils->printSOBInfo(sob));
            HostSchedCommandsGen2Arch::serializeHostWaitForCompletionCommand(waitForCompHostStream->getOuterQueue(),
                                                                             sliceState.m_comm,
                                                                             recvHostStream->getSrCount());
        }
    }

    provider.notifyHostScheduler(m_archStreamIdx);
}
void GaudiDirectNonCollectiveScaleoutDescriptor::run(NonCollectiveState& nonCollectiveState)
{
    LOG_HCL_TRACE(
        HCL,
        "(NonCollectiveState): m_schedIdx={},  m_archStreamIdx={}, m_uarchStreamIdx={}, remoteRank={}, m_isSend={}",
        this->m_schedIdx,
        this->m_archStreamIdx,
        this->m_uarchStreamIdx,
        nonCollectiveState.m_remoteRank,
        nonCollectiveState.m_isSend);

    LibfabricScaleoutProvider& provider   = dynamic_cast<LibfabricScaleoutProvider&>(m_scaleoutProvider);
    const uint64_t             deviceAddr = nonCollectiveState.m_execution.m_deviceAddress;
    const HCL_Rank             remoteRank = nonCollectiveState.m_remoteRank;
    unsigned                   hostUarchStreamIdx = getHostUarchStreamIdx();
    const uint32_t             soAddr     = nonCollectiveState.m_execution.m_completionSoAddr;
    const sob_info             sob(m_collectiveRoutines.getScalUtils()->getSOBInfo(soAddr));

    const uint32_t size =
        nonCollectiveState.m_execution.m_deviceCount *
        dataTypeSizeInBytes(nonCollectiveState.m_dataType);  // TODO - not good - we implicitly mult 64bits * unsigned
                                                             // and store it in 32 bits w/o check.
    LOG_HCL_TRACE(HCL,
                  "(NonCollectiveState): deviceAddr=0x{:x}, size={}, remoteRank={}, m_recvFenceValue={}, m_isSend={}",
                  deviceAddr,
                  size,
                  remoteRank,
                  nonCollectiveState.m_recvFenceValue,
                  nonCollectiveState.m_isSend);

    if (nonCollectiveState.m_isSend)
    {
        HostStream* sendHostStream = provider.m_hostStreamVec[m_archStreamIdx][hostUarchStreamIdx][HOST_STREAM_SEND];
        HostStream* waitForCompHostStream =
            provider.m_hostStreamVec[m_archStreamIdx][hostUarchStreamIdx][HOST_STREAM_WAIT_FOR_SEND_COMP];
        const fence_info fence(nonCollectiveState.m_execution.m_scaleoutFences[0]);

        // a dummy signal to ensure that the send (on the host stream) doesn't begin before barrierArm finishes
        m_commands.serializeLbwWriteCommand(m_currentStream,
                                            m_schedIdx,
                                            fence.lbw.addr,
                                            m_collectiveRoutines.getSoConfigValue(1, true));
        LOG_HCL_TRACE(HCL,
                      "scaleout send to remoteRank={}, send dummy signal to fence.index={}",
                      remoteRank,
                      fence.index);

        LOG_HCL_TRACE(HCL,
                      "scaleout send to remoteRank={}, will signal to {}",
                      remoteRank,
                      m_collectiveRoutines.getScalUtils()->printSOBInfo(sob));

        sendHostStream->incSrCount();
        OfiCompCallbackParams compParams {
            sob.smIdx,
            sob.sobId,
            m_collectiveRoutines.getSoConfigValue(nonCollectiveState.signalToCost(SignalEvent::HNIC_SCALEOUT_SEND),
                                                  true),
            m_collectiveRoutines.getDevice(),
            libfabricCompCallback};
        HostSchedCommandsGen2Arch::serializeHostScaleOutCommandWithFence(sendHostStream->getOuterQueue(),
                                                                         nonCollectiveState.m_isSend,
                                                                         deviceAddr,
                                                                         remoteRank,
                                                                         size,
                                                                         nonCollectiveState.m_comm,
                                                                         fence.index,
                                                                         compParams,
                                                                         sendHostStream->getSrCount());

        HostSchedCommandsGen2Arch::serializeHostWaitForCompletionCommand(waitForCompHostStream->getOuterQueue(),
                                                                         nonCollectiveState.m_comm,
                                                                         sendHostStream->getSrCount());
    }
    else  // receive
    {
        HostStream* recvHostStream = provider.m_hostStreamVec[m_archStreamIdx][hostUarchStreamIdx][HOST_STREAM_RECV];
        HostStream* waitForCompHostStream =
            provider.m_hostStreamVec[m_archStreamIdx][hostUarchStreamIdx][HOST_STREAM_WAIT_FOR_RECV_COMP];
        if (nonCollectiveState.m_firstRank)
        {
            // Needs to be done once per arbitrator recv stream
            const fence_info fence = nonCollectiveState.m_execution.m_scaleoutFences[0];
            m_commands.serializeLbwWriteCommand(m_currentStream, m_schedIdx, fence.lbw.addr, fence.lbw.data);
            LOG_HCL_TRACE(HCL,
                          "scaleout recv from remoteRank={}, will signal to fence.index={}",
                          remoteRank,
                          fence.index);
            HostSchedCommandsGen2Arch::serializeHostFenceCommand(recvHostStream->getOuterQueue(),
                                                                 fence.index,
                                                                 recvHostStream->getSrCount());
        }

        LOG_HCL_TRACE(HCL,
                      "scaleout recv from remoteRank={}, will signal to {}",
                      remoteRank,
                      m_collectiveRoutines.getScalUtils()->printSOBInfo(sob));
        recvHostStream->incSrCount();
        OfiCompCallbackParams compParams {
            sob.smIdx,
            sob.sobId,
            m_collectiveRoutines.getSoConfigValue(nonCollectiveState.signalToCost(SignalEvent::HNIC_SCALEOUT_SEND),
                                                  true),
            m_collectiveRoutines.getDevice(),
            libfabricCompCallback};
        HostSchedCommandsGen2Arch::serializeHostSendScaleOutCommand(recvHostStream->getOuterQueue(),
                                                                    nonCollectiveState.m_isSend,
                                                                    deviceAddr,
                                                                    remoteRank,
                                                                    size,
                                                                    nonCollectiveState.m_comm,
                                                                    compParams,
                                                                    recvHostStream->getSrCount());

        HostSchedCommandsGen2Arch::serializeHostWaitForCompletionCommand(waitForCompHostStream->getOuterQueue(),
                                                                         nonCollectiveState.m_comm,
                                                                         recvHostStream->getSrCount());
    }

    provider.notifyHostScheduler(m_archStreamIdx);
}