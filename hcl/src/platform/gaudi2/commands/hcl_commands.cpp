#include "platform/gaudi2/commands/hcl_commands.h"

#include <cstdint>  // for uint32_t, uint64_t
#include <map>      // for map
#include <vector>   // for vector
#include "hcl_api_types.h"
#include "hcl_utils.h"                               // for VERIFY
#include "infra/scal/gen2_arch_common/scal_names.h"  // for SchedulersIndex
#include "hcl_log_manager.h"                         // for LOG_*
#include "platform/gaudi2/context_manager.h"         // for ContextManager
#include "platform/gaudi2/context_manager_priv.h"    // for RequiredCollecti...
#include "platform/gaudi2/hcl_count_descriptor.h"    // for CountDescriptor
#include "platform/gaudi2/hcl_graph_sync.h"          // for HclGraphSyncGaudi2
#include "platform/gaudi2/hcl_packets.h"             // for serializeAllocBa...
#include "platform/gaudi2/port_mapping.h"            // for Gaudi2DevicePortMapping
#include "platform/gaudi2/send_recv_aggregator.h"    // for SendRecvAggregator
#include "sched_pkts.h"                              // for g2fw
#include "platform/gen2_arch_common/send_recv_aggregator.h"  // for SendRecvEntry
#include "platform/gaudi2/nic_passthrough_handler.h"         // for pRecordWithMetadata
#include "platform/gen2_arch_common/hcl_collective_routines.h"  // for RR_BUFFER_GRANULARITY_SCALEOUT

namespace hcl
{
class ScalStreamBase;
}

HclCommandsGaudi2::HclCommandsGaudi2() : HclCommandsGen2Arch() {}

bool HclCommandsGaudi2::isCastDown(uint32_t dmaType)
{
    return (g2fw::edma_eng_arc_cmd_t)dmaType == g2fw::NIC_EDMA_CMD_CAST_DOWN_CLEAR;
}

bool HclCommandsGaudi2::isCastUp(uint32_t dmaType)
{
    return (g2fw::edma_eng_arc_cmd_t)dmaType == g2fw::NIC_EDMA_CMD_CAST_UP_BATCH_V3;
}

bool HclCommandsGaudi2::isMemCpy(uint32_t dmaType)
{
    return (g2fw::edma_eng_arc_cmd_t)dmaType == g2fw::NIC_EDMA_CMD_MEMCPY_V3;
}

unsigned HclCommandsGaudi2::getDmaTypeCastUp()
{
    return g2fw::NIC_EDMA_CMD_CAST_UP_BATCH_V3;
}
unsigned HclCommandsGaudi2::getDmaTypeCastDown()
{
    return g2fw::NIC_EDMA_CMD_CAST_DOWN_CLEAR;
}
unsigned HclCommandsGaudi2::getDmaTypeMemCpy()
{
    return g2fw::NIC_EDMA_CMD_MEMCPY_V3;
}

static uint32_t calculateRemoteIndex(std::array<int, HLS2_BOX_SIZE>& deviceToRemoteIndex,
                                     int                             selfModuleId,
                                     int                             remoteDevice,
                                     HCL_CollectiveOp                currentOp,
                                     bool                            isSend,
                                     bool                            isComplexCollective,
                                     bool                            isReductionInIMB,
                                     bool                            reproReduction,
                                     bool                            isHierarchical,
                                     uint64_t                        count,
                                     uint64_t                        cellCount,
                                     HCL_CollectiveOp                complexCollective,
                                     bool                            isRoot)
{
    if ((currentOp != eHCLScatter && currentOp != eHCLGather && currentOp != eHCLSimpleBroadcast) &&
        (deviceToRemoteIndex[remoteDevice] == -1 || deviceToRemoteIndex[selfModuleId] == -1))
        return -1;

    bool outOfBounds = cellCount * deviceToRemoteIndex[selfModuleId] >= count;
    switch (currentOp)
    {
        case eHCLReduceScatter:
            if (isSend || reproReduction)
            {
                return deviceToRemoteIndex[remoteDevice];
            }
            else if (isComplexCollective && !isReductionInIMB && (!isHierarchical || outOfBounds))
            {
                if (complexCollective == eHCLReduce && !isRoot && !outOfBounds)
                {
                    return 0;
                }
                return deviceToRemoteIndex[selfModuleId];
            }
            else if (isComplexCollective && isReductionInIMB && outOfBounds)
            {
                return deviceToRemoteIndex[selfModuleId];
            }
            else if (complexCollective == eHCLReduce && isRoot && !isReductionInIMB && isHierarchical)
            {
                return deviceToRemoteIndex[selfModuleId];
            }
            else
            {
                return 0;
            }
        case eHCLAllGather:
            return isSend ? (isComplexCollective ? deviceToRemoteIndex[selfModuleId] : 0)
                          : deviceToRemoteIndex[remoteDevice];
        case eHCLGather:
        case eHCLAll2All:
        case eHCLScatter:  // FALLTHROUGH
        case eHCLSimpleBroadcast:
            return deviceToRemoteIndex[remoteDevice];
        case eHCLNoCollective:  // send recv
            return 0;
        default:
            VERIFY(false, "Cannot run collectiveOp {} on Gaudi2 device", (int)currentOp);
    }
}

static uint32_t calculateRsi(const int remoteRankToRsi, HCL_CollectiveOp collectiveOp, uint32_t remoteRankIteration)
{
    if ((collectiveOp != eHCLBroadcast && collectiveOp != eHCLSinglePeerBroadcast &&
         collectiveOp != eHCLSimpleBroadcast) &&
        (remoteRankToRsi == -1))
    {
        return -1;
    }

    return remoteRankIteration;
}

void HclCommandsGaudi2::serializeDmaCommand(hcl::ScalStreamBase& scalStream, DmaCmdParams& cmd)
{
    LOG_HCL_TRACE(HCL, "SOAddress1(0x{:x}), SOAddress2(0x{:x})", cmd.m_soAddressLSB, cmd.m_soAddressLSB2);
    uint64_t sendDataSize = cmd.m_chunkCount * dataTypeSizeInBytes(cmd.m_dataType) *
                            ((isCastDown(cmd.m_dmaType) && !GCFG_HCL_USE_EDMA_COMMAND_V3.value()) ? 2 : 1);
    bool isReduction = cmd.m_collectiveOp == eHCLReduceScatter || cmd.m_collectiveOp == eHCLAllReduce ||
                       cmd.m_collectiveOp == eHCLReduce;
    bool     is16BitMemcpy   = isDataTypeTwoBytes(cmd.m_dataType);
    bool     useReductionInd = ((is16BitMemcpy && cmd.m_useCasting) || cmd.m_isGDRMemcpy);

    if (GCFG_HCL_USE_EDMA_COMMAND_V3.value())
    {
        hcclRedOp_t reductionOp = cmd.m_reduceOp;
        uint32_t tempDmaType;
        if (cmd.m_useSibo)
        {
            tempDmaType = g2fw::NIC_EDMA_CMD_SIBO_OPS_V3;
        }
        else
        {
            tempDmaType = g2fw::NIC_EDMA_CMD_LIN_OPS_V3;
            reductionOp = hcclSum;
        }
        SchedArcCommandsGaudi2::serializeDmaCommandV3(scalStream,
                                                      cmd.m_schedIdx,
                                                      tempDmaType,
                                                      cmd.m_soAddressLSB,
                                                      sendDataSize,
                                                      cmd.m_recvBaseAddress,
                                                      cmd.m_sendBaseAddress,
                                                      reductionOp,
                                                      cmd.m_streamCtxtId,
                                                      cmd.m_dataType,
                                                      cmd.m_poolId,
                                                      cmd.m_isForScaleout,
                                                      cmd.m_useCasting,
                                                      cmd.m_numberOfRanks,
                                                      cmd.m_numberOfReproBuffers,
                                                      cmd.m_indexOfReproBuffer,
                                                      is16BitMemcpy,
                                                      cmd.m_soAddressLSB2,
                                                      cmd.m_isBFloat,
						      useReductionInd);
    }
    else  // v2
    {
        SchedArcCommandsGaudi2::serializeDmaCommandV2(scalStream,
                                                      cmd.m_schedIdx,
                                                      cmd.m_dmaType,
                                                      cmd.m_soAddressLSB,
                                                      cmd.m_soAddressLSB2,
                                                      sendDataSize,
                                                      cmd.m_recvBaseAddress,
                                                      cmd.m_sendBaseAddress,
                                                      cmd.m_reduceOp,
                                                      isReduction,
                                                      cmd.m_reductionSignalToCg,
                                                      cmd.m_dataType,
                                                      cmd.m_poolId,
                                                      cmd.m_isReproReduction,
                                                      cmd.m_useSibo,
                                                      cmd.m_numberOfRanks,
                                                      cmd.m_numberOfReproBuffers,
                                                      cmd.m_indexOfReproBuffer,
                                                      cmd.m_isReproReduction && isDataTypeTwoBytes(cmd.m_dataType),
                                                      cmd.m_isGDRMemcpy);
    }
}

void HclCommandsGaudi2::serializeGlobalDmaCommand(hcl::ScalStreamBase&                  scalStream,
                                                  uint32_t                              soAddressLSB,
                                                  const std::vector<sibAddressAndSize>& sibAddressesAndSizes,
                                                  uint32_t                              fwStrideSize,
                                                  uint64_t                              fwBaseAddress)
{
    if (GCFG_HCL_USE_EDMA_COMMAND_V3.value())
    {
        SchedArcCommandsGaudi2::serializeGlobalDmaCommandV3(
            scalStream,
            soAddressLSB,
            sibAddressesAndSizes,
            fwStrideSize,
            fwBaseAddress,
            ScalNetworkGarbageCollectorAndReductionGroups::SCAL_EDMA_NETWORK_GC_REDUCTION_GROUP0);
    }
    else
    {
        SchedArcCommandsGaudi2::serializeGlobalDmaCommandV2(
            scalStream,
            soAddressLSB,
            sibAddressesAndSizes,
            ScalNetworkGarbageCollectorAndReductionGroups::SCAL_EDMA_NETWORK_GC_REDUCTION_GROUP0);
        SchedArcCommandsGaudi2::serializeGlobalDmaCommandV2(
            scalStream,
            soAddressLSB,
            sibAddressesAndSizes,
            ScalNetworkGarbageCollectorAndReductionGroups::SCAL_EDMA_NETWORK_GC_REDUCTION_GROUP1);
    }
}

void HclCommandsGaudi2::serializeMemsetCommand(hcl::ScalStreamBase& scalStream,
                                               unsigned             schedIdx,
                                               uint64_t             addr,
                                               uint64_t             sizeInBytes,
                                               uint32_t             soAddressLSB,
                                               uint8_t              streamCtxtID,
                                               hcclDataType_t       dataType,
                                               hcclRedOp_t          reduceOp,
                                               bool                 useSibo,
                                               uint32_t             poolId,
                                               bool                 isForScaleout,
                                               uint32_t             numberOfRanks,
                                               uint32_t             numberOfReproBuffers,
                                               unsigned             indexOfReproBuffer)
{
    if (GCFG_HCL_USE_EDMA_COMMAND_V3.value())
    {
        uint32_t tempDmaType;
        if (useSibo)
        {
            tempDmaType = g2fw::NIC_EDMA_CMD_SIBO_MEMSET_V3;
        }
        else
        {
            tempDmaType = g2fw::NIC_EDMA_CMD_LIN_MEMSET_V3;
        }

        SchedArcCommandsGaudi2::serializeDmaCommandV3(scalStream,
                                                      schedIdx,
                                                      tempDmaType,
                                                      soAddressLSB,
                                                      sizeInBytes,
                                                      addr,
                                                      addr,
                                                      reduceOp,
                                                      streamCtxtID,
                                                      dataType,
                                                      poolId,
                                                      isForScaleout,
                                                      false,
                                                      numberOfRanks,
                                                      numberOfReproBuffers,
                                                      indexOfReproBuffer);
    }
    else
    {
        SchedArcCommandsGaudi2::serializeDmaCommandV2(scalStream,
                                                      schedIdx,
                                                      g2fw::NIC_EDMA_CMD_CAST_DOWN_CLEAR,
                                                      0,
                                                      soAddressLSB,
                                                      sizeInBytes,
                                                      addr,
                                                      addr,
                                                      reduceOp,
                                                      false,
                                                      false,
                                                      dataType);
    }
}

void HclCommandsGaudi2::serializeInitSequenceCommands(hcl::ScalStreamBase&                  recvStream,
                                                      hcl::ScalStreamBase&                  recvSOStream,
                                                      hcl::ScalStreamBase&                  dmaStream,
                                                      unsigned                              indexOfCg,
                                                      uint64_t                              soAddressLSB,
                                                      const std::vector<sibAddressAndSize>& sibAddressesAndSizes,
                                                      ContextManager&                       contextManager,
                                                      uint32_t                              fwStrideSize,
                                                      uint64_t                              fwBaseAddress,
                                                      uint8_t                               apiId,
                                                      unsigned                              edmaEngineWorkDistributionSize)
{
    HclCommandsGaudi2  commands;
    HclGraphSyncGaudi2 graphSync(0, commands);
    // one signal for each scaleup port +
    // if SO global context should be updated - signal for each scaleout port +
    // 3 signals (1 for each engine (V3)) for global dma command +
    // 3 signals for each memset of buffers (1 for each engine (V3))
    // *global DMA command does not signal to CG if not V3.
    unsigned numberOfSignals =
        contextManager.m_portMapping.getNumScaleUpPorts() +
        (GCFG_HCL_USE_EDMA_COMMAND_V3.value() ? edmaEngineWorkDistributionSize : 0) +
        sibAddressesAndSizes.size() * (GCFG_HCL_USE_EDMA_COMMAND_V3.value() ? edmaEngineWorkDistributionSize : 1);

    if (contextManager.m_portMapping.isUpateScaleOutGlobalContextRequired())
    {
        numberOfSignals += contextManager.m_portMapping.getMaxNumScaleOutPorts();
    }

    SchedArcCommandsGaudi2::serializeAllocBarrierCommand(recvStream,
                                                         (int)hcl::SchedulersIndex::recvScaleUp,
                                                         indexOfCg,
                                                         1);
    SchedArcCommandsGaudi2::serializeLbwWriteCommand(
        recvStream,
        (unsigned)hcl::SchedulersIndex::recvScaleUp,
        soAddressLSB,
        graphSync.getSoConfigValue(COMP_SYNC_GROUP_CMAX_TARGET - numberOfSignals, true));

    // Use RR flow as default in order to enable RR and non RR mode to be able to work simultaneously

    for (size_t index = 0; index < sibAddressesAndSizes.size(); index++)
    {
        LOG_TRACE(HCL,
                  "RR | intermediateBaseAddress[{}] 0x{:x}, slice size: 0x{:x}",
                  index,
                  sibAddressesAndSizes[index].sibBaseAddr,
                  sibAddressesAndSizes[index].sibSize);
    }

    // The address passed here is used by the NIC and main usage is in-order receive for scale-up, so we pass only the
    // buffer that contains scale-up pools
    contextManager.serializeUpdateGlobalContext(recvStream,
                                                soAddressLSB & 0xffffffff,
                                                sibAddressesAndSizes[1].sibBaseAddr,
                                                sibAddressesAndSizes[1].sibSize);

    if (contextManager.m_portMapping.isUpateScaleOutGlobalContextRequired())
    {
        contextManager.serializeUpdateGlobalContextScaleOut(recvSOStream, soAddressLSB & 0xffffffff);
    }
    serializeGlobalDmaCommand(dmaStream, soAddressLSB & 0xffffffff, sibAddressesAndSizes, fwStrideSize, fwBaseAddress);

    uint8_t streamCtxtID = hcl::encodeStreamContextID(apiId, hcl::DEFAULT_STREAM_IDX);
    // sibAddressesAndSizes = pools per stream
    // {stream 0 {SO_RR_POOL=pool 0, SU_RR_POOL+REDUCE_POOl=pool 1},
    // {stream 1 {SO_RR_POOL=pool 0, SU_RR_POOL+REDUCE_POOl=pool 1},
    // {stream 2 {SO_RR_POOL=pool 0, SU_RR_POOL+REDUCE_POOl=pool 1}}
    for (size_t index = 0; index < sibAddressesAndSizes.size(); index++)
    {
        serializeMemsetCommand(dmaStream,
                               (unsigned)hcl::SchedulersIndex::dma,
                               sibAddressesAndSizes[index].sibBaseAddr,
                               sibAddressesAndSizes[index].sibSize,
                               soAddressLSB & 0xffffffff,
                               streamCtxtID,
                               hcclFloat32,
                               hcclSum,
                               true,                        // true for sibo memset
                               index % MAX_NUM_POOL_SIZES,  // pool index 0/1
                               false,
                               sibAddressesAndSizes[index].sibAmount,
                               sibAddressesAndSizes[index].sibAmount,
                               index / 2);
    }
}

void HclCommandsGaudi2::serializeScaleUpCollectiveOp(hcl::ScalStreamBase&   scalStream,
                                                     ScaleUpCollectiveOpG2& scaleupCollectiveOp)
{
    // make sure we have a valid device index
    VERIFY(scaleupCollectiveOp.m_selfModuleId >= 0, "received invalid device {}", scaleupCollectiveOp.m_selfModuleId);

    // if this is a send (and not a recv) command we need to shift the collective context by 8
    if (scaleupCollectiveOp.m_isSend)
    {
        scaleupCollectiveOp.m_collectiveContextIndex += 8;
    }

    // here we create the collective context which we would like to have in the FW
    hcclRedOp_t               effectiveReductionOp = hcclOpNone;
    RequiredCollectiveContext requiredContext(scaleupCollectiveOp.m_collectiveOp,
                                              effectiveReductionOp,
                                              scaleupCollectiveOp.m_soAddress,
                                              scaleupCollectiveOp.m_isSend,
                                              scaleupCollectiveOp.m_baseAddress >> 32,
                                              scaleupCollectiveOp.m_dataType,
                                              scaleupCollectiveOp.m_strideCount);

    std::array<UniqueCollectiveContext, HLS2_BOX_SIZE> uniqueContexts;
    int                                                numberOfEnabledConnections = 0;

    // calculate RRI, and connection params
    for (int deviceIndex = 0; deviceIndex < HLS2_BOX_SIZE; deviceIndex++)
    {
        UniqueCollectiveContext& uniqueContext = uniqueContexts[deviceIndex];
        uniqueContext.remote_index             = calculateRemoteIndex(scaleupCollectiveOp.m_deviceToRemoteIndex,
                                                          scaleupCollectiveOp.m_selfModuleId,
                                                          deviceIndex,
                                                          scaleupCollectiveOp.m_collectiveOp,
                                                          scaleupCollectiveOp.m_isSend,
                                                          scaleupCollectiveOp.m_isComplexCollective,
                                                          scaleupCollectiveOp.m_isReductionInIMB,
                                                          scaleupCollectiveOp.m_reproReduction,
                                                          scaleupCollectiveOp.m_isHierarchical,
                                                          scaleupCollectiveOp.m_count,
                                                          scaleupCollectiveOp.m_cellCount,
                                                          scaleupCollectiveOp.m_complexCollective,
                                                          scaleupCollectiveOp.m_isRoot);
        if (uniqueContext.remote_index == (unsigned)-1)
        {
            uniqueContext.connection_enabled = 0;
        }
        else
        {
            uniqueContext.connection_enabled = 1;
            numberOfEnabledConnections++;
        }
    }

    requiredContext.m_remoteDescriptor = ContextManager::createRemoteDescriptor(uniqueContexts);

    unsigned commDescIndex          = 0;
    unsigned syncObjectAddressIndex = 0;
    bool     isAllGather            = (scaleupCollectiveOp.m_collectiveOp == eHCLAllGather);

    // check the required context vs the cached one and let me know what dwords need updating
    scaleupCollectiveOp.m_contextManager.updateCollectiveContextScaleUp(scalStream,
                                                                        scaleupCollectiveOp.m_selfModuleId,
                                                                        scaleupCollectiveOp.m_isSend,
                                                                        scaleupCollectiveOp.m_collectiveContextIndex,
                                                                        scaleupCollectiveOp.m_dynamicComm,
                                                                        isAllGather,
                                                                        requiredContext,
                                                                        nullptr,
                                                                        syncObjectAddressIndex,
                                                                        commDescIndex);

    CountDescriptor countDesc(scaleupCollectiveOp.m_cellCount, NUM_SCALEUP_PORTS_PER_CONNECTION);

    if (countDesc.isShort() && ((scaleupCollectiveOp.m_baseAddress % 16) == 0))
    {
        if (scaleupCollectiveOp.m_isSend || !scaleupCollectiveOp.m_reproReduction)
        {
            SchedArcCommandsGaudi2::serializeCollectiveSendShortCommand(
                scalStream,
                scaleupCollectiveOp.m_collectiveContextIndex,
                commDescIndex,
                scaleupCollectiveOp.m_isSend,
                scaleupCollectiveOp.m_hasBufferSize,
                scaleupCollectiveOp.m_count,
                syncObjectAddressIndex,
                scaleupCollectiveOp.m_collectiveOp == eHCLGather && scaleupCollectiveOp.m_isSend,
                countDesc.m_cacheLineCount,
                countDesc.m_cacheLineRemainder,
                countDesc.m_elementRemainder,
                scaleupCollectiveOp.m_baseAddress,
                scaleupCollectiveOp.m_notifyRndvAck,
                scaleupCollectiveOp.m_waitForRndvAcks);
        }
        else
        {
            SchedArcCommandsGaudi2::serializeCollectiveRecvShortInOrderCommand(
                scalStream,
                scaleupCollectiveOp.m_collectiveContextIndex,
                commDescIndex,
                scaleupCollectiveOp.m_hasBufferSize,
                syncObjectAddressIndex,
                countDesc.m_cacheLineCount,
                scaleupCollectiveOp.m_dynamicComm.getRankInPod(),
                scaleupCollectiveOp.m_accuIndex,
                scaleupCollectiveOp.m_rrIndex,
                scaleupCollectiveOp.m_numOfRanks,
                countDesc.numberOfActivatedNics(),
                scaleupCollectiveOp.m_poolId);
        }
    }
    else
    {
        LOG_DEBUG(HCL,
                  "Decided to use long variation of collective, need {} cache lines and base address is 0x{:x}",
                  countDesc.m_cacheLineCount,
                  scaleupCollectiveOp.m_baseAddress);
        SchedArcCommandsGaudi2::serializeCollectiveSendLongCommand(scalStream,
                                                                   scaleupCollectiveOp.m_collectiveContextIndex,
                                                                   commDescIndex,
                                                                   scaleupCollectiveOp.m_isSend,
                                                                   scaleupCollectiveOp.m_hasBufferSize,
                                                                   scaleupCollectiveOp.m_count,
                                                                   syncObjectAddressIndex,
                                                                   scaleupCollectiveOp.m_collectiveOp == eHCLGather &&
                                                                       scaleupCollectiveOp.m_isSend,
                                                                   countDesc.m_cacheLineCount,
                                                                   countDesc.m_cacheLineRemainder,
                                                                   countDesc.m_elementRemainder,
                                                                   scaleupCollectiveOp.m_baseAddress,
                                                                   scaleupCollectiveOp.m_notifyRndvAck,
                                                                   scaleupCollectiveOp.m_waitForRndvAcks);
    }
}

void HclCommandsGaudi2::serializeScaleOutCollectiveOp(hcl::ScalStreamBase&    scalStream,
                                                      ScaleOutCollectiveOpG2& scaleoutCollectiveOp)
{
    // if this is a send (and not a recv) command we need to shift the collective context by 8
    if (scaleoutCollectiveOp.m_isSend)
    {
        scaleoutCollectiveOp.m_collectiveContextIndex += 8;
    }

    // here we create the collective context which we would like to have in the FW
    hcclRedOp_t effectiveReductionOp =
        (!scaleoutCollectiveOp.m_doReduction) ? hcclSum : scaleoutCollectiveOp.m_reduceOp;
    RequiredCollectiveContext requiredContext(scaleoutCollectiveOp.m_collectiveOp,
                                              effectiveReductionOp,
                                              scaleoutCollectiveOp.m_soAddress,
                                              scaleoutCollectiveOp.m_isSend,
                                              scaleoutCollectiveOp.m_baseAddress >> 32,
                                              scaleoutCollectiveOp.m_dataType,
                                              scaleoutCollectiveOp.m_strideCount);

    // get a set of all the dwords to update
    edwords_t dwordsForUpdate =
        scaleoutCollectiveOp.m_contextManager.getDwordsForUpdate(false,
                                                                 scaleoutCollectiveOp.m_collectiveContextIndex,
                                                                 scaleoutCollectiveOp.m_comm,
                                                                 requiredContext);

    // get the SO to increment and the dword values
    unsigned                      syncObjectAddressIndex;
    ContextManager::ContextValues contextValues = {};
    scaleoutCollectiveOp.m_contextManager.updateCollectiveContextScaleOut(scaleoutCollectiveOp.m_collectiveContextIndex,
                                                                          requiredContext,
                                                                          dwordsForUpdate,
                                                                          syncObjectAddressIndex,  // output
                                                                          contextValues);          // output

    // get the rsi descriptors
    std::array<uint16_t, 4> qpnDesc = {0};

    nics_mask_t scaleOutPorts = scaleoutCollectiveOp.m_contextManager.m_portMapping.getScaleOutPorts();

    qpnDesc[0] = calculateRsi(scaleoutCollectiveOp.m_remoteRankToRsi,
                              scaleoutCollectiveOp.m_collectiveOp,
                              scaleoutCollectiveOp.m_remoteRankIteration);

    int i = 1;
    for (auto nic : scaleOutPorts)
    {
        qpnDesc[i++] =
            scaleoutCollectiveOp.m_contextManager.getRemoteRankQp(scaleoutCollectiveOp.m_collectiveContextIndex,
                                                                  scaleoutCollectiveOp.m_comm,
                                                                  scaleoutCollectiveOp.m_remoteRank,
                                                                  nic,
                                                                  scaleoutCollectiveOp.m_qpSet);
    }

    CountDescriptor countDesc(scaleoutCollectiveOp.m_cellCount,
                              scaleoutCollectiveOp.m_contextManager.m_portMapping.getNumScaleOutPorts());

    SchedArcCommandsGaudi2::serializeCollectiveSendScaleOutCommand(scalStream,
                                                                   scaleoutCollectiveOp.m_collectiveContextIndex,
                                                                   scaleoutCollectiveOp.m_isSend,
                                                                   scaleoutCollectiveOp.m_hasBufferSize,
                                                                   scaleoutCollectiveOp.m_count,
                                                                   syncObjectAddressIndex,
                                                                   countDesc.m_cacheLineCount,
                                                                   countDesc.m_cacheLineRemainder,
                                                                   countDesc.m_elementRemainder,
                                                                   scaleoutCollectiveOp.m_baseAddress,
                                                                   contextValues,
                                                                   qpnDesc,
                                                                   scaleoutCollectiveOp.m_notifyRndvAck,
                                                                   scaleoutCollectiveOp.m_waitForRndvAcks);
}

static int getFirstValid(const SendRecvArray& sendRecvArray)
{
    int i = 0;
    for (const SendRecvEntry& entry : sendRecvArray)
    {
        if (entry.isValid) return i;
        i++;
    }
    return -1;
}

void HclCommandsGaudi2::serializeScaleUpSendRecv(hcl::ScalStreamBase& scalStream,
                                                 ContextManager&      contextManager,
                                                 SendRecvAggregator&  aggregator,
                                                 const SendRecvArray& sendRecvArray,
                                                 int                  selfModuleId,
                                                 HCL_Comm             comm,
                                                 unsigned             collectiveContextIndex,
                                                 uint32_t             soAddress,
                                                 bool                 isSend,
                                                 bool                 isLast,
                                                 bool                 notifyRndvAck,
                                                 bool                 waitForRndvAcks)
{
    VERIFY(selfModuleId >= 0, "received invalid device {}", selfModuleId);
    LOG_HCL_TRACE(HCL, "selfModuleId={}, isLast={}, isSend={}", selfModuleId, isLast, isSend);

    int            firstValid = getFirstValid(sendRecvArray);
    uint32_t       addressMSB = firstValid >= 0 ? sendRecvArray[firstValid].address >> 32 : 0;
    hcclDataType_t dataType   = firstValid >= 0 ? sendRecvArray[firstValid].dataType : hcclNumTypes;

    if (isSend)
    {
        collectiveContextIndex += 8;
    }

    RequiredCollectiveContext aggregatedContext;
    RequiredCollectiveContext requiredContext(eHCLNoCollective,
                                              hcclOpNone,
                                              soAddress,
                                              isSend,
                                              addressMSB,
                                              dataType,
                                              /*strideCount=*/0);

    if (aggregator.willFlush() && aggregator.getRequiredContext(aggregatedContext))
    {
        edwords_t dwordsForUpdate;
        aggregatedContext.dwordDiff(requiredContext, dwordsForUpdate);
        dwordsForUpdate.DW0 = false;  // ignore reduction because we specify that in the send/recv command
        dwordsForUpdate.DW1 = false;  // ignore SOB changes because we didn't increment it yet

        if (dwordsForUpdate > 0)
        {
            // Need to submit the changes to the context we accumulated already.
            flushAggregator(scalStream,
                            aggregator,
                            contextManager,
                            collectiveContextIndex,
                            selfModuleId,
                            comm,
                            isSend,
                            &dwordsForUpdate,
                            aggregatedContext,
                            notifyRndvAck,
                            waitForRndvAcks);
        }
    }

    aggregator.addSendRecvArray(sendRecvArray, selfModuleId, collectiveContextIndex, std::move(requiredContext));

    if (isLast)
    {
        // This is the last send/recv command - we need to flush either way.
        VERIFY(aggregator.getRequiredContext(aggregatedContext),
               "No aggregated collective context even though one was just created");
        flushAggregator(scalStream,
                        aggregator,
                        contextManager,
                        collectiveContextIndex,
                        selfModuleId,
                        comm,
                        isSend,
                        nullptr,
                        aggregatedContext,
                        notifyRndvAck,
                        waitForRndvAcks);
    }
}

void HclCommandsGaudi2::flushAggregator(hcl::ScalStreamBase&       scalStream,
                                        SendRecvAggregator&        aggregator,
                                        ContextManager&            contextManager,
                                        unsigned                   collectiveContextIndex,
                                        int                        selfModuleId,
                                        HCL_Comm                   comm,
                                        bool                       isSend,
                                        edwords_t*                 pDwords,
                                        RequiredCollectiveContext& collectiveContext,
                                        bool                       notifyRndvAck,
                                        bool                       waitForRndvAcks)
{
    unsigned commDescIndex          = 0;
    unsigned syncObjectAddressIndex = 0;

    contextManager.updateCollectiveContextScaleUp(scalStream,
                                                  selfModuleId,
                                                  isSend,
                                                  collectiveContextIndex,
                                                  comm,
                                                  false,
                                                  collectiveContext,
                                                  pDwords,
                                                  syncObjectAddressIndex,
                                                  commDescIndex);

    aggregator.flush(scalStream,
                     contextManager,
                     collectiveContextIndex,
                     commDescIndex,
                     selfModuleId,
                     comm,
                     syncObjectAddressIndex,
                     isSend,
                     notifyRndvAck,
                     waitForRndvAcks);
}

void HclCommandsGaudi2::serializeAllocBarrierCommand(hcl::ScalStreamBase& scalStream,
                                                     unsigned             schedIdx,
                                                     uint32_t             completionGroupIndex,
                                                     uint32_t             requiredSobs)
{
    SchedArcCommandsGaudi2::serializeAllocBarrierCommand(scalStream, schedIdx, completionGroupIndex, requiredSobs);
};

void HclCommandsGaudi2::serializeLbwWriteCommand(hcl::ScalStreamBase& scalStream,
                                                 unsigned             schedIdx,
                                                 uint32_t             destination,
                                                 uint32_t             data,
                                                 bool                 blockUntilCompletion)
{
    SchedArcCommandsGaudi2::serializeLbwWriteCommand(scalStream, schedIdx, destination, data, blockUntilCompletion);
};

void HclCommandsGaudi2::serializeFenceCommand(hcl::ScalStreamBase& scalStream,
                                              unsigned             schedIdx,
                                              uint32_t             fenceIndex,
                                              uint32_t             target)
{
    SchedArcCommandsGaudi2::serializeFenceCommand(scalStream, schedIdx, fenceIndex, target);
};

void HclCommandsGaudi2::serializeFenceIncCommand(hcl::ScalStreamBase& scalStream,
                                                 unsigned             schedIdx,
                                                 uint32_t             fenceIndex)
{
    SchedArcCommandsGaudi2::serializeFenceIncCommand(scalStream, schedIdx, fenceIndex);
};

void HclCommandsGaudi2::serializeNopCommand(hcl::ScalStreamBase& scalStream, unsigned schedIdx, uint32_t padding)
{
    SchedArcCommandsGaudi2::serializeNopCommand(scalStream, schedIdx, padding);
}

void HclCommandsGaudi2::serializeNicNopCommand(pRecordWithMetadata& record,
                                               unsigned             collectiveContextIndex,
                                               uint32_t             dupMask,
                                               size_t               requiredCredits,
                                               unsigned             syncObjectAddressIndex,
                                               bool                 incSOB)
{
    SchedArcCommandsGaudi2::serializeNicNopCommand(record,
                                                   collectiveContextIndex,
                                                   dupMask,
                                                   requiredCredits,
                                                   syncObjectAddressIndex,
                                                   incSOB);
}

void HclCommandsGaudi2::serializeNicPassthroughCommand(hcl::ScalStreamBase&              scalStream,
                                                       std::vector<pRecordWithMetadata>& records,
                                                       size_t                            credits,
                                                       bool                              isSend)
{
    SchedArcCommandsGaudi2::serializeNicPassthroughCommand(scalStream, records, credits, isSend);
}

size_t HclCommandsGaudi2::recordsSizeInDwords(std::vector<pRecordWithMetadata>& records)
{
    return SchedArcCommandsGaudi2::recordsSizeInDwords(records);
}

void HclCommandsGaudi2::serializeUserSendCommand(std::vector<uint32_t>& out,
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
    SchedArcCommandsGaudi2::serializeUserSendCommand(out,
                                                     collectiveContextIndex,
                                                     commDescIndex,
                                                     syncObjectAddressIndex,
                                                     cacheLineCount,
                                                     cacheLineRemainder,
                                                     elementRemainder,
                                                     dataType,
                                                     address,
                                                     isLastInGroup,
                                                     notifyRndvAck,
                                                     waitForRndvAcks);
}

void HclCommandsGaudi2::memsetIMBs(DeviceBufferManager&              imb,
                                   hcl::IntermediateBufferContainer* imbContainer,
                                   SignalsManager*                   signalsManager,
                                   SliceState&                       sendSliceState,
                                   SliceState&                       recvSliceState,
                                   unsigned int                      sizeInBytes,
                                   hcl::syncInfo                     longSo,
                                   unsigned                          schedIdx,
                                   hcl::ScalStream&                  garbageCollectionStream,
                                   HCL_StreamId                      m_streamId,
                                   e_devicePoolID                    poolId,
                                   uint8_t                           streamCtxtID,
                                   hcclDataType_t                    dataType)
{
    // get relevant slice
    unsigned indexOfReproBuffer = imb.getSliceId(poolId, m_streamId);

    // get correct index by relevant granularity
    indexOfReproBuffer /= imb.getFactor(poolId);

    if (imb.bufferExpired(poolId))
    {
        unsigned bufferSize = imbContainer->getSliceSize(DeviceBufferManager::getPoolSizeIndex(poolId));

        VERIFY(sizeInBytes <= bufferSize,
               "Unsupported buffer size, sizeInBytes={}, bufferSize={}",
               sizeInBytes,
               bufferSize);

        unsigned    memsetLoops   = 1;
        unsigned    initialOffset = 0;
        hcclRedOp_t effectiveOp   = sendSliceState.m_reduceOp;

        if (poolId == SCALEOUT_RR_POOL)
        {
            if (!GCFG_HCL_USE_EDMA_COMMAND_V3.value())
            {
                // memset loops are required for lin memset, used only for v2 commands
                memsetLoops = std::min(sendSliceState.m_reproScaleoutBuffersAmount, sendSliceState.m_boxIterations);
            }
            if (sendSliceState.m_16BitReduction)
            {
                if (!GCFG_HCL_USE_EDMA_COMMAND_V3.value())
                {
                    // bf16 v2 commands - cdc command cleans first buffer
                    memsetLoops -= 1;
                    initialOffset = bufferSize;
                }
                sizeInBytes = sizeInBytes << 1;
            }
            effectiveOp = hcclSum;
        }

        LOG_TRACE(HCL_ECR,
                  "Clear buffer {}, loops {}, size 0x{:x}, long SO {}",
                  poolId,
                  memsetLoops,
                  sizeInBytes,
                  longSo.targetValue);

        uint32_t currNumberOfRanks;
        uint32_t currNumberOfReproBuffers;

        if (poolId == REDUCE_RR_POOL)
        {
            VERIFY(recvSliceState.m_collectiveOp == eHCLReduce,
                   "REDUCE_RR_POOL is only used in eHCLReduce collectiveOp, current collectiveOp={}",
                   recvSliceState.m_collectiveOp);
            // single chunk from each peer rank on recv / single chunk to cast down after reduce
            currNumberOfRanks = 1;
            // single buffer every slice
            currNumberOfReproBuffers = 1;
        }
        else if (poolId == SCALEOUT_RR_POOL)
        {
            currNumberOfRanks = std::min(sendSliceState.m_reproScaleoutBuffersAmount, sendSliceState.m_boxIterations);
            currNumberOfReproBuffers = sendSliceState.m_reproScaleoutBuffersAmount;  // 8 buffers every slice
        }
        else
        {
            VERIFY(false, "The following pool id={} should not be used in memset.", poolId);
        }

        for (unsigned i = 0; i < memsetLoops; ++i)
        {
            serializeMemsetCommand(garbageCollectionStream,
                                   schedIdx,
                                   sendSliceState.getIntermediateBuffer(poolId) + initialOffset +
                                       i * bufferSize,  // for v3 commands, memsetLoops = 1, i = 0
                                   sizeInBytes,
                                   signalsManager->enqueueInternalCompletion(SignalEvent::EDMA_MEMSET),
                                   streamCtxtID,
                                   dataType,
                                   effectiveOp,
                                   true,  // true for sibo memset v3, false for lin memset
                                   poolId,
                                   false,  // isForScaleout
                                   currNumberOfRanks,
                                   currNumberOfReproBuffers,
                                   indexOfReproBuffer);
        }
    }
}

void HclCommandsGaudi2::serializePdmaCommand(hcl::ScalStreamBase& scalStream,
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
                                             uint32_t             sobAddr,
                                             bool                 isFirstBufferUse)
{
    SchedArcCommandsGaudi2::serializePdmaCommand(scalStream,
                                                 schedIdx,
                                                 isDownload,
                                                 hostAddress,
                                                 deviceAddress,
                                                 size,
                                                 isReduction,
                                                 isFirstBufferUse ? hcclSum : reduceOp,
                                                 isCastUp,
                                                 apiId,
                                                 streamIndex,
                                                 dataType,
                                                 sobAddr);
}