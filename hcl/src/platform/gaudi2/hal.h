#pragma once
#include "platform/gen2_arch_common/hal.h"
#include "gaudi2/asic_reg/pcie_wrap_special_regs.h"  // for mmPCIE_WRAP_IND_ARMISC_INFO
#include "gaudi2/asic_reg/gaudi2_blocks.h"           // for mmPCIE_WRAP_BASE

namespace hcl
{
class Gaudi2Hal : public Gen2ArchHal
{
public:
    Gaudi2Hal() = default;
    uint64_t getFlushPCIeReg() const override;
    virtual uint32_t getMaxQpPerInternalNic() const override;
    virtual uint32_t getMaxQpPerExternalNic() const override;
    virtual uint32_t getCollectiveContextsCount() const;
    uint64_t         getMaxQPsPerNicNonPeer() const;

private:
    const uint64_t m_flushReg =
        mmPCIE_WRAP_BASE + mmPCIE_WRAP_SPECIAL_GLBL_SPARE_0;  // Register close to PCIe to be used for flush

    // The number of QPs per NIC is limited because each QP holds a WQE table, and the total number of
    // WQEs per NIC is 420520
    const uint32_t m_maxQpPerInternalNic = 100;
    const uint32_t m_maxQpPerExternalNic = GCFG_MAX_QP_PER_EXTERNAL_NIC.value();

    const uint32_t m_collectiveContextsCount = 16;
    const uint64_t m_maxQPsPerNicNonPeer     = 2;
};

}  // namespace hcl