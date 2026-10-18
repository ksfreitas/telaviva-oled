#pragma once
// HTTPS GET through WinHTTP for the update check. Never throws; any failure comes back as transportOk=false.
#include "UpdateChecker.h"

namespace telaviva {

class WinHttpGet final : public IHttpGet {
public:
    HttpResult get(const std::string& url, const std::string& userAgent) override;
};

}  // namespace telaviva
