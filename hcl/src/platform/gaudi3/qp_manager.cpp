#include "qp_manager.h"

#include <ext/alloc_traits.h>  // for __alloc_traits<>::value...
#include <algorithm>           // for max
#include <cstdint>             // for uint32_t, uint8_t

#include "hcl_utils.h"                   // for VERIFY
#include "platform/gaudi3/hal.h"         // for Gaudi3Hal
#include "platform/gaudi3/hcl_device.h"  // for HclDeviceGaudi3
#include "platform/gaudi3/commands/hcl_commands.h"
#include "hcl_math_utils.h"
#include "platform/gen2_arch_common/server_connectivity.h"    // for Gen2ArchServerConnectivity
#include "platform/gaudi3/gaudi3_base_server_connectivity.h"  // for Gaudi3BaseServerConnectivity

QPManagerGaudi3::QPManagerGaudi3(HclDeviceGaudi3& device) : QPManager(device)
{
    m_maxQPsPerConnection = m_device.getHal()->getMaxQPsPerNic();
    VERIFY(m_maxQPsPerConnection == MAX_QPS_PER_CONNECTION_G3);
}

uint32_t QPManagerGaudi3::getQPi(const HCL_CollectiveOp collectiveOp, const bool isSend)
{
    switch (collectiveOp)
    {
        case eHCLReduceScatter:
            return isSend ? G3::QP_e::QPE_RS_SEND : G3::QP_e::QPE_RS_RECV;
            break;
        case eHCLAllGather:
            return isSend ? G3::QP_e::QPE_AG_SEND : G3::QP_e::QPE_AG_RECV;
            break;
        case eHCLAll2All:
            return isSend ? G3::QP_e::QPE_A2A_SEND : G3::QP_e::QPE_A2A_RECV;
            break;
        default:
            VERIFY(false, "invalid op({})", collectiveOp);
    }

    VERIFY(false, "unreachable code");
    return 0;
}

uint32_t QPManagerGaudi3::getDestQPi(const unsigned qpi) const
{
    switch (qpi)
    {
        case G3::QP_e::QPE_RS_RECV:
            return G3::QP_e::QPE_RS_SEND;
            break;
        case G3::QP_e::QPE_AG_RECV:
            return G3::QP_e::QPE_AG_SEND;
            break;
        case G3::QP_e::QPE_RS_SEND:
            return G3::QP_e::QPE_RS_RECV;
            break;
        case G3::QP_e::QPE_AG_SEND:
            return G3::QP_e::QPE_AG_RECV;
            break;
        case G3::QP_e::QPE_A2A_SEND:
            return G3::QP_e::QPE_A2A_RECV;
            break;
        case G3::QP_e::QPE_A2A_RECV:
            return G3::QP_e::QPE_A2A_SEND;
            break;
    }

    VERIFY(false, "unreachable code");

    return 0;
}

QPUsage QPManagerGaudi3::getBaseQpAndUsage(HclDynamicCommunicator& dynamicComm,
                                           HCL_CollectiveOp        collectiveOp,
                                           bool                    isSend,
                                           bool                    isComplexCollective,
                                           bool                    isReductionInIMB,
                                           bool                    isHierarchical,
                                           uint64_t                count,
                                           uint64_t                cellCount,
                                           HclConfigType           boxType,
                                           bool                    isScaleOut,
                                           HCL_Rank                remoteRank,
                                           uint8_t                 qpSet,
                                           const bool              isReduction,
                                           HCL_CollectiveOp        complexCollective,
                                           bool                    isRoot)
{
    QPUsage ret = {0, false};

    G3::QP_e qpi;
    bool     outOfBounds = count != INVALID_COUNT &&
                       ((cellCount * mod(dynamicComm.getMyRank(), dynamicComm.getScaleupGroupSize())) >= count);
    switch (collectiveOp)
    {
        case eHCLReduceScatter:
            if (isSend)
            {
                qpi = G3::QP_e::QPE_RS_SEND;
            }
            else if (isComplexCollective && !isReductionInIMB && (!isHierarchical || outOfBounds))
            {
                if (complexCollective == eHCLReduce && !isRoot && !outOfBounds)
                {
                    ret.disregardRank = true;
                }
                qpi = G3::QP_e::QPE_RS_RECV;
            }
            else if ((isComplexCollective && isReductionInIMB && outOfBounds) || isReduction)
            {
                qpi = G3::QP_e::QPE_RS_RECV;
            }
            else if (complexCollective == eHCLReduce && isRoot && !isReductionInIMB && isHierarchical)
            {
                qpi = G3::QP_e::QPE_RS_RECV;
            }
            else
            {
                qpi               = G3::QP_e::QPE_RS_RECV;
                ret.disregardRank = true;
            }
            break;
        case eHCLGather:  // FALLTHROUGH
        case eHCLAllGather:
            if (isSend)
            {
                qpi = G3::QP_e::QPE_AG_SEND;
                if (!isComplexCollective || collectiveOp == eHCLGather)
                {
                    ret.disregardRank = true;
                }
            }
            else
            {
                qpi = G3::QP_e::QPE_AG_RECV;
            }
            break;
        case eHCLAll2All:
            if (isScaleOut)
            {
                if (isSend)
                {
                    qpi = G3::QP_e::QPE_RS_SEND;
                }
                else
                {
                    qpi = G3::QP_e::QPE_RS_RECV;
                }
            }
            else
            {
                if (isSend)
                {
                    qpi = G3::QP_e::QPE_A2A_SEND;
                }
                else
                {
                    qpi = G3::QP_e::QPE_A2A_RECV;
                }
            }
            break;
        case eHCLReduce:
        case eHCLScatter:
            if (boxType == LOOPBACK) ret.disregardRank = true;
            if (isSend)
            {
                qpi = G3::QP_e::QPE_RS_SEND;
            }
            else
            {
                qpi               = G3::QP_e::QPE_RS_RECV;
                ret.disregardRank = true;
            }
            break;
        case eHCLBroadcast:            // FALLTHROUGH
        case eHCLSinglePeerBroadcast:  // FALLTHROUGH
        case eHCLSimpleBroadcast:
            if (isSend)
            {
                qpi = G3::QP_e::QPE_AG_SEND;
            }
            else
            {
                qpi = G3::QP_e::QPE_AG_RECV;
            }
            ret.disregardRank = true;
            break;
        case eHCLNoCollective:  // send recv
            if (isSend)
            {
                qpi = G3::QP_e::QPE_RS_SEND;
            }
            else
            {
                qpi = G3::QP_e::QPE_RS_RECV;
            }
            ret.disregardRank = true;
            break;
        default:
            VERIFY(false, "Cannot run collectiveOp {} on Gaudi3 device", (int)collectiveOp);
    }

    const QPManagerHints hints(dynamicComm, remoteRank, INVALID_QP, qpi, INVALID_QP, qpSet);
    ret.qpn = getQPn(hints);

    // we use offset 0 for all collective in scaleOut
    if (isScaleOut) ret.disregardRank = true;

    return ret;
}

/* ScaleUp QP Manager */

QPManagerGaudi3ScaleUp::QPManagerGaudi3ScaleUp(HclDeviceGaudi3& device) : QPManagerGaudi3(device)
{
    m_remoteRankOffsets.resize(DEFAULT_COMMUNICATORS_SIZE);
    m_myRankOffsets.resize(DEFAULT_COMMUNICATORS_SIZE);

    for (auto& commRemoteRankOffsets : m_remoteRankOffsets)
    {
        commRemoteRankOffsets.fill((uint16_t)-1);
    }
    for (auto& commMyRankOffsets : m_myRankOffsets)
    {
        commMyRankOffsets.fill((uint16_t)-1);
    }

    m_qpInfoScaleUp.resize(DEFAULT_COMMUNICATORS_SIZE);
    for (auto& qpi : m_qpInfoScaleUp)
    {
        qpi.fill(INVALID_QP);
    }
}

void QPManagerGaudi3ScaleUp::resizeDBForNewComms(const HCL_Comm comm)
{
    const size_t oldSize = m_qpInfoScaleUp.size();
    const size_t newSize = oldSize + DEFAULT_COMMUNICATORS_SIZE;

    LOG_HCL_INFO(HCL, "resizing m_qpInfoScaleUp for comm {} from {} to {}", comm, oldSize, newSize);

    m_qpInfoScaleUp.resize(newSize);
    for (unsigned index = oldSize; index < newSize; index++)
    {
        for (auto& qpn : m_qpInfoScaleUp.at(index))
        {
            qpn = INVALID_QP;
        }
    }
}

void QPManagerGaudi3ScaleUp::registerQPs(const QPManagerHints& hints, const QpsVector& qps)
{
    const HCL_Comm comm = hints.m_comm;

    VERIFY(qps.size() == m_maxQPsPerConnection,
           "Each connection should hold {} QPs but opened {} QPs for comm {}",
           m_maxQPsPerConnection,
           qps.size(),
           comm);

    if (unlikely(comm >= m_qpInfoScaleUp.size()))
    {
        resizeDBForNewComms(comm);
    }

    for (unsigned qpi = 0; qpi < m_maxQPsPerConnection; qpi++)
    {
        m_qpInfoScaleUp.at(comm).at(qpi) = qps[qpi];

        LOG_HCL_DEBUG(HCL, "m_qpInfoScaleUp[comm {}][qpi {}] = qpn {}", comm, qpi, m_qpInfoScaleUp.at(comm).at(qpi));
    }
}

void QPManagerGaudi3ScaleUp::setConfiguration(hcl::ScalStream& stream, const HCL_Comm comm, const bool isSend)
{
    for (const auto& collectiveOp : {eHCLReduceScatter, eHCLAllGather, eHCLAll2All})
    {
        setNicOffsets(stream, comm, collectiveOp, isSend);
        setLastRankScaleup(stream, comm, collectiveOp, isSend);
    }
}

uint32_t QPManagerGaudi3ScaleUp::getQPn(const QPManagerHints& hints) const
{
    const HCL_Comm comm = hints.m_comm;
    const unsigned qpi  = hints.m_qpi;

    return m_qpInfoScaleUp.at(comm).at(qpi);
}

uint32_t QPManagerGaudi3ScaleUp::getQPi(const QPManagerHints& hints) const
{
    const HCL_Comm comm = hints.m_comm;
    const unsigned nic  = hints.m_nic;
    const unsigned qpn  = hints.m_qpn;

    for (unsigned qpi = 0; qpi < m_maxQPsPerConnection; qpi++)
    {
        if (m_qpInfoScaleUp.at(comm).at(qpi) + m_device.getNicToQpOffset(nic) == qpn)
        {
            return qpi;
        }
    }

    VERIFY(false, "could not find a match for comm {} qpn {}", comm, qpn);
}

uint32_t QPManagerGaudi3ScaleUp::getLastRankPortMask(HclDynamicCommunicator& dynamicComm,
                                                     const HCL_CollectiveOp  collectiveOp,
                                                     const bool              isSend) const
{
    if ((collectiveOp == eHCLAllGather && isSend) || (collectiveOp == eHCLReduceScatter && !isSend))
    {
        const HclDeviceGaudi3& device = (const HclDeviceGaudi3&)m_device;
        return device.getServerConnectivityGaudi3().getInnerRanksPortMask(dynamicComm);
    }
    return 0;
}

void QPManagerGaudi3ScaleUp::setNicOffsets(hcl::ScalStream&       stream,
                                           const HCL_Comm         comm,
                                           const HCL_CollectiveOp collectiveOp,
                                           const bool             isSend)
{
    // for each scenario all nics use the same qpn
    const QPManagerHints hints(comm,
                               HCL_INVALID_RANK,
                               INVALID_QP,
                               QPManagerGaudi3::getQPi(collectiveOp, isSend));  // TODO: fix func call
    const uint32_t       qpn = getQPn(hints);

    LOG_HCL_TRACE(HCL, "comm={}, collectiveOp={}, qpn={}, isSend={}", comm, collectiveOp, qpn, isSend);

    // get nic to remote rank index map
    std::array<uint16_t, MAX_NICS_GEN2ARCH>& remoteIndices = getRemoteRankIndices(comm, collectiveOp, isSend);

    // add the command to the cyclic buffer
    HclCommandsGaudi3& commands = ((HclCommandsGaudi3&)(m_device.getGen2ArchCommands()));
    commands.serializeUpdateNicOffsets(stream, isSend, true, qpn, remoteIndices);
}

void QPManagerGaudi3ScaleUp::resizeOffsetDBs(const HCL_Comm comm)
{
    VERIFY(m_remoteRankOffsets.size() == m_myRankOffsets.size(), "Offsets DBs must be equal");
    size_t old_size = m_remoteRankOffsets.size();
    LOG_HCL_INFO(HCL, "Resizing m_remoteRankOffsets and m_myRankOffsets for new comm({})", comm);

    m_remoteRankOffsets.resize(old_size + DEFAULT_COMMUNICATORS_SIZE);
    m_myRankOffsets.resize(old_size + DEFAULT_COMMUNICATORS_SIZE);
    for (size_t i = old_size; i < m_remoteRankOffsets.size(); i++)
    {
        m_remoteRankOffsets[i].fill((uint16_t)-1);
        m_myRankOffsets[i].fill((uint16_t)-1);
    }
}

std::array<uint16_t, MAX_NICS_GEN2ARCH>&
QPManagerGaudi3ScaleUp::getRemoteRankIndices(HCL_Comm comm, HCL_CollectiveOp collectiveOp, bool isSend)
{
    HclDynamicCommunicator& dynamicComm    = m_device.getComm(comm);
    uint64_t                nicsStatusMask = m_device.getNicsStatusMask();
    const uint64_t          maxNics        = m_device.getHal()->getMaxNics();

    LOG_HCL_DEBUG(HCL,
                  "collectiveOp={}, isSend={}, nicsStatusMask={:024b}, maxNics={}",
                  collectiveOp,
                  isSend,
                  nicsStatusMask,
                  maxNics);

    // resize if needed
    if (comm >= m_remoteRankOffsets.size())
    {
        resizeOffsetDBs(comm);
    }

    // this is an array of offsets for the nics, please note that all offsets can be set later to zero
    // if the disregard rank bit is set to true in the collectiveOp command
    std::array<uint16_t, MAX_NICS_GEN2ARCH>& remoteRankOffsets = m_remoteRankOffsets[comm];

    bool needsRemoteRankIndex = (collectiveOp == eHCLAll2All ||
                                 ((collectiveOp == eHCLAllGather && !isSend) || (collectiveOp == eHCLReduceScatter)));

    if (needsRemoteRankIndex)
    {
        // Loop through all the nics
        for (uint16_t nicIndex = 0; nicIndex < maxNics; nicIndex++)
        {
            // If a nic is not active we do not need to configure it
            if ((nicsStatusMask & (1 << nicIndex)) == 0)
            {
                remoteRankOffsets[nicIndex] = 0;
                continue;
            }
            // Find the rank associated with this nic
            for (HCL_Rank rank : dynamicComm.getInnerRanksInclusive())
            {
                // For each nic, we want to find the rank that it goes out to
                // ==
                if ((unsigned)m_device.getServerConnectivity().getRemoteDevice(nicIndex, comm) ==
                    dynamicComm.m_remoteDevices[rank]->header.hwModuleID)
                {
                    remoteRankOffsets[nicIndex] =
                        mod(rank, dynamicComm.getScaleupGroupSize()) -
                        (((collectiveOp == eHCLReduceScatter) && !isSend && (rank > dynamicComm.getMyRank())) ? 1 : 0);
                    break;
                }
            }
        }
        return remoteRankOffsets;
    }

    // Loop through all the nics
    std::array<uint16_t, MAX_NICS_GEN2ARCH>& myRankOffsets = m_myRankOffsets[comm];
    for (uint16_t nicIndex = 0; nicIndex < maxNics; nicIndex++)
    {
        // If a nic is not active we do not need to configure it
        if ((nicsStatusMask & (1 << nicIndex)) == 0)
        {
            myRankOffsets[nicIndex] = 0;
            continue;
        }
        myRankOffsets[nicIndex] = mod(dynamicComm.getMyRank(), dynamicComm.getScaleupGroupSize());
    }
    return myRankOffsets;
}

void QPManagerGaudi3ScaleUp::setLastRankScaleup(hcl::ScalStream&       stream,
                                                const HCL_Comm         comm,
                                                const HCL_CollectiveOp collectiveOp,
                                                const bool             isSend)
{
    HclDeviceGaudi3&            device             = (HclDeviceGaudi3&)m_device;
    Gen2ArchServerConnectivity& serverConnectivity = device.getServerConnectivity();
    HclDynamicCommunicator&     dynamicComm        = device.getComm(comm);

    // for each scenario all nics use the same qpn
    const QPManagerHints hints(comm,
                               HCL_INVALID_RANK,
                               INVALID_QP,
                               QPManagerGaudi3::getQPi(collectiveOp, isSend));  // TODO: fix func call
    uint32_t             qpn = getQPn(hints);

    // we need to set the port mask to 1 for port that go out to the last rank
    uint32_t portsMask = 0;

    // get the last rank in scale up
    auto lastRank = dynamicComm.getScaleUpLastRank();

    if (lastRank != dynamicComm.getMyRank())
    {
        if (!(collectiveOp == eHCLAllGather && isSend))
        {
            // loop through all the nics
            for (uint16_t nicIndex = 0; nicIndex < device.getHal()->getMaxNics(); nicIndex++)
            {
                // we want to find the nics that go out to the last rank
                if ((unsigned)serverConnectivity.getRemoteDevice(nicIndex, comm) ==
                    dynamicComm.m_remoteDevices[lastRank]->header.hwModuleID)
                {
                    portsMask |= (1 << nicIndex);
                }
            }
        }
    }
    else
    {
        portsMask = getLastRankPortMask(dynamicComm, collectiveOp, isSend);
    }

    // add the command to the cyclic buffer
    HclCommandsGaudi3& commands = ((HclCommandsGaudi3&)(device.getGen2ArchCommands()));
    commands.serializeUpdateLastRank(stream, isSend, true, qpn, portsMask);
}

void QPManagerGaudi3ScaleUp::closeQPs(const QPManagerHints& hints)
{
    const HCL_Comm            comm  = hints.m_comm;
    const UniqueSortedVector& ranks = m_device.getComm(comm).getInnerRanksExclusive();

    for (auto& rank : ranks)
    {
        for (unsigned qpi = 0; qpi < m_maxQPsPerConnection; qpi++)
        {
            for (auto nic : m_device.getActiveNics(m_device.getMyRank(comm), rank, 1, comm))
            {
                if (m_device.isScaleOutPort(nic, comm)) continue;

                const uint32_t qpBase = m_qpInfoScaleUp.at(comm).at(qpi);
                if (isInvalidQPn(qpBase)) continue;

                const uint32_t qpn = qpBase + m_device.getNicToQpOffset(nic);
                LOG_HCL_TRACE(HCL, "closing QP: comm({}) nic({}) qpi({}) qpn({})", comm, nic, qpi, qpn);

                m_device.destroyQp(nic, qpn);
            }

            m_qpInfoScaleUp.at(comm).at(qpi) = 0;
        }
    }
}

/* ScaleOut QP Manager*/

QPManagerGaudi3ScaleOut::QPManagerGaudi3ScaleOut(HclDeviceGaudi3& device) : QPManagerGaudi3(device) {}

void QPManagerGaudi3ScaleOut::resizeDBForNewComms(const HCL_Comm comm)
{
    const size_t oldSize = m_qpInfoScaleOut.size();
    const size_t newSize = oldSize + DEFAULT_COMMUNICATORS_SIZE;

    LOG_HCL_INFO(HCL, "resizing m_qpInfoScaleOut for comm {} from {} to {}", comm, oldSize, newSize);

    m_qpInfoScaleOut.resize(newSize);
    for (unsigned index = oldSize; index < newSize; index++)
    {
        for (auto& qpSet : m_qpInfoScaleOut.at(index))
        {
            for (auto& qpi : qpSet)
            {
                qpi.fill(INVALID_QP);
            }
        }
    }
}

void QPManagerGaudi3ScaleOut::resizeDBPerComm(const HCL_Comm comm)
{
    const size_t commSize = m_device.getCommSize(comm);

    LOG_HCL_INFO(HCL, "resizing for comm {} to size {}", comm, commSize);

    m_qpInfoScaleOut.at(comm).resize(commSize);
    for (auto& qpSet : m_qpInfoScaleOut.at(comm))
    {
        for (auto& qpi : qpSet)
        {
            qpi.fill(INVALID_QP);
        }
    }
}

void QPManagerGaudi3ScaleOut::allocateQPDBStorage(const HCL_Comm comm)
{
    if (comm >= m_qpInfoScaleOut.size())
    {
        resizeDBForNewComms(comm);
    }

    if (m_qpInfoScaleOut[comm].size() == 0)
    {
        resizeDBPerComm(comm);
    }
}

void QPManagerGaudi3ScaleOut::registerQPs(const QPManagerHints& hints, const QpsVector& qps)
{
    const HCL_Comm comm       = hints.m_comm;
    const unsigned remoteRank = hints.m_remoteRank;

    if (unlikely(comm >= m_qpInfoScaleOut.size()))
    {
        resizeDBForNewComms(comm);
    }
    if (unlikely(m_qpInfoScaleOut.at(comm).size() == 0))
    {
        resizeDBPerComm(comm);
    }

    for (unsigned qpSet = 0; qpSet < MAX_QPS_SETS_PER_CONNECTION; qpSet++)
    {
        for (unsigned qpi = 0; qpi < m_maxQPsPerConnection; qpi++)
        {
            const unsigned qpIndex = m_maxQPsPerConnection * qpSet + qpi;
            if (qpIndex >= qps.size()) break;

            const uint32_t qpBase = qps.at(qpIndex);

            m_qpInfoScaleOut.at(comm).at(remoteRank).at(qpSet).at(qpi) = qpBase;

            LOG_HCL_DEBUG(HCL,
                          "m_qpInfoScaleOut[comm {}][rank {}][qpSet {}][qpi {}] = qpBase {}",
                          comm,
                          remoteRank,
                          qpSet,
                          qpi,
                          m_qpInfoScaleOut.at(comm).at(remoteRank).at(qpSet).at(qpi));
        }
    }
}

uint32_t QPManagerGaudi3ScaleOut::getQPn(const QPManagerHints& hints) const
{
    const HCL_Comm comm       = hints.m_comm;
    const unsigned remoteRank = hints.m_remoteRank;
    const unsigned qpSet      = hints.m_qpSet;
    const unsigned qpi        = hints.m_qpi;

    return m_qpInfoScaleOut.at(comm).at(remoteRank).at(qpSet).at(qpi);
}

uint32_t QPManagerGaudi3ScaleOut::getQPi(const QPManagerHints& hints) const
{
    const HCL_Comm comm       = hints.m_comm;
    const unsigned remoteRank = hints.m_remoteRank;
    const unsigned nic        = hints.m_nic;
    const unsigned qpn        = hints.m_qpn;

    for (unsigned qpSet = 0; qpSet < MAX_QPS_SETS_PER_CONNECTION; qpSet++)
    {
        for (unsigned qpi = 0; qpi < m_maxQPsPerConnection; qpi++)
        {
            if (m_qpInfoScaleOut.at(comm).at(remoteRank).at(qpSet).at(qpi) + m_device.getNicToQpOffset(nic) == qpn)
            {
                return qpi;
            }
        }
    }

    VERIFY(false, "could not find a match for comm {} rank {} nic {} qpn {}", comm, remoteRank, nic, qpn);
    return 0;
}

void QPManagerGaudi3ScaleOut::closeQPs(const QPManagerHints& hints)
{
    const HCL_Comm            comm  = hints.m_comm;
    const UniqueSortedVector& ranks = m_device.getComm(comm).getOuterRanksExclusive();

    // in HNIC flows we do not open or register scaleout QPs, so do not need to close any
    if (m_qpInfoScaleOut.size() == 0) return;

    for (auto& rank : ranks)
    {
        for (unsigned qpSet = 0; qpSet < MAX_QPS_SETS_PER_CONNECTION; qpSet++)
        {
            for (unsigned qpi = 0; qpi < m_maxQPsPerConnection; qpi++)
            {
                for (auto nic : m_device.getActiveNics(m_device.getMyRank(comm), rank, 1, comm))
                {
                    if (!(m_device.isScaleOutPort(nic, comm))) continue;

                    const uint32_t qpBase = m_qpInfoScaleOut.at(comm).at(rank).at(qpSet).at(qpi);
                    if (isInvalidQPn(qpBase)) continue;

                    const uint32_t qpn = qpBase + m_device.getNicToQpOffset(nic);
                    LOG_HCL_TRACE(HCL,
                                  "closing QP: comm({}) rank({}) nic({}) qpSet({}) qpi({}) qpn({})",
                                  comm,
                                  rank,
                                  nic,
                                  qpSet,
                                  qpi,
                                  qpn);

                    m_device.destroyQp(nic, qpn);
                }

                m_qpInfoScaleOut.at(comm).at(rank).at(qpSet).at(qpi) = 0;
            }
        }
    }
}
