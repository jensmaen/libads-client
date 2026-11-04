// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2026 Jens Fresenborg
 */

#include "AmsTransport.h"

TcpTransport::TcpTransport(const struct addrinfo *const destination,
			   const std::atomic<bool> *const abort)
	: socket(destination, abort)
{
}

size_t TcpTransport::read(uint8_t *const buffer, const size_t maxBytes,
			  timeval *const timeout) const
{
	return socket.read(buffer, maxBytes, timeout);
}

size_t TcpTransport::write(const Frame &frame) const
{
	return socket.write(frame);
}

void TcpTransport::Shutdown()
{
	socket.Shutdown();
}

uint32_t TcpTransport::OwnIp() const
{
	return socket.Connect();
}

bool TcpTransport::IsConnectedTo(const struct addrinfo *const targetAddresses) const
{
	return socket.IsConnectedTo(targetAddresses);
}
