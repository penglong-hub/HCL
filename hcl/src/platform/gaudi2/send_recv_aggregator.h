#pragma once

#include <cstdint>                                    // for uint64_t, uint16_t
#include <array>                                      // for array
#include <vector>                                     // for vector

#include "hcl_api_types.h"                            // for HCL_Comm
#include "platform/gaudi2/context_manager_priv.h"     // for RequiredCollect...
#include "platform/gaudi2/nic_passthrough_handler.h"  // for NicPassthroughHandler
#include "platform/gaudi2/port_mapping.h"             // for Gaudi2DevicePortMapping
#include "platform/gaudi2/types.h"                    // for HLS2_BOX_SIZE
#include "platform/gen2_arch_common/types.h"          // for MAX_NICS_GEN2ARCH, GEN2ARCH_HLS_BOX_SIZE
#include "platform/gen2_arch_common/send_recv_aggregator.h"  // for SendRecvAggregatorBase

class ContextManager;
class HclCommandsGaudi2;
class HclCommandsGen2Arch;
namespace hcl
{
class ScalStreamBase;
}

class SendRecvAggregator : public SendRecvAggregatorBase
{
public:
    SendRecvAggregator(const std::vector<unsigned>&   nicEngines,
                       const Gaudi2DevicePortMapping& portMapping,
                       HclCommandsGen2Arch&           commands);
    virtual ~SendRecvAggregator() = default;
    SendRecvAggregator(SendRecvAggregator&&)      = delete;
    SendRecvAggregator(const SendRecvAggregator&) = delete;
    SendRecvAggregator& operator=(SendRecvAggregator&&) = delete;
    SendRecvAggregator& operator=(const SendRecvAggregator&) = delete;

    static_assert(GEN2ARCH_HLS_BOX_SIZE == HLS2_BOX_SIZE, "G2 must match Gen2Arch box size");

    bool getRequiredContext(RequiredCollectiveContext& requiredContext);
    void addSendRecvArray(const SendRecvArray&              arr,
                          int                               selfModuleId,
                          unsigned                          collectiveContextIndex,
                          const RequiredCollectiveContext&& requiredContext);

    void flush(hcl::ScalStreamBase& scalStream,
               ContextManager&      contextManager,
               unsigned             collectiveContextIndex,
               unsigned             commDescIndex,
               int                  selfModuleId,
               HCL_Comm             comm,
               unsigned             syncObjectAddressIndex,
               bool                 isSend,
               bool                 notifyRndvAck,
               bool                 waitForRndvAcks);

private:
    void configureLastEntriesPerDevice();

    bool                      m_requiredContextSet = false;
    RequiredCollectiveContext m_requiredContext;

    HclCommandsGaudi2&    m_commands;
    NicPassthroughHandler m_nicPassthroughHandler;
};