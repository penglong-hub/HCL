#pragma once

#include <array>                                  // for array
#include <cstdint>                                // for uint16_t
#include <vector>                                 // for vector
#include <memory>                                 // for allocator, unique_ptr
#include <map>

#include "hcl_api_types.h"                        // for HCL_Rank
#include "hccl_types.h"                           // for hcclResult_t
#include "hcl_types.h"                            // for MAX_RANKS, INVALID_POD
#include "interfaces/hcl_unique_sorted_vector.h"  // for UniqueSortedVector
#include "interfaces/hcl_remote_device.h"         // for HclRemoteDevice
#include "hccl/ofi_communicator.h"                // for ofi_communicator_handle
#include "interfaces/hcl_hal.h"                   // for HalPtr
#include "hccl_internal_defs.h"                   // for internal_unique_id_t

class HclStaticBuffersManager;
class IHclDevice;

class HclDynamicCommunicator
{
public:
    HclDynamicCommunicator(HCL_Comm comm, hcl::HalPtr hal);
    virtual ~HclDynamicCommunicator() = default;

    bool init(const uint32_t hcclCommSize, const HCL_Rank rank, const int box_size);

    void setUniqueID(const internal_unique_id_t* internal_unique_id);

    /**
     * determine whether comm is inside the pod communicator
     */
    bool isCommunicatorInPod() const;

    /**
     * determine whether comm is a pod peers communicator
     */
    bool isCommunicatorPodPeers() const;
    bool isCommunicatorMultiPod() const;      // determine if comm requires scaleout
    bool isCommunicatorHierarchical() const;  // determine if comm requires scaleout & scaleup

    bool isPeer(HCL_Rank rank) const;
    bool arePeers(HCL_Rank rank1, HCL_Rank rank2) const;

    bool isRankInsidePod(HCL_Rank rank) const;
    bool isRanksInSamePod(HCL_Rank rank1, HCL_Rank rank2) const;
    bool isPeerOrInsideSamePod(HCL_Rank rank);

    const RankInfoHeader&                  getRemoteConnectionHeader(HCL_Rank rank) const;
    const UniqueSortedVector&              getInnerRanksExclusive();
    const UniqueSortedVector&              getInnerRanksInclusive();
    const UniqueSortedVector&              getOuterRanksExclusive();
    const UniqueSortedVector&              getOuterRanksInclusive();
    const UniqueSortedVector&              getConnectedRanks();
    const std::vector<uint16_t>&           getRankToPodMap();
    const std::vector<HCL_Rank>&           getPodToRankMap();

    HCL_Rank                  getMyRank() const;
    HCL_Rank                  getScaleUpLastRank();
    HCL_Rank                  getScaleOutLastRank();
    bool                      isLastRankInPod();
    uint16_t                  getMyPod();
    const UniqueSortedVector& getRanks() const;
    int                       getCommSize();
    int                       getPodSize();
    const uint64_t            getCollectiveCtr() const;
    void                      incCollectiveCtr();
    const uint64_t            incSendCtr(int peer);
    const uint64_t            getSendCtr(int peer);
    const uint64_t            incRecvCtr(int peer);
    const uint64_t            getRecvCtr(int peer);
    HCL_Rank                  getRankInPod() const;
    void                      setRankInPod();
    bool                      setSpotlightType(unsigned spotlightType);
    const unsigned            getSpotlightType() const;
    unsigned                  getMaxScaleOutQpSetsNum();
    uint64_t                  getSliceSize() const;

    hcclResult_t                           prepareAndValidateComm(bool isLoopbackModeOrNullSubmission = false);
    bool      AddNewRemoteDevice(HCL_Rank newRank);
    const std::string                      getCommUniqueId() const;

    HclRemoteDeviceArray                   m_remoteDevices;
    RankInfo                               m_rankInfo = {};
    int                                    m_commSize = -1;
    HCL_Rank                               m_rankInPod = -1;

    bool initializeHostNicBridge(const UniqueSortedVector& outerRanks);

    ofi_communicator_handle m_hostNicBridge;

    const std::vector<HCL_Rank>& getRemoteRanks() const;
    hcclResult_t                 setCommPodSize();

    int m_podSize = -1;

    mutable UniqueSortedVector m_ranksCache;

    std::vector<uint64_t> m_streamLatestLongSo;

    operator HCL_Comm() const {return m_commId;}
private:
    HCL_Comm  m_commId;
    hcclResult_t validateComm();
    hcclResult_t validateRankIds();
    /**
     * @brief Set the Slice Size based on communicator size and scaleout method.
     *
     * If GDR and more than 1 box - use increased slice size.
     *
     * @return hcclSuccess if configured slice size is valid
     */
    hcclResult_t setSliceSize();

    UniqueSortedVector              m_innerRanksExclusiveCache;  // exclude rank itself
    UniqueSortedVector              m_innerRanksInclusiveCache;  // include rank itself
    UniqueSortedVector              m_outerRanksExclusiveCache;  // exclude rank itself
    UniqueSortedVector              m_outerRanksInclusiveCache;  // include rank itself
    UniqueSortedVector              m_connectedRanks;            // exclude rank itself (inside pod + peers)
    std::vector<uint16_t>           m_rankToPodMap = {};
    std::vector<HCL_Rank>           m_podToRankMap = {};

    std::map<int, uint64_t> m_sendCounter;
    std::map<int, uint64_t> m_recvCounter;

    internal_unique_id_t       m_commUniqueId;
    std::string                m_commUniqueIdStr;
    hcl::HalPtr                m_hal;
    std::vector<HCL_Rank>      m_remoteRanks   = {};
    uint64_t                   m_collectiveCtr = 0;
    unsigned                   m_spotlightType = DEFAULT_SPOTLIGHT;
    uint64_t                   m_sliceSize;
};