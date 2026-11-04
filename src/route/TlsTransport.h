// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2026 Jens Fresenborg

   Secure ADS transport: TLS 1.2 (mbedTLS) on TCP 8016 with self-signed
   certificates (SSC), a pre-shared key (PSK) or an own CA (SCA), then one
   TlsConnectInfo exchange, then AMS frames without the AMS/TCP header.
 */

#pragma once

#include "AmsTransport.h"
#include "TlsSettings.h"
#include "AdsDef.h"

#include <memory>
#include <stdexcept>
#include <string>

/** Secure ADS failed; adsError is the ADS return code to hand out. */
struct TlsError : std::runtime_error {
	TlsError(long error, const std::string &what)
		: std::runtime_error(what)
		, adsError(error)
	{
	}
	const long adsError;
};

struct TlsTransport : AmsTransport {
	/**
	 * Connects to an AMS router over an existing route; the target's
	 * certificate has to match the one stored by AddRemoteRouteSecure()
	 * for the same host (file target-<host>.pem).
	 * @param[in] ownNetId sent to the target; 0.0.0.0.0.0: own IP + .1.1
	 * @param[in] abort optional, see TcpSocket: aborts connecting, the
	 *            handshake and reading
	 * @throw TlsError, std::runtime_error
	 */
	TlsTransport(const struct addrinfo *destination, const std::string &host,
		     const bhf::ads::TlsSettings &settings, AmsNetId ownNetId,
		     AmsNetId target, const std::atomic<bool> *abort = nullptr);

	/**
	 * Adds a route on the target with user and password (ADD_REMOTE) and
	 * stores the target's certificate; afterwards the transport is not used.
	 * @throw TlsError, std::runtime_error
	 */
	static AmsNetId AddRoute(const struct addrinfo *destination,
				 const std::string &host,
				 const bhf::ads::TlsSettings &settings,
				 AmsNetId ownNetId, const std::string &user,
				 const std::string &password);

	~TlsTransport() override;

	size_t read(uint8_t *buffer, size_t maxBytes,
		    timeval *timeout) const override;
	size_t write(const Frame &frame) const override;
	void Shutdown() override;
	uint32_t OwnIp() const override;
	bool IsConnectedTo(const struct addrinfo *targetAddresses) const override;
	bool HasTcpHeader() const override
	{
		return false;
	}

	/** AmsNetId the target reported in its TlsConnectInfo */
	AmsNetId TargetNetId() const;

	struct Impl;

    private:
	TlsTransport(const struct addrinfo *destination, const std::string &host,
		     const bhf::ads::TlsSettings &settings,
		     const std::atomic<bool> *abort = nullptr);
	std::unique_ptr<Impl> d;
};
