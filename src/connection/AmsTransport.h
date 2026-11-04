// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2026 Jens Fresenborg

   Transport under AmsConnection: plain TCP (port 48898, frames with the 6-byte
   AMS/TCP header) or Secure ADS (TlsTransport: TLS on port 8016, frames
   without it).
 */

#pragma once

#include "Sockets.h"

#include <memory>

struct AmsTransport {
	virtual ~AmsTransport() = default;

	/** Reads up to maxBytes; throws on timeout or a closed connection. */
	virtual size_t read(uint8_t *buffer, size_t maxBytes,
			    timeval *timeout) const = 0;
	/** Writes the whole frame and returns the number of bytes written. */
	virtual size_t write(const Frame &frame) const = 0;
	/** Makes a blocking read() return, e.g. to stop the receive thread. */
	virtual void Shutdown() = 0;
	/** @return the own IPv4 address of the connection (0xffffffff for IPv6) */
	virtual uint32_t OwnIp() const = 0;
	virtual bool IsConnectedTo(const struct addrinfo *targetAddresses) const = 0;
	/** true, if every frame starts with the 6-byte AMS/TCP header */
	virtual bool HasTcpHeader() const = 0;
};

/** Plain ADS over TCP, the transport Beckhoff's AmsConnection always used. */
struct TcpTransport : AmsTransport {
	/** @param[in] abort optional, see TcpSocket: aborts connecting and reading */
	explicit TcpTransport(const struct addrinfo *destination,
			      const std::atomic<bool> *abort = nullptr);

	size_t read(uint8_t *buffer, size_t maxBytes,
		    timeval *timeout) const override;
	size_t write(const Frame &frame) const override;
	void Shutdown() override;
	uint32_t OwnIp() const override;
	bool IsConnectedTo(const struct addrinfo *targetAddresses) const override;
	bool HasTcpHeader() const override
	{
		return true;
	}

    private:
	TcpSocket socket;
};
