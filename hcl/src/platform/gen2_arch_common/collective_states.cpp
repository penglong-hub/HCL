#include "platform/gen2_arch_common/collective_states.h"

#include <cmath>      // for ceil
#include <algorithm>  // for max
#include <cstdint>

#include "hcl_global_conf.h"  // for GCFG_*
#include "platform/gen2_arch_common/types.h"
#include "hcl_api_types.h"
#include "hcl_dynamic_communicator.h"
#include "hcl_utils.h"
#include "platform/gen2_arch_common/device_buffer_manager.h"
#include "intermediate_buffer_container.h"
#include "hcl_log_manager.h"       // for LOG_*
#include "interfaces/hcl_unique_sorted_vector.h"
#include "platform/gen2_arch_common/hcl_address_generator.h"
#include "platform/gen2_arch_common/port_mapping.h"
#include "platform/gen2_arch_common/collective_utils.h"  // for getNextBox, getPrevBox
#include "hcl_math_utils.h"                              // for div_round_up

#define SLICE_RATIO_FIXED_POINT_ACCURACY 4
#define MAX_NUM_SLICES_SEARCH            4

CommonState::CommonState(HclCollectiveParams& other,
                         DeviceBufferManager& intermediateBufferManager,
                         bool                 isHostNic,
                         bool                 isGdr,
                         unsigned             workDistributionGroupSize,
                         const unsigned       maxNumScaleUpPortsPerConnection,
                         unsigned             numScaleOutPorts,
                         synDeviceType        deviceType,
                         RemainderCalculator* remainderCalculator)
: HclCollectiveParams(other),
  m_rootBox(m_root == HCL_INVALID_RANK ? (unsigned)-1 : m_dynamicComm.getRankToPodMap()[m_root]),
  m_isMultiPod(m_dynamicComm.isCommunicatorMultiPod()),
  m_isRoot(m_root == m_dynamicComm.getMyRank()),
  m_isRootPeer(isRootPeerExclusive(m_dynamicComm.getMyRank())),
  m_isRootBox(m_dynamicComm.getMyPod() == m_rootBox),
  m_isHostNic(isHostNic),
  m_isGdr(isGdr),
  m_workDistributionGroupSize(workDistributionGroupSize),
  m_numScaleOutPorts(numScaleOutPorts),
  m_dataTypeSizeInBytes(dataTypeSizeInBytes(m_dataType)),
  m_intermediateBufferManager(intermediateBufferManager),
  m_remainderCalculator(remainderCalculator),
  m_boxType((HclConfigType)GCFG_BOX_TYPE_ID.value()),
  m_maxNumScaleUpPortsPerConnection(maxNumScaleUpPortsPerConnection)
{
    initCollectiveOp(deviceType == synDeviceGaudi2);

    checkInPlaceOp();
    setIsReductionCollective();
    check16BitReductionOp();
    checkHierarchicalOp();
    calcMaxSliceCounts();
    calcReproScaleoutLongterm();

    m_signalsCalculator = SignalsCalculatorFactory::create(deviceType == synDeviceGaudi3);
    m_signalsCalculator->initialize(*this);
}

uint64_t CommonState::calculateCUID()
{
    struct cuid_t
    {
        union
        {
            struct
            {
                uint64_t collectiveOp : 4;   // 0..3
                uint64_t currentOp : 4;      // 4..7
                uint64_t inPlace : 1;        // 8
                uint64_t isRoot : 1;         // 9
                uint64_t isRootPeer : 1;     // 10
                uint64_t isRootBox : 1;      // 11
                uint64_t isMultiPod : 1;     // 12
                uint64_t isPeersOnly : 1;    // 13
                uint64_t isHostNic : 1;      // 14
                uint64_t isGaudiDirect : 1;  // 15
                uint64_t isFloat : 1;        // 16
                uint64_t isBf16 : 1;         // 17
                uint64_t all2allIter : 4;    // 18..21
                uint64_t comm : 16;          // 22..37
                uint64_t boxIter : 10;       // 38..47
                uint64_t reserved : 16;      // 48..63
            };
            uint64_t raw;
        };
    };

    static_assert(sizeof(cuid_t) == sizeof(uint64_t), "Size of cuid_t structure is not as expected!");

    cuid_t ret;
    ret.collectiveOp  = m_collectiveOp;
    ret.currentOp     = m_currentOp;
    ret.inPlace       = m_inPlace;
    ret.isRoot        = m_isRoot;
    ret.isRootPeer    = m_isRootPeer;
    ret.isRootBox     = m_isRootBox;
    ret.isMultiPod    = m_isMultiPod;
    ret.isPeersOnly   = (m_isMultiPod && m_dynamicComm.getPodSize() == 1);
    ret.isHostNic     = m_isHostNic;
    ret.isGaudiDirect = m_isGdr;
    ret.isFloat       = (m_dataType == hcclFloat32 || m_dataType == hcclFloat16);
    ret.isBf16        = (m_dataType == hcclBfloat16);
    ret.all2allIter   = m_all2allIter;
    ret.comm          = m_comm;
    ret.boxIter       = m_boxIter;
    ret.reserved      = 0;
    return ret.raw;
}

bool CommonState::isRoot() const
{
    return m_isRoot;
}

bool CommonState::isRootOrRootPeer() const
{
    return (m_isRoot || m_isRootPeer);
}

bool CommonState::isRootPeerInclusive(HCL_Rank rank) const
{
    return m_dynamicComm.arePeers(rank, m_root);
}

bool CommonState::isRootPeerExclusive(HCL_Rank rank) const
{
    return (isRootPeerInclusive(rank) && rank != m_root);
}

bool CommonState::isRootPeer() const
{
    return m_isRootPeer;
}

unsigned CommonState::rootBox() const
{
    return m_rootBox;
}

bool CommonState::isHostNic() const
{
    return m_isHostNic;
}

bool CommonState::isRemainderAllowedForCollective() const
{
    switch (m_collectiveOp)
    {
        case eHCLAllReduce:
        case eHCLBroadcast:
        case eHCLSinglePeerBroadcast:
        case eHCLReduce:
            return true;
        default:
            return false;
    }
}

bool CommonState::isLastBox(BoxNumInfo& boxNumInfo) const
{
    return (boxNumInfo.m_boxNum == (m_boxIterations - 1));
}

bool CommonState::isLastSlice(unsigned iterNum) const
{
    return iterNum == (m_sliceIterations - 1);
}

bool CommonState::isComplexImplementation() const
{
    switch (m_collectiveOp)
    {
        case eHCLReduceScatter:
        case eHCLAll2All:
        case eHCLAllGather:
        case eHCLGather:
        case eHCLScatter:
        case eHCLSimpleBroadcast:
        case eHCLNoCollective:
            return false;
        case eHCLAllReduce:
        case eHCLBroadcast:
        case eHCLSinglePeerBroadcast:
        case eHCLReduce:
            return true;
        case eHCLCollectiveLastValue:
            VERIFY(false);
    }
    return false;
}

bool CommonState::isSendAddrValid() const
{
    switch (m_collectiveOp)
    {
        case eHCLAllReduce:
        case eHCLAll2All:
        case eHCLAllGather:
        case eHCLReduceScatter:
        case eHCLReduce:
            return true;
        case eHCLSimpleBroadcast:
        case eHCLBroadcast:
        case eHCLSinglePeerBroadcast:
            return (isRoot() ? true : false);
        default:
            VERIFY(false, "Unknown collective opcode {}", m_collectiveOp);
            break;
    }

    return 0;
}

bool CommonState::isRecvAddrValid() const
{
    switch (m_collectiveOp)
    {
        case eHCLAllReduce:
        case eHCLBroadcast:
        case eHCLSinglePeerBroadcast:
        case eHCLSimpleBroadcast:
        case eHCLAll2All:
        case eHCLAllGather:
        case eHCLReduceScatter:
            return true;
        case eHCLReduce:
            return (isRoot() ? true : false);
        default:
            VERIFY(false, "Unknown collective opcode {}", m_collectiveOp);
            break;
    }

    return 0;
}

bool CommonState::isEdgeIteration(BoxNumInfo& boxNumInfo) const
{
    return calcBoxIterRecv(boxNumInfo) + m_reproScaleoutBuffersAmount >= m_boxIterations;
}

unsigned CommonState::calcBoxIterRecv(BoxNumInfo& boxNumInfo) const
{
    unsigned boxIter = m_boxIterations + m_dynamicComm.getMyPod() - boxNumInfo.m_boxNum;
    if (boxIter >= m_boxIterations)
    {
        boxIter -= m_boxIterations;
    }
    return boxIter;
}

uint64_t CommonState::calcSendAddrSize() const
{
    uint64_t countSize = m_dataTypeSizeInBytes * m_count;

    switch (m_collectiveOp)
    {
        case eHCLReduce:
        case eHCLAllReduce:
        case eHCLBroadcast:
        case eHCLSinglePeerBroadcast:
        case eHCLSimpleBroadcast:
        case eHCLNoCollective:
        case eHCLAll2All:
        case eHCLAllGather:      // In AG count is sendCount
        case eHCLReduceScatter:  // In RS count is sendCount
            return countSize;
        default:
            break;
    }

    VERIFY(false, "Unknown collective opcode {}", m_collectiveOp);
    return 0;
}

uint64_t CommonState::calcRecvAddrSize() const
{
    uint64_t countSize = m_dataTypeSizeInBytes * m_count;

    switch (m_collectiveOp)
    {
        case eHCLReduce:
        case eHCLAllReduce:
        case eHCLBroadcast:
        case eHCLSinglePeerBroadcast:
        case eHCLSimpleBroadcast:
        case eHCLNoCollective:
        case eHCLAll2All:
            return countSize;
        case eHCLAllGather:
            return countSize * m_dynamicComm.getCommSize();  // In AG count is sendCount
        case eHCLReduceScatter:
            return countSize / m_dynamicComm.getCommSize();  // In RS count is sendCount
        default:
            break;
    }

    VERIFY(false, "unknown collective opcode {}", m_collectiveOp);
    return 0;
}

void CommonState::initializeSignalsCalculator()
{
    m_signalsCalculator->initialize(*this);
}

unsigned CommonState::countSignalsSingleOp() const
{
    size_t commSize = m_dynamicComm.getInnerRanksInclusive().size();
    if (m_collectiveOp == eHCLSinglePeerBroadcast && !isRootOrRootPeer())
    {
        if (m_currentOp == eHCLAllGather)
        {
            return m_maxNumScaleUpPortsPerConnection * (commSize - 2);
        }
        else  // eHCLScatter
        {
            return m_maxNumScaleUpPortsPerConnection;
        }
    }
    else if (((m_currentOp == eHCLSimpleBroadcast || m_currentOp == eHCLGather) && !isRootOrRootPeer()) ||
             (m_collectiveOp == eHCLBroadcast && m_currentOp == eHCLScatter && !isRoot()))
    {
        return m_maxNumScaleUpPortsPerConnection;
    }

    return m_maxNumScaleUpPortsPerConnection * (commSize - 1);
}

uint64_t CommonState::getIntermediateBuffer(e_devicePoolID poolIndex)
{
    return m_intermediateBufferManager.getCurrentBuffer(poolIndex);
}

void CommonState::initCollectiveOp(const bool singlePeerBroadcastAllowed)
{
    if (m_collectiveOp == eHCLBroadcast)
    {
        if ((m_count * m_dataTypeSizeInBytes) <= GCFG_HCL_COMPLEX_BCAST_MIN_SIZE.value() ||
            m_dynamicComm.getPodSize() <= 2)
        {
            m_collectiveOp = eHCLSimpleBroadcast;
        }
        else if (singlePeerBroadcastAllowed && (GCFG_HCL_USE_SINGLE_PEER_BROADCAST.value() || !m_isMultiPod))
        {
            m_collectiveOp = eHCLSinglePeerBroadcast;
        }
    }
}

void CommonState::initCurrentOp(HCL_CollectiveOp currentOp, unsigned boxIter, unsigned all2allIter)
{
    m_currentOp   = currentOp;
    m_boxIter     = boxIter;
    m_all2allIter = all2allIter;

    m_cuid = calculateCUID();
    m_signalsCalculator->initialize(*this);
    determineSyncUpBufferWithLtu();
}

bool CommonState::isLongtermGPSORequired(const unsigned boxIter)
{
    const bool isSelfBox = boxIter == 0;

    switch (m_collectiveOp)
    {
        case eHCLBroadcast:
            return m_currentOp == eHCLScatter && !isRoot() &&
                   ((m_dynamicComm.getMyPod() == rootBox() && isSelfBox) ||
                    (m_dynamicComm.getMyPod() != rootBox() && boxIter == 1));
            break;

        case eHCLSinglePeerBroadcast:
            return m_currentOp == eHCLScatter && !isRootOrRootPeer() &&
                   ((m_dynamicComm.getMyPod() == rootBox() && isSelfBox) ||
                    (m_dynamicComm.getMyPod() != rootBox() && boxIter == 1));
            break;

        case eHCLReduce:
            return m_currentOp == eHCLReduceScatter && isSelfBox && (m_isMultiPod || !m_isRoot);
            break;

        case eHCLReduceScatter:
            if (m_currentOp == eHCLReduceScatter && m_isMultiPod && isSelfBox)
            {
                return true;
            }
            break;

        case eHCLAllReduce:
            return m_currentOp == eHCLReduceScatter && isSelfBox;
            break;

        case eHCLAll2All:
            return m_all2allIterations > 1 && !isSelfBox && m_all2allIter == 0;
            break;

        default:
            break;
    }

    return false;
}

unsigned CommonState::calcLongtermContinuousTarget(const unsigned boxIter)
{
    unsigned continuousTarget;
    switch (m_collectiveOp)
    {
        case eHCLBroadcast:
            // we Scatter in the root box and next box only, so we need long term gpso for not more than 2 iterations
            continuousTarget = getBroadcastScatterOpBoxIterations();
            if (!m_isRootBox) --continuousTarget;
            break;
        case eHCLSinglePeerBroadcast:
            continuousTarget = 1;
            break;
        case eHCLReduceScatter:
            continuousTarget = m_boxIterations - 1 - boxIter;
            break;
        case eHCLReduce:
        {
            // uint16_t myPod = m_dynamicComm.getMyPod();
            if (m_isRootBox)
            {
                if (m_isRoot && m_isMultiPod)
                {
                    continuousTarget = m_boxIterations - 1;  // last RS
                }
                else
                {
                    continuousTarget = m_isMultiPod ? m_boxIterations : 1;  // first Gather
                }
            }
            else
            {
                // Non root boxes always execute single gather iteration, so total number of iterations is
                // (#boxes-1) for RS and 1 for Gather
                continuousTarget = m_boxIterations;
            }
            break;
        }
        case eHCLAllReduce:
            continuousTarget = m_isMultiPod ? m_boxIterations + 1 : 1;
            break;
        case eHCLAll2All:
            continuousTarget = m_all2allIterations - 1;
            break;
        default:
            continuousTarget = 0;
            break;
    }

    return continuousTarget;
}

void CommonState::calcMaxSliceCounts()
{
    uint64_t totalCountPerRank     = 0;
    uint32_t commSize              = m_dynamicComm.getCommSize();
    uint32_t podSize               = m_dynamicComm.getPodSize();
    uint32_t numParticipatingRanks = commSize;  // #ranks which divide m_count between them
    uint64_t sliceSize             = m_dynamicComm.getSliceSize();

    m_optimalBufferCount = div(sliceSize, (uint64_t)m_dataTypeSizeInBytes);

    switch (m_collectiveOp)
    {
        case eHCLSimpleBroadcast:
            m_scaleUpStrideCount = m_count;
            m_boxStrideCount     = 0;
            totalCountPerRank    = m_count;
            break;

        case eHCLScatter:
            m_scaleUpStrideCount = div(m_count, (uint64_t)numParticipatingRanks);
            m_boxStrideCount     = m_optimalBufferCount * podSize;
            totalCountPerRank    = m_scaleUpStrideCount;
            break;

        case eHCLGather:
        case eHCLAllGather:
            m_scaleUpStrideCount = m_count;
            m_boxStrideCount     = m_count * podSize;
            totalCountPerRank    = m_count;
            break;

        case eHCLBroadcast:
            numParticipatingRanks = podSize;
            m_scaleUpStrideCount  = m_count;  // gives an upper bond
            m_boxStrideCount      = 0;        // doesn't matter here, since we are working on the same data on all boxes
            totalCountPerRank     = m_remainderCalculator->getDiv(m_count, numParticipatingRanks);
            break;

        case eHCLSinglePeerBroadcast:
            numParticipatingRanks = podSize - 1;
            if (m_isHostNic && m_isMultiPod)
            {
                m_optimalBufferCount = div(m_optimalBufferCount, (uint64_t)numParticipatingRanks);
            }
            m_scaleUpStrideCount = m_optimalBufferCount;
            m_boxStrideCount     = 0;
            totalCountPerRank    = m_remainderCalculator->getDiv(m_count, numParticipatingRanks);
            break;

        case eHCLReduce:
        case eHCLAllReduce:
            m_scaleUpStrideCount = m_optimalBufferCount;
            totalCountPerRank    = m_remainderCalculator->getDiv(m_count, numParticipatingRanks);
            m_boxStrideCount     = totalCountPerRank * podSize;
            break;

        case eHCLAll2All:
            m_scaleUpStrideCount = div(m_count, (uint64_t)numParticipatingRanks);
            m_boxStrideCount     = m_scaleUpStrideCount * podSize;
            totalCountPerRank    = m_scaleUpStrideCount;
            break;

        case eHCLReduceScatter:
            m_scaleUpStrideCount = div(m_count, (uint64_t)numParticipatingRanks);
            m_boxStrideCount     = m_scaleUpStrideCount * podSize;
            totalCountPerRank    = m_scaleUpStrideCount;
            break;

        case eHCLNoCollective:
            m_scaleUpStrideCount = 0;
            m_boxStrideCount     = 0;
            totalCountPerRank    = m_count;
            break;

        case eHCLCollectiveLastValue:
            VERIFY(false, "invalid collective operation value [{}] for {}", (int)m_collectiveOp, __func__);
            break;
    }

    m_isSlicing = m_remainderCalculator->isSlicing(m_count,
                                                   totalCountPerRank,
                                                   m_optimalBufferCount,
                                                   numParticipatingRanks);

    if (!m_isSlicing)
    {
        m_sliceIterations = 1;
        return;
    }

    m_sliceIterations = getNumSlices(totalCountPerRank, numParticipatingRanks);

    LOG_TRACE(HCL_ECR, "Counts for #slices: op {} count {} comm size {} slices {} optimal buffer count {}",
              m_collectiveOp, m_count, commSize, m_sliceIterations, m_optimalBufferCount);

    if (m_collectiveOp == eHCLAll2All)
    {
        m_all2allIterations      = podSize;
        m_all2allIterStrideCount = m_rankScaleUpCount;
    }

    switch (m_collectiveOp)
    {
        case eHCLSimpleBroadcast:
            m_rankScaleUpCount   = m_optimalBufferCount;
            m_rankScaleOutCount  = m_rankScaleUpCount;
            m_sliceOffsetCount   = m_optimalBufferCount;
            m_boxCount           = m_optimalBufferCount;
            break;

        case eHCLScatter:
            m_rankScaleUpCount   = m_optimalBufferCount;
            m_sliceOffsetCount   = m_optimalBufferCount;
            m_boxStrideCount     = m_scaleUpStrideCount * podSize;
            m_boxCount           = m_rankScaleUpCount * podSize;
            m_rankScaleOutCount  = m_boxCount;
            break;

        case eHCLGather:
        case eHCLAllGather:
            m_rankScaleUpCount   = m_optimalBufferCount;
            m_rankScaleOutCount  = m_rankScaleUpCount;
            m_sliceOffsetCount   = m_optimalBufferCount;
            m_boxCount           = m_optimalBufferCount * podSize;
            break;

        case eHCLBroadcast:
            m_rankScaleUpCount   = m_optimalBufferCount;
            m_boxCount           = m_optimalBufferCount * podSize;
            m_rankScaleOutCount  = m_rankScaleUpCount;
            m_sliceOffsetCount   = m_boxCount;
            break;

        case eHCLSinglePeerBroadcast:
            m_rankScaleUpCount   = m_optimalBufferCount;
            m_scaleUpStrideCount = m_optimalBufferCount;
            m_boxCount           = m_optimalBufferCount * (podSize - 1);
            m_rankScaleOutCount  = m_boxCount;
            m_sliceOffsetCount   = m_boxCount;
            break;

        case eHCLReduce:
        case eHCLAllReduce:
            m_rankScaleUpCount   = m_optimalBufferCount;
            m_rankScaleOutCount  = m_rankScaleUpCount;
            m_scaleUpStrideCount = m_rankScaleUpCount;
            m_boxCount           = m_optimalBufferCount * podSize;
            m_boxStrideCount     = totalCountPerRank * podSize;
            m_sliceOffsetCount   = m_scaleUpStrideCount * podSize;
            break;

        case eHCLAll2All:
            m_rankScaleUpCount   = m_optimalBufferCount;
            m_sliceOffsetCount   = m_optimalBufferCount;
            m_rankScaleOutCount  = m_rankScaleUpCount;
            m_boxCount           = m_optimalBufferCount * podSize;
            break;

        case eHCLReduceScatter:
            m_rankScaleUpCount   = m_optimalBufferCount;
            m_rankScaleOutCount  = m_rankScaleUpCount;
            m_sliceOffsetCount   = m_optimalBufferCount;
            m_boxCount           = m_optimalBufferCount * podSize;
            break;

        case eHCLNoCollective:
            m_rankScaleUpCount   = m_optimalBufferCount;
            m_rankScaleOutCount  = m_rankScaleUpCount;
            m_boxCount           = m_optimalBufferCount;
            m_sliceOffsetCount   = m_rankScaleUpCount;
            break;

        case eHCLCollectiveLastValue:
            VERIFY(false, "invalid collective operation value [{}] for {}", (int)m_collectiveOp, __func__);
            break;
    }
}

/*
 * calculate number of slices with the criteria of equal slices as much as possible
 */
uint32_t CommonState::getNumSlices(uint64_t totalRankCount, uint32_t numRanks)
{
    uint32_t originalBufferCount = (uint32_t)m_optimalBufferCount;
    uint32_t minBufferCount      = (uint32_t)div(m_optimalBufferCount, GCFG_HCL_MIN_IMB_SIZE_FACTOR.value());
    uint32_t minSlices           = div_round_up(totalRankCount, m_optimalBufferCount);
    uint32_t maxSlices           = minSlices + MAX_NUM_SLICES_SEARCH;;
    uint32_t numSlices           = 0;
    uint32_t minSliceRatio       = m_optimalBufferCount << SLICE_RATIO_FIXED_POINT_ACCURACY;
    uint32_t lastSliceCount;
    uint32_t sliceRatio;
    uint32_t sliceCount;
    uint32_t numSlicesToCheck;
    uint32_t sliceCountNotRounded;
    uint64_t sumSlices;

    // must be slicing when calling this function
    if (minSlices == 1)
    {
        minSlices = 2;
        maxSlices++;
    }

    // check first slicing with max buffer size
    if (m_remainderCalculator->isValidSlicing((uint32_t)m_optimalBufferCount,
                                              (uint32_t)m_optimalBufferCount,
                                              m_count,
                                              minSlices,
                                              numRanks,
                                              0))
    {
        numSlices      = minSlices;
        lastSliceCount = totalRankCount - (m_optimalBufferCount * (minSlices - 1));
        minSliceRatio  = div(m_optimalBufferCount << SLICE_RATIO_FIXED_POINT_ACCURACY, (uint64_t)lastSliceCount);
    }

    for (numSlicesToCheck = minSlices; numSlicesToCheck < maxSlices; numSlicesToCheck++)
    {
        // first get rough slice count according to #slices
        sliceCountNotRounded = div_round_up(totalRankCount, numSlicesToCheck);
        // next round up to comm size so slices other than last slice won't have remainder
        sliceCount           = div_round_up(sliceCountNotRounded, numRanks) * numRanks;
        sumSlices            = sliceCount * (numSlicesToCheck - 1);
        // if rounding up results in last slice count <= 0 -> invalid, continue to next #slices
        if (totalRankCount <= sumSlices)
        {
            continue;
        }
        lastSliceCount = totalRankCount - sumSlices;
        if (m_remainderCalculator
                ->isValidSlicing(originalBufferCount,
                                 sliceCount,
                                 m_count,
                                 numSlicesToCheck,
                                 numRanks,
                                 minBufferCount))
        {
            sliceRatio = div(sliceCount << SLICE_RATIO_FIXED_POINT_ACCURACY, lastSliceCount);
            if (sliceRatio < minSliceRatio)
            {
                minSliceRatio        = sliceRatio;
                numSlices            = numSlicesToCheck;
                m_optimalBufferCount = sliceCount;
            }
        }
        // slicing results in a too small buffer - no need to check higher slicing
        else if (sliceCount < minBufferCount || minSliceRatio == (1 << SLICE_RATIO_FIXED_POINT_ACCURACY))
        {
            break;
        }
    }

    VERIFY(numSlices > 1, "Not found optimal buffer size. op {} count {} num Ranks {} optimal buffer count {}",
           m_collectiveOp, m_count, numRanks, m_optimalBufferCount);

    return numSlices;
}

void CommonState::calcSliceCounts(unsigned sliceIter)
{
    if (sliceIter == (m_sliceIterations - 1))
    {
        uint64_t podSize  = m_dynamicComm.getPodSize();
        uint64_t commSize = m_dynamicComm.getCommSize();
        uint64_t totalCountForLastSlice;
        switch (m_collectiveOp)
        {
            case eHCLSimpleBroadcast:
            case eHCLNoCollective:
                totalCountForLastSlice = m_count - (m_rankScaleUpCount * (m_sliceIterations - 1));
                m_rankScaleUpCount     = totalCountForLastSlice;
                m_rankScaleOutCount    = totalCountForLastSlice;
                m_boxCount             = totalCountForLastSlice;
                break;

            case eHCLGather:
            case eHCLAllGather:
                totalCountForLastSlice = m_count - (m_rankScaleUpCount * (m_sliceIterations - 1));
                m_rankScaleUpCount     = totalCountForLastSlice;
                m_rankScaleOutCount    = totalCountForLastSlice;
                m_boxCount             = totalCountForLastSlice * podSize;
                break;

            case eHCLBroadcast:
                totalCountForLastSlice = m_count - (m_boxCount * (m_sliceIterations - 1));
                m_rankScaleUpCount     = m_remainderCalculator->getDiv(totalCountForLastSlice, podSize);
                m_rankScaleOutCount    = m_rankScaleUpCount;
                m_scaleUpStrideCount   = m_rankScaleUpCount;
                m_boxCount             = totalCountForLastSlice;
                m_boxStrideCount       = 0;
                m_remainderCount =
                    m_remainderCalculator->getRemainderCount(totalCountForLastSlice, m_rankScaleUpCount, podSize);

                break;

            case eHCLSinglePeerBroadcast:
                totalCountForLastSlice = m_count - (m_boxCount * (m_sliceIterations - 1));
                m_rankScaleUpCount     = m_remainderCalculator->getDiv(totalCountForLastSlice, podSize - 1);
                m_scaleUpStrideCount   = m_rankScaleUpCount;
                m_boxCount             = totalCountForLastSlice;
                m_boxStrideCount       = 0;
                m_rankScaleOutCount    = m_boxCount;
                break;

            case eHCLScatter:
            {
                totalCountForLastSlice = m_count - (m_rankScaleUpCount * commSize * (m_sliceIterations - 1));
                m_rankScaleUpCount     = totalCountForLastSlice / commSize;
                m_boxCount             = m_rankScaleUpCount * podSize;
                m_rankScaleOutCount    = m_boxCount;
                break;
            }

            case eHCLAll2All:
                totalCountForLastSlice = (m_count / commSize) - (m_rankScaleUpCount * (m_sliceIterations - 1));
                m_rankScaleUpCount     = totalCountForLastSlice;
                m_boxCount             = m_rankScaleUpCount * podSize;
                m_rankScaleOutCount    = m_rankScaleUpCount;
                if (m_isHostNic && !m_isSlicing)
                {
                    m_all2allIterations      = div_round_up(m_rankScaleUpCount * podSize, m_optimalBufferCount);
                    m_all2allIterStrideCount = m_optimalBufferCount;
                }
                break;
            case eHCLReduceScatter:
                totalCountForLastSlice = m_count - (m_boxCount * m_boxIterations * (m_sliceIterations - 1));
                m_rankScaleUpCount     = totalCountForLastSlice / commSize;
                m_boxCount             = m_rankScaleUpCount * podSize;
                m_rankScaleOutCount    = m_rankScaleUpCount;
                break;

            case eHCLReduce:
            case eHCLAllReduce:
            {
                totalCountForLastSlice = m_count - (m_boxCount * m_boxIterations * (m_sliceIterations - 1));
                m_rankScaleUpCount     = m_remainderCalculator->getDiv(totalCountForLastSlice, commSize);
                m_remainderCount       = m_remainderCalculator->getRemainderCount(totalCountForLastSlice,
                                                                                  m_rankScaleUpCount,
                                                                                  commSize);
                m_rankScaleOutCount    = m_rankScaleUpCount;
                m_scaleUpStrideCount   = m_rankScaleUpCount;
                m_boxCount             = m_rankScaleUpCount * podSize;
                break;
            }
            case eHCLCollectiveLastValue:
                VERIFY(false, "invalid collective operation value [{}] for {}", (int)m_collectiveOp, __func__);
                break;
        }

        if (isRemainderAllowedForCollective())
        {
            m_hasBufferSize = m_boxCount != (m_rankScaleUpCount * podSize);
        }
        else
        {
            m_hasBufferSize = false;
        }
    }
}

uint64_t CommonState::getAddressOffset(unsigned int iterNum)
{
    return iterNum * m_sliceOffsetCount * m_dataTypeSizeInBytes;
}

uint64_t CommonState::getChunkCount()
{
    return m_rankScaleUpCount;
}

uint64_t CommonState::getChunkCountToClear()
{
    uint64_t all2allCorrection = (m_collectiveOp == eHCLAll2All ? m_dynamicComm.getPodSize() : 1);
    return m_rankScaleUpCount * all2allCorrection;
}

uint64_t CommonState::getStrideCount()
{
    if (isComplexImplementation())
    {
        return m_rankScaleUpCount;
    }
    return m_scaleUpStrideCount;
}

uint64_t CommonState::getSendAddress(unsigned int iterNum)
{
    return m_sendBufferAddr + getAddressOffset(iterNum);
}

uint64_t CommonState::getRecvAddress(unsigned int iterNum)
{
    return m_recvBufferAddr + getAddressOffset(iterNum);
}

uint8_t CommonState::getQpSet()
{
    return m_qpSet;
}

void CommonState::checkInPlaceOp()
{
    uint32_t commSize = m_dynamicComm.getCommSize();
    HCL_Rank myRank   = m_dynamicComm.getMyRank();
    uint64_t dataSize = m_count * m_dataTypeSizeInBytes;
    uint64_t bufferOffset;

    switch (m_collectiveOp)
    {
        case eHCLReduceScatter:
            bufferOffset = (dataSize / commSize) * myRank;
            m_inPlace    = (m_recvBufferAddr == m_sendBufferAddr + bufferOffset);
            break;

        case eHCLAllGather:
            bufferOffset = dataSize * myRank;
            m_inPlace    = (m_sendBufferAddr == m_recvBufferAddr + bufferOffset);
            break;

        case eHCLGather:
        case eHCLAllReduce:
        case eHCLSimpleBroadcast:
        case eHCLBroadcast:
        case eHCLSinglePeerBroadcast:
            m_inPlace = (m_sendBufferAddr == m_recvBufferAddr);
            break;

        case eHCLReduce:
            // no inplace for bf16 Reduce collective - same graph
            if (m_dataType == hcclBfloat16 || m_dataType == hcclFloat16 || m_dynamicComm.isCommunicatorMultiPod())
            {
                m_inPlace = false;
            }
            else
            {
                m_inPlace = (m_sendBufferAddr == m_recvBufferAddr);
            }
            break;

        case eHCLAll2All:
            VERIFY(m_sendBufferAddr != m_recvBufferAddr,
                   "All2All in place operation is unsupported (sendBuffer ({:x}), recvBuffer ({:x}))",
                   m_sendBufferAddr,
                   m_recvBufferAddr);
            m_inPlace = false;
            break;

        case eHCLNoCollective:
            m_inPlace = false;
            break;

        default:
            VERIFY(false, "Unsupported operation [{}] in checkInPlaceOp", (int)m_collectiveOp);
    }
}

void CommonState::setIsReductionCollective()
{
    m_isReductionCollective =
        (m_collectiveOp == eHCLReduceScatter || m_collectiveOp == eHCLAllReduce || m_collectiveOp == eHCLReduce);
}

void CommonState::check16BitReductionOp()
{
    m_16BitReduction = (m_isReductionCollective && (m_dataType == hcclBfloat16 || m_dataType == hcclFloat16));
}

void CommonState::calcReproScaleoutLongterm()
{
    if (m_isMultiPod &&
        (m_collectiveOp == eHCLReduceScatter || m_collectiveOp == eHCLAllReduce || m_collectiveOp == eHCLReduce))
    {
        m_reproScaleoutLongtermAmount = (m_reproScaleoutBuffersAmount >= m_boxIterations)
                                            ? 1
                                            : (2 * m_reproScaleoutBuffersAmount >= m_boxIterations
                                                   ? (m_boxIterations + 1 - m_reproScaleoutBuffersAmount)
                                                   : m_reproScaleoutBuffersAmount + 1);
    }
    else
    {
        // Default, doesn't mean necessarily that a longterm gpso will be allocated.
        m_reproScaleoutLongtermAmount = 1;
    }

    VERIFY(m_reproScaleoutLongtermAmount <= m_reproScaleoutBuffersAmount + 1);
}

void CommonState::determineSyncUpBufferWithLtu()
{
    m_syncUpBufferWithLtu =
        m_isMultiPod && m_currentOp == eHCLReduceScatter && !isHostNic() && m_dynamicComm.getPodSize() > 1;
}

void CommonState::checkHierarchicalOp()
{
    if (!m_isMultiPod)
    {
        m_boxIterations = 1;
        m_boxStrideCount     = 0;
        return;
    }
    else if (eHCLNoCollective == m_collectiveOp)
    {
        m_boxIterations = 1;
        m_boxStrideCount     = 0;
        return;
    }

    m_boxIterations = m_dynamicComm.m_commSize / m_dynamicComm.getPodSize();
}

bool CommonState::isScaleoutRequired(bool isSend, BoxNumInfo& sendBoxNumInfo)
{
    // no scaleout on first box iteration
    if (sendBoxNumInfo.m_boxNum == m_dynamicComm.getMyPod())
    {
        return false;
    }
    switch (m_currentOp)
    {
        // symmetric operations have always scaleout send and recv
        case eHCLReduceScatter:
        case eHCLAll2All:
            return true;

        case eHCLAllGather:
            // AG is also symmetric, when used in broadcast algorithms, no scaleout (only in scatter)
            if (m_collectiveOp != eHCLBroadcast && m_collectiveOp != eHCLSinglePeerBroadcast)
            {
                return true;
            }
            break;
        case eHCLScatter:
        {
            unsigned myBox = m_dynamicComm.getMyPod();
            if (isSend)
            {
                // send out only to next box (box iteration 1). no send to root box
                // and in single peer broadcast only root and its peers send out
                if (sendBoxNumInfo.m_boxNum == getNextBox(myBox, m_boxIterations) &&
                    sendBoxNumInfo.m_boxNum != rootBox() &&
                    (m_collectiveOp != eHCLSinglePeerBroadcast || isRootOrRootPeer()))
                {
                    return true;
                }
            }
            else  // recv
            {
                // recv only from prev box (box iteration 1). root box doesn;t recv
                // and in single peer broadcast only root peers recv
                if (sendBoxNumInfo.m_boxNum == getPrevBox(myBox, m_boxIterations) && myBox != rootBox() &&
                    (m_collectiveOp != eHCLSinglePeerBroadcast || isRootPeer()))
                {
                    return true;
                }
            }
            break;
        }

        case eHCLGather:
            // send out only to root box
            if (isSend && sendBoxNumInfo.m_boxNum == rootBox())
            {
                return true;
            }
            // only root box recv out
            if (!isSend && m_dynamicComm.getMyPod() == rootBox())
            {
                return true;
            }
            break;

        case eHCLSimpleBroadcast:
            // in simple broadcast only root sends out
            if (isSend && m_isRoot)
            {
                return true;
            }
            // only root peers recv from root
            if (!isSend && isRootPeer() && sendBoxNumInfo.m_boxNum == rootBox())
            {
                return true;
            }
            break;

        case eHCLSinglePeerBroadcast:
        case eHCLBroadcast:
        case eHCLAllReduce:
        case eHCLReduce:
        case eHCLNoCollective:
        case eHCLCollectiveLastValue:
            VERIFY(false, "Invalid current op {} in CommonState::isScaleoutRequired", m_currentOp);
    }

    return false;
}

void CommonState::calcSliceQpSet(const unsigned sliceIter)
{
    /* Params used to calculate m_qpSet, should be symmetric between ranks */
    m_qpSet = (m_dynamicComm.getCollectiveCtr() + sliceIter) % m_dynamicComm.getMaxScaleOutQpSetsNum();
}

unsigned CommonState::getBroadcastScatterOpBoxIterations() const
{
    return std::min(m_boxIterations, 2u);
}

SliceState::SliceState(const CommonState&   commonState,
                       HclAddressGenerator& addressGenerator,
                       HCL_CollectiveOp     currentOp,
                       bool                 isSend,
                       unsigned             sliceIter,
                       BoxNumInfo           boxNumInfo,
                       int                  streamId)
: CommonState(commonState), m_isSend(isSend), m_sliceIter(sliceIter), m_boxNumInfo(boxNumInfo)
{
    m_currentOp         = currentOp;

    calcBoxAndScaleOutCounts();

    LOG_TRACE(HCL_ECR,
              "Counts for collective {}, slice {}, box num {}: box type {}: ScaleUp cell count {}, ScaleUp stride {},"
              "Box count {}, Box stride {}, ScaleOut cell count {}, slice offset count {}, has_buffer {}, "
              "collective count {}, slices {}",
              m_collectiveOp,
              sliceIter,
              m_boxNumInfo.m_boxNum,
              m_boxNumInfo.m_orientation,
              m_rankScaleUpCount,
              m_scaleUpStrideCount,
              m_boxCount,
              m_boxStrideCount,
              m_rankScaleOutCount,
              m_sliceOffsetCount,
              m_hasBufferSize,
              m_count,
              m_sliceIterations);

    if (!m_isMultiPod) return;

    m_isHierarchicalFirst = (m_boxNumInfo.m_boxNum == m_dynamicComm.getMyPod());
    m_isHierarchicalLast =
        ((m_isSend ? getNextBox(m_boxNumInfo.m_boxNum, m_boxIterations)
                   : getPrevBox(m_boxNumInfo.m_boxNum, m_boxIterations)) == m_dynamicComm.getMyPod());

    m_execution.m_deviceCount = m_boxStrideCount;
    m_execution.m_cellCount   = m_rankScaleOutCount;

    if (m_collectiveOp == eHCLAll2All && !m_isSlicing)
    {
        if (isHostNic())
        {
            // Since in HNIC all2all we use SCALEUP_RR_AND_ALL2ALL_POOL IMB as the slicing factor, in some cases data
            // stored in this IMB, Can be larger than the Host buffer size, so we will break iteration to multiple
            // all2all iteration so that the data will fit into the Host buffer (last all2all iteration can be smaller
            // than the other iterations)
            const uint64_t maxCountPerIMB = m_optimalBufferCount;
            m_execution.m_cellCount =
                std::min(maxCountPerIMB,
                         (m_rankScaleUpCount * m_dynamicComm.getPodSize()) - (maxCountPerIMB * m_all2allIter));
        }
        else
        {
            m_execution.m_cellCount *= m_dynamicComm.getPodSize();
        }
    }

    if (isComplexImplementation() || (m_isSend && m_collectiveOp == eHCLAll2All && m_isSlicing))
    {
        m_execution.m_strideCount = m_rankScaleUpCount;
    }
    else
    {
        m_execution.m_strideCount = m_scaleUpStrideCount;
    }

    uint64_t offset = m_dynamicComm.getRankInPod() * m_execution.m_strideCount * m_dataTypeSizeInBytes;

    if (!m_isHierarchicalFirst)
    {
        if (m_isSend)
        {
            m_execution.m_deviceAddress =
                addressGenerator.generateScaleOutSendAddress(*this, m_sliceIter, m_boxNumInfo, m_currentOp, offset);
        }
        else
        {
            m_execution.m_deviceAddress =
                addressGenerator.generateScaleOutRecvAddress(*this, m_sliceIter, m_boxNumInfo, m_currentOp, offset);
        }
    }
}

void SliceState::calcBoxAndScaleOutCounts()
{
    if (m_sliceIter == (m_sliceIterations - 1))
    {
        uint64_t podSize  = m_dynamicComm.getPodSize();
        switch (m_collectiveOp)
        {
            case eHCLReduce:
            case eHCLAllReduce:
            {
                HCL_Rank myRankInPod = m_dynamicComm.getRankInPod();
                unsigned boxIndex    = m_dynamicComm.getMyPod();
                bool     isLastRankInPod = m_dynamicComm.isLastRankInPod();

                if ((m_currentOp == eHCLReduceScatter && m_isSend) || (m_currentOp != eHCLReduceScatter && !m_isSend))
                {
                    boxIndex = m_boxNumInfo.m_boxNum;
                }

                m_boxCount          = m_remainderCalculator->getBoxCount(m_boxCount,
                                                                         m_boxIterations,
                                                                         podSize,
                                                                         boxIndex,
                                                                         m_rankScaleOutCount,
                                                                         m_remainderCount);
                m_rankScaleOutCount = m_remainderCalculator->getScaleOutCount(m_rankScaleOutCount,
                                                                              m_boxIterations,
                                                                              m_boxCount,
                                                                              boxIndex,
                                                                              myRankInPod,
                                                                              m_rankScaleUpCount,
                                                                              m_remainderCount,
                                                                              isLastRankInPod);

                break;
            }
            case eHCLBroadcast:
            {
                HCL_Rank myRankInPod     = m_dynamicComm.getRankInPod();
                bool     isLastRankInPod = m_dynamicComm.isLastRankInPod();

                // for broadcast we split data between ranks in pod rather then all ranks in comm,
                // so every box is treated like last box, hence setting boxIndex = 0 and numBoxes = 1
                m_rankScaleOutCount = m_remainderCalculator->getScaleOutCount(m_rankScaleOutCount,
                                                                              1,
                                                                              m_boxCount,
                                                                              0,
                                                                              myRankInPod,
                                                                              m_rankScaleUpCount,
                                                                              m_remainderCount,
                                                                              isLastRankInPod);
                break;
            }
            case eHCLSimpleBroadcast:
            case eHCLNoCollective:
            case eHCLGather:
            case eHCLAllGather:
            case eHCLSinglePeerBroadcast:
            case eHCLScatter:
            case eHCLAll2All:
            case eHCLReduceScatter:
                break;
            case eHCLCollectiveLastValue:
                VERIFY(false, "invalid collective operation value [{}] for {}", (int)m_collectiveOp, __func__);
                break;
        }

        if (isRemainderAllowedForCollective())
        {
            m_hasBufferSize = m_boxCount != (m_rankScaleUpCount * podSize);
        }
        else
        {
            m_hasBufferSize = false;
        }
    }
}

// check if AllReduce-AllGather or Reduce-Gather, need to wait for ReduceScatter to finish
bool SliceState::gatherOpsWaitForRS(bool isScaleup)
{
    bool     AGWaitForRS;
    bool     GatherWaitForRS;
    unsigned myPod = m_dynamicComm.getMyPod();

    if (isScaleup)
    {
        AGWaitForRS = m_collectiveOp == eHCLAllReduce && m_currentOp == eHCLAllGather &&
                      (!m_isMultiPod || m_boxNumInfo.m_boxNum == myPod);

        GatherWaitForRS = m_collectiveOp == eHCLReduce && m_currentOp == eHCLGather && myPod == m_rootBox && !m_isRoot;
    }
    else  // scaleout
    {
        AGWaitForRS = m_collectiveOp == eHCLAllReduce && m_currentOp == eHCLAllGather && m_isMultiPod &&
                      getPrevBox(m_boxNumInfo.m_boxNum, m_boxIterations) == myPod;
        GatherWaitForRS = m_collectiveOp == eHCLReduce && m_currentOp == eHCLGather && m_isMultiPod &&
                          m_boxNumInfo.m_boxNum == m_rootBox && myPod != m_rootBox;
    }

    return AGWaitForRS || GatherWaitForRS;
}

NonCollectiveState::NonCollectiveState(const CommonState&   commonState,
                                       HclAddressGenerator& addressGenerator,
                                       const bool           isSend,
                                       const uint32_t       completionSoAddr,
                                       const bool           isAnyScaleoutRequired)
: CommonState(commonState),
  m_isSend(isSend),
  m_completionSoAddr(completionSoAddr),
  m_addressGenerator(addressGenerator),
  m_isScaleoutRequired(isAnyScaleoutRequired)
{
}

void NonCollectiveState::updateState(const unsigned       remoteBox,
                                     const HCL_Rank       remoteRank,
                                     const hcclDataType_t dataType,
                                     const uint64_t       deviceAddress,
                                     const uint64_t       count,
                                     const bool           firstRank,
                                     const unsigned int   recvFenceValue,
                                     const uint64_t       hostMappedAddr,
                                     const uint64_t       hostAddr)
{
    LOG_HCL_TRACE(HCL,
                  "remoteBox={}, remoteRank={}, dataType={}, deviceAddress=0x{:x}, count={}, firstRank={}, "
                  "recvFenceValue={}, hostMappedAddr=0x{:x}, hostAddr=0x{:x}",
                  remoteBox,
                  remoteRank,
                  dataType,
                  deviceAddress,
                  count,
                  firstRank,
                  recvFenceValue,
                  hostMappedAddr,
                  hostAddr);
    m_dataType   = dataType;
    m_remoteBox  = remoteBox;
    m_remoteRank = remoteRank;
    m_firstRank  = firstRank;

    BoxNumInfo remoteBoxNumInfo(remoteBox,
                                m_isSend ? BoxNumInfo::boxOrientation::NEXT_BOX : BoxNumInfo::boxOrientation::PREV_BOX);
    if (m_isSend)
    {
        m_sendBufferAddr            = deviceAddress;
        m_execution.m_deviceAddress = m_addressGenerator.generateScaleOutSendAddress(*this,
                                                                                     0 /* sliceIter, not used */,
                                                                                     remoteBoxNumInfo,
                                                                                     eHCLNoCollective,
                                                                                     0 /* offset, not used */);
    }
    else
    {
        m_recvBufferAddr            = deviceAddress;
        m_recvFenceValue            = recvFenceValue;
        m_execution.m_deviceAddress = m_addressGenerator.generateScaleOutRecvAddress(*this,
                                                                                     0 /* sliceIter, not used */,
                                                                                     remoteBoxNumInfo,
                                                                                     eHCLNoCollective,
                                                                                     0 /* offset, not used */);
    }
    m_execution.m_deviceCount = count;
    m_hostMappedAddr          = hostMappedAddr;
    m_hostAddr                = hostAddr;

    LOG_HCL_TRACE(HCL,
                  "remoteBox={}, boxType={}, remoteRank={}, m_hostMappedAddr=0x{:x}, m_hostAddr=0x{:x}",
                  remoteBoxNumInfo.m_boxNum,
                  remoteBoxNumInfo.m_orientation,
                  remoteRank,
                  m_hostMappedAddr,
                  m_hostAddr);
}

bool NonCollectiveState::isScaleOutRequired() const
{
    return m_isScaleoutRequired;  // TODO: needs to update once moved inside iteration loop and credits SW115278
}

void NonCollectiveState::calcSliceQpSet(const unsigned sliceIter)
{
    /* Params used to calculate m_qpSet, should be symmetric between ranks */
    m_qpSet = (sliceIter) % m_dynamicComm.getMaxScaleOutQpSetsNum();
}