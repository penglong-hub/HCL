#include "platform/gaudi3/hcl_device.h"

#include <memory>   // for make_shared, make_unique
#include <utility>  // for pair
#include <numeric>

#include "platform/gen2_arch_common/hcl_device_config.h"              // for HclDeviceConfig
#include "hcl_dynamic_communicator.h"                                 // for HclDyna...
#include "hcl_global_conf.h"                                          // for GCFG_MA...
#include "hcl_types.h"                                                // for HclConfigType
#include "hcl_utils.h"                                                // for VERIFY
#include "infra/scal/gaudi3/scal_manager.h"                           // for Gaudi3S...
#include "infra/scal/gen2_arch_common/scal_manager.h"                 // for Gen2Arc...
#include "platform/gaudi3/commands/hcl_commands.h"                    // for HclComm...
#include "platform/gen2_arch_common/eq_handler.h"                     // for IEventQ...
#include "platform/gaudi3/hal.h"                                      // for Gaudi3Hal
#include "platform/gaudi3/hal_hls3pcie.h"                             // for Gaudi3Hls3PCieHal
#include "platform/gaudi3/qp_manager.h"                               // for QPManager
#include "platform/gen2_arch_common/intermediate_buffer_container.h"  // for IntermediateBufferContainer
#include "platform/gen2_arch_common/scaleout_provider.h"
#include "ibverbs/hcl_ibverbs.h"
#include "interfaces/hcl_hal.h"                    // for HalPtr
#include "platform/gen2_arch_common/server_def.h"  // for Gen2ArchServerDef
#include "platform/gaudi3/signals/calculator.h"    // for SignalsCalculatorGaudi3

class QPManagerScaleOutGaudi3;

/* tests only constructor */
HclDeviceGaudi3::HclDeviceGaudi3(HclDeviceControllerGen2Arch& controller,
                                 const int                    moduleId,
                                 HclDeviceConfig&             deviceConfig,
                                 Gen2ArchServerDef&           serverDef)
: HclDeviceGen2Arch(true, controller, deviceConfig, serverDef)
{
    registerOpenQpCallback(LOOPBACK, [&](HCL_Comm comm) { return openQpsLoopback(comm); });
    registerOpenQpCallback(HLS3, [&](HCL_Comm comm) { return openQpsHLS(comm); });
    registerOpenQpCallback(HL338, [&](HCL_Comm comm) { return openQpsHLS(comm); });
    setHal(serverDef.getHalSharedPtr());
}

// Runtime ctor
HclDeviceGaudi3::HclDeviceGaudi3(HclDeviceControllerGen2Arch& controller,
                                 HclDeviceConfig&             deviceConfig,
                                 hcl::HalPtr                  halShared,
                                 Gen2ArchServerDef&           serverDef)
: HclDeviceGen2Arch(controller, deviceConfig, serverDef)
{
    // Read box type and create server specific objects
    const HclConfigType configType = (HclConfigType)GCFG_BOX_TYPE_ID.value();
    setHal(serverDef.getHalSharedPtr());
    if ((configType == HLS3) || (configType == LOOPBACK))
    {
    }
    else if (configType == HL338)
    {
        m_boxConfigType = HL338;
    }
    else
    {
        VERIFY(false, "Invalid server type {} for G3 device", configType);
    }
    LOG_HCL_INFO(HCL, "Set server type to {}", m_boxConfigType);

    std::shared_ptr<QPManager> qpManagerScaleUp  = std::make_shared<QPManagerGaudi3ScaleUp>(*this);
    std::shared_ptr<QPManager> qpManagerScaleOut = std::make_shared<QPManagerGaudi3ScaleOut>(*this);

    for (unsigned nic = 0; nic < MAX_NICS_GEN2ARCH; nic++)
    {
        if (isScaleOutPort(nic /*, HCL_Comm comm*/))
        {
            m_qpManagers.at(nic) = qpManagerScaleOut;
        }
        else
        {
            m_qpManagers.at(nic) = qpManagerScaleUp;
        }
    }

    m_scalManager.getHBMAddressRange(m_allocationRangeStart, m_allocationRangeEnd);
    registerOpenQpCallback(LOOPBACK, [&](HCL_Comm comm) { return openQpsLoopback(comm); });
    registerOpenQpCallback(HLS3, [&](HCL_Comm comm) { return openQpsHLS(comm); });
    registerOpenQpCallback(HL338, [&](HCL_Comm comm) { return openQpsHLS(comm); });

    updateDisabledPorts();
    initNicsMask();
    openWQs();
    m_eqHandler = new IEventQueueHandler();
    m_eqHandler->startThread(this);
    // The scaleout mode is set according also to if all scaleout ports are disabled by LKD/HCL or not. This is
    // regardless of communicator setup.
    setScaleoutMode(getServerConnectivity().getNumScaleOutPorts(/*HCL_Comm comm*/));
    m_sibContainer = new hcl::IntermediateBufferContainer(m_hal->getMaxStreams());
    createOfiPlugin();
    m_scaleoutProvider = ScaleoutProvider::createScaleOutProvider(this);
    setEdmaEngineGroupSizes();
    m_signalsCalculator = std::make_unique<SignalsCalculatorGaudi3>();
}

hlthunk_device_name HclDeviceGaudi3::getDeviceName()
{
    return HLTHUNK_DEVICE_GAUDI3;
}

void HclDeviceGaudi3::registerQps(HCL_Comm comm, HCL_Rank remoteRank, const QpsVector& qps, int nic)
{
    const QPManagerHints hints(comm, remoteRank);

    m_qpManagers.at(nic)->registerQPs(hints, qps);
}

void HclDeviceGaudi3::setScaleUpQPConfiguration(hcl::ScalStream& stream, HCL_Comm comm, bool isSend)
{
    const uint16_t defaultScaleUpPort = getServerConnectivity().getDefaultScaleUpPort(comm);
    m_qpManagers.at(defaultScaleUpPort)->setConfiguration(stream, comm, isSend);
}

QPUsage HclDeviceGaudi3::getBaseQpAndUsage(HclDynamicCommunicator& dynamicComm,
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
    const unsigned nic = isScaleOut ? getServerConnectivity().getDefaultScaleOutPortByIndex()
                                    : getServerConnectivity().getDefaultScaleUpPort(dynamicComm);
    return m_qpManagers.at(nic)->getBaseQpAndUsage(dynamicComm,
                                                   collectiveOp,
                                                   isSend,
                                                   isComplexCollective,
                                                   isReductionInIMB,
                                                   isHierarchical,
                                                   count,
                                                   cellCount,
                                                   boxType,
                                                   isScaleOut,
                                                   remoteRank,
                                                   qpSet,
                                                   isReduction,
                                                   complexCollective,
                                                   isRoot);
}

bool HclDeviceGaudi3::isSender(unsigned _qpi)
{
    return ((_qpi == G3::QP_e::QPE_RS_SEND) || (_qpi == G3::QP_e::QPE_AG_SEND) || (_qpi == G3::QP_e::QPE_A2A_SEND));
}

uint32_t HclDeviceGaudi3::getQpi(HCL_Comm comm, uint8_t nic, HCL_Rank remoteRank, uint32_t qpn, uint8_t qpSet)
{
    const QPManagerHints hints(comm, remoteRank, nic, INVALID_QP, qpn, qpSet);

    return m_qpManagers.at(nic)->getQPi(hints);
}

uint32_t HclDeviceGaudi3::createCollectiveQp(bool isScaleOut)
{
    return g_ibv.create_collective_qp(isScaleOut);
}

void HclDeviceGaudi3::allocateQps(HCL_Comm comm, const bool isScaleOut, const HCL_Rank remoteRank, QpsVector& qpnArr)
{
    LOG_HCL_TRACE(HCL,
                  "comm={}, isScaleOut={}, myRank={}, remoteRank={}",
                  comm,
                  isScaleOut,
                  getMyRank(comm),
                  remoteRank);
    uint8_t qpSets = getNumQpSets(isScaleOut, comm, remoteRank);
    for (uint8_t qpSet = 0; qpSet < qpSets; qpSet++)
    {
        for (uint64_t i = 0; i < getHal()->getMaxQPsPerNic(); i++)
        {
            // for non-peers - we only need to open the RS qps since they are used for send receive
            // for scale out peers - we need 4 qps only RS and AG, A2A will be directed to use RS
            // for scale up - we open 6 qps (G3::QP_e)
            // for null-submit mode - we don't open QPs
            bool isPeer = getComm(comm).isPeer(remoteRank);
            if ((isScaleOut && ((!isPeer && !QPManagerGaudi3ScaleOut::isRsQp(i)) ||
                                (isPeer && QPManagerGaudi3ScaleOut::isA2AQp(i)))) ||
                GCFG_HCL_NULL_SUBMIT.value())

            {
                qpnArr.push_back(0);
            }
            else
            {
                qpnArr.push_back(createCollectiveQp(isScaleOut));
            }

            LOG_HCL_DEBUG(HCL,
                          "Allocate QP, remoteRank({}){} qpSet: {}, QPi: {}, QPn: {}",
                          remoteRank,
                          remoteRank == getMyRank(comm) ? " Loopback connection, " : "",
                          qpSet,
                          i,
                          qpnArr[(qpSet * getHal()->getMaxQPsPerNic()) + i]);
        }
    }

    const unsigned nic = isScaleOut ? getServerConnectivity().getDefaultScaleOutPortByIndex()
                                    : getServerConnectivity().getDefaultScaleUpPort(comm);
    registerQps(comm, remoteRank, qpnArr, nic);
}

inline uint32_t HclDeviceGaudi3::createQp(uint32_t nic, unsigned qpId, uint32_t coll_qpn)
{
    auto offs = getNicToQpOffset(nic);

    return g_ibv.create_qp(isSender(qpId), nic, coll_qpn + offs);
}

void HclDeviceGaudi3::openRankQps(HCL_Comm    comm,
                                  HCL_Rank    rank,
                                  nics_mask_t nics,
                                  QpsVector&  qpnArr,
                                  const bool  isScaleOut)
{
    LOG_HCL_TRACE(HCL, "Processing rank={}", rank);

    uint8_t qpSets = getNumQpSets(isScaleOut, comm, rank);

    // loop over the active nics
    for (auto nic : nics)
    {
        createNicQps(comm, rank, nic, qpnArr, qpSets);
    }

    updateRankHasQp(comm, rank);
}

/**
 * @brief open QPs in loopback mode, use remote ranks QP data
 */
void HclDeviceGaudi3::openRankQpsLoopback(HCL_Comm comm, HCL_Rank rank, QpsVector& qpnArr)
{
    HCL_Rank myRank = getMyRank(comm);
    LOG_HCL_TRACE(HCL, "Processing rank={}", myRank);

    // loop over nics
    for (uint16_t index = 0; index < COMPACT_RANK_INFO_NICS; index++)
    {
        uint32_t nic    = getComm(comm).m_rankInfo.remoteInfo[rank].gaudiNicQPs.qp[index].nic;
        uint8_t  qpSets = getNumQpSets(isScaleOutPort(nic), comm, myRank);
        createNicQps(comm, rank, nic, qpnArr, qpSets);
    }

    // loopback always
    updateRankHasQp(comm, myRank);
}

/**
 * @brief create all QP sets/QPs on a single nic
 */
void HclDeviceGaudi3::createNicQps(HCL_Comm comm, HCL_Rank rank, uint8_t nic, QpsVector& qpnArr, uint8_t qpSets)
{
    // check if nic is down, we can open qps only for active nics
    if (!(m_hclNic.mask[nic]))
    {
        return;
    }

    for (uint8_t qpSet = 0; qpSet < qpSets; qpSet++)
    {
        uint8_t qpSetBase = getHal()->getMaxQPsPerNic() * qpSet;
        // allocate max QPs per nic
        for (unsigned i = 0; i < getHal()->getMaxQPsPerNic(); i++)
        {
            uint8_t qpnArrIndex = qpSetBase + i;
            if (qpnArr[qpnArrIndex] == 0) continue;
            uint32_t qpnWithOffset = createQp(nic, i, qpnArr[qpnArrIndex]);

            getComm(comm).m_rankInfo.remoteInfo[rank].gaudiNicQPs[nic].qp[qpSet][i] = qpnWithOffset;
        }
    }
}

#define ACTIVE_NICS(rank) getActiveNics(getMyRank(comm), rank, 1, comm)

hcclResult_t HclDeviceGaudi3::openQpsHlsScaleUp(HCL_Comm comm)
{
    LOG_HCL_TRACE(HCL, "comm={}", comm);

    // comm is scale-out only, no need for internal QPs
    if (getComm(comm).getInnerRanksExclusive().size() == 0)
    {
        return hcclSuccess;
    }

    QpsVector qpnArr;
    allocateQps(comm, false, HCL_INVALID_RANK, qpnArr);

    // in null-submit mode don't open QPs
    if (unlikely(GCFG_HCL_NULL_SUBMIT.value()))
    {
        return hcclSuccess;
    }

    // loop over all scale up ranks
    for (auto& rank : getComm(comm).getInnerRanksExclusive())
    {
        openRankQps(comm, rank, ACTIVE_NICS(rank), qpnArr, false);
    }

    return hcclSuccess;
}

hcclResult_t HclDeviceGaudi3::openQpsHlsScaleOut(HCL_Comm comm, const UniqueSortedVector& outerRanks)
{
    LOG_HCL_TRACE(HCL, "comm={}, outerRanks={}", comm, outerRanks);

    // allocate scale-out QPs memory for communicator
    allocateQPDBStorage(comm);

    // loop over all outer ranks
    for (auto& rank : outerRanks)
    {
        QpsVector qpnArr;
        allocateQps(comm, true, rank, qpnArr);

        // in null-submit mode don't open QPs
        if (likely(!GCFG_HCL_NULL_SUBMIT.value()))
        {
            openRankQps(comm, rank, ACTIVE_NICS(rank), qpnArr, true);
        }
    }

    return hcclSuccess;
}

hcclResult_t HclDeviceGaudi3::openQpsLoopback(HCL_Comm comm)
{
    HclConfigType configType = (HclConfigType)GCFG_BOX_TYPE_ID.value();
    if (configType != LOOPBACK)
    {
        LOG_HCL_ERR(HCL, "Invalid config type ({}), expecting LOOPBACK ({})", configType, LOOPBACK);
        return hcclInvalidArgument;
    }

    LOG_HCL_TRACE(HCL, "");

    // initialize nic-index mapping
    initRemoteNicsLoopback(comm);

    // open scaleup QPs
    QpsVector scaleupQPArr;
    allocateQps(comm, false, HCL_INVALID_RANK, scaleupQPArr);
    for (uint8_t rank = 0; rank < GCFG_LOOPBACK_SCALEUP_GROUP_SIZE.value(); rank++)
    {
        openRankQpsLoopback(comm, rank, scaleupQPArr);
    }

    // open scaleout QPs
    for (auto& rank : getComm(comm).getOuterRanksExclusive())
    {
        QpsVector scaleoutQPArr;
        allocateQps(comm, true, rank, scaleoutQPArr);
        openRankQpsLoopback(comm, rank, scaleoutQPArr);
    }

    return hcclSuccess;
}

unsigned HclDeviceGaudi3::getSenderWqeTableSize()
{
    return m_cgSize;
}

unsigned HclDeviceGaudi3::getReceiverWqeTableSize()
{
    return m_cgSize;
}

hcclResult_t HclDeviceGaudi3::updateQps(HCL_Comm comm)
{
    hcclResult_t            rc          = hcclSuccess;
    HclDynamicCommunicator& dynamicComm = getComm(comm);
    LOG_INFO(HCL, "Update scale-up QPs");
    for (auto& rank : dynamicComm.getInnerRanksExclusive())
    {
        rc = updateRankQps(comm, rank);
        VERIFY(rc == hcclSuccess, "updateQps failed rc={}", rc);
    }

    LOG_INFO(HCL, "Update scale-out connections");
    m_scaleoutProvider->verifyConnections(comm);

    // call ServerConnectivity comm init before scal config QPs
    // as scal is using the ServerConnectivity ports mapping
    getServerConnectivity().onCommInit(dynamicComm);
    if (dynamicComm.commScaleupGroupHasMultipleRanks()) getScalManager().configQps(comm, this);
    return rc;
}

void HclDeviceGaudi3::updateDisabledPorts()
{
    const uint64_t disabledPortsMap         = ~(getServerConnectivity().getEnabledPortsMask(/*HCL_Comm comm*/));
    const uint64_t disabledPortsMapLoopback = GCFG_LOOPBACK_DISABLED_NICS.value().empty()
                                                  ? 0
                                                  : getServerConnectivity().getExternalPortsMask(/*HCL_Comm comm*/);

    m_deviceConfig.updateDisabledPorts(disabledPortsMap, disabledPortsMapLoopback);
}

void HclDeviceGaudi3::getLagInfo(const uint16_t nic, uint8_t& lagIdx, uint8_t& lastInLag, const HCL_Comm comm)
{
    int maxSubPort = 0;
    if (isScaleOutPort(nic, comm))
    {
        lagIdx     = getServerConnectivity().getScaleoutSubPortIndex(nic, comm);
        maxSubPort = getServerConnectivity().getNumScaleOutPorts(comm) - 1;
    }
    else
    {
        lagIdx     = getServerConnectivity().getSubPortIndex(nic, comm);
        maxSubPort = getServerConnectivity().getMaxSubPort(false, comm);
    }
    lastInLag = (lagIdx == maxSubPort);
    LOG_HCL_DEBUG(HCL,
                  "nic={}, comm={}, lagIdx={}, maxSubPort={}, lastInLag={}",
                  nic,
                  comm,
                  lagIdx,
                  maxSubPort,
                  lastInLag);
}

uint8_t HclDeviceGaudi3::getPeerNic(HCL_Rank rank, HCL_Comm comm, uint8_t port)
{
    const HclConfigType configType = (HclConfigType)GCFG_BOX_TYPE_ID.value();
    if (getComm(comm).isRankInsideScaleupGroup(rank))  // scaleup port
    {
        if (configType == LOOPBACK)
        {
            return port;
        }
        else
        {
            return getServerConnectivity().getPeerPort(port, comm);
        }
    }
    else  // scaleout rank
    {
        // Handle remote peers / non peers, non-peers can have different scaleout ports
        const nics_mask_t myScaleOutPorts = getServerConnectivity().getScaleOutPorts(comm);
        const unsigned    remoteDevice = getComm(comm).m_remoteDevices[rank]->header.hwModuleID;  // Find target device
        const nics_mask_t remoteScaleoutPorts =
            getServerConnectivityGaudi3().getRemoteScaleOutPorts(remoteDevice,
                                                                 comm);  // get the remote scaleout ports list
        for (auto myScaleOutPort : myScaleOutPorts)
        {
            if (port == myScaleOutPort)  // Find the required port in our device scaleout ports list
            {
                const unsigned subPortIndex = getServerConnectivity().getSubPortIndex(port, comm);
                VERIFY(subPortIndex < remoteScaleoutPorts.count(),
                       "subPortIndex={} out of range for remote rank={}, port={}, remoteDevice={}, "
                       "remoteScaleoutPorts.size={}",
                       subPortIndex,
                       rank,
                       port,
                       remoteDevice,
                       remoteScaleoutPorts.count());
                const unsigned peerPort = remoteScaleoutPorts(subPortIndex);
                // We assume same disabled port masks for current and remote devices
                const uint8_t peerNic = configType == LOOPBACK ? port : peerPort;
                LOG_HCL_TRACE(HCL,
                              "rank={}, port={}, remoteDevice={}, subPortIndex={}, peerPort={}, peerNic={}",
                              rank,
                              port,
                              remoteDevice,
                              subPortIndex,
                              peerPort,
                              peerNic);
                return peerNic;
            }
        }
        VERIFY(false, "Didn't find any scaleout ports for port={}, remoteRank={}, comm={}", port, rank, comm);
    }
}

void HclDeviceGaudi3::setEdmaEngineGroupSizes()
{
    edmaEngineGroupSizes[0] = m_scalManager.getNumberOfEdmaEngines(0);
    LOG_HCL_TRACE(HCL, "EDMA group0 has {} engines", edmaEngineGroupSizes[0]);
}

void HclDeviceGaudi3::openWQs()
{
    VERIFY(m_hal);

    for (auto nic : m_hclNic.mask)
    {
        // Hybrid ports can be used as both SU and SO
        // Since WQs are only opened once (not per comm) we must assume that at some point in time
        // a hybrid port will be possible used for SO, so this QP should be allocated.
        const uint32_t max_qps =
            isScaleOutPort(nic /*, HCL_Comm comm*/) ? m_hal->getMaxQpPerExternalNic() : m_hal->getMaxQpPerInternalNic();

        m_hclNic[nic] = allocateNic(nic, max_qps + 1);
    }

    g_ibv.create_fifos(m_scalManager.getScalHandle());

    for (auto nic : m_hclNic.mask)
    {
        m_hclNic[nic]->init();
    }
}
