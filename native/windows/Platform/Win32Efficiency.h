#pragma once
// Real OS calls behind IEfficiency. The Win32 headers stay in Win32Efficiency.cpp.
#include "IEfficiency.h"

namespace telaviva {

class Win32Efficiency final : public IEfficiency {
public:
    ProcessEfficiencyReading currentProcessState() override;
    bool setProcessEcoQoS(bool enabled) override;
    bool setThreadEcoQoS(EfficiencyThreadId thread, bool exempt) override;
};

}  // namespace telaviva
