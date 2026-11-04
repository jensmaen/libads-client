// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2015 - 2022 Beckhoff Automation GmbH & Co. KG
 */

#pragma once

#include "StandaloneAdsDef.h"

#include <iosfwd>
bool operator<(const AmsNetId &lhs, const AmsNetId &rhs);
bool operator<(const AmsAddr &lhs, const AmsAddr &rhs);
std::ostream &operator<<(std::ostream &os, const AmsNetId &netId);
AmsNetId make_AmsNetId(const std::string &addr);

namespace bhf
{
namespace ads
{
enum nSystemServiceOpenFile : uint32_t {
	SYSTEMSERVICE_OPENGENERIC /*[[deprecated]]*/ = 1,
};
}
}
