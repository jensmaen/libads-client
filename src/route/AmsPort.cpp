// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2015 - 2022 Beckhoff Automation GmbH & Co. KG
 */

#include "AmsPort.h"

namespace std
{
bool operator==(const AmsAddr &lhs, const AmsAddr &rhs)
{
	return 0 == memcmp(&lhs, &rhs, sizeof(lhs));
}
}

AmsPort::AmsPort()
	: tmms(DEFAULT_TIMEOUT)
	, port(0)
{
}

void AmsPort::Close()
{
	// the notifications of the port are deleted by AmsRouter (Subscriptions)
	tmms = DEFAULT_TIMEOUT;
	port = 0;
}

bool AmsPort::IsOpen() const
{
	return !!port;
}

uint16_t AmsPort::Open(uint16_t __port)
{
	port = __port;
	return port;
}
