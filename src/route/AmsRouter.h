// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2015 - 2022 Beckhoff Automation GmbH & Co. KG
 */

#pragma once

#include "AdsBackend.h"
#include "AmsPort.h"
#include "Subscriptions.h"
#include "TlsSettings.h"
#include <condition_variable>
#include <thread>
#include <unordered_set>

/**
 * Route way: ADS directly to the targets over routes of our own. Every
 * route reconnects after a loss, every second, until it is deleted;
 * requests meanwhile wait until their timeout (ADSERR_CLIENT_SYNCTIMEOUT),
 * notifications are created again once the connection is back.
 */
struct AmsRouter : Router, AdsBackend {
	AmsRouter(AmsNetId netId = AmsNetId{});
	~AmsRouter();

	uint16_t OpenPort();
	bool OwnsPort(uint16_t port) override;
	long ClosePort(uint16_t port) override;
	long GetLocalAddress(uint16_t port, AmsAddr *pAddr) override;
	void SetLocalAddress(AmsNetId netId);
	long GetTimeout(uint16_t port, uint32_t &timeout) override;
	long SetTimeout(uint16_t port, uint32_t timeout) override;
	long AddNotification(AmsRequest &request, uint32_t *pNotification,
			     std::shared_ptr<Notification> notify) override;
	long DelNotification(uint16_t port, const AmsAddr *pAddr,
			     uint32_t hNotification) override;

	[[deprecated]]
	long AddRoute(AmsNetId ams, const IpV4 &ip);
	/** @param[in] tls nullptr: plain ADS on 48898, else Secure ADS on 8016 */
	long AddRoute(AmsNetId ams, const std::string &host,
		      const bhf::ads::TlsSettings *tls = nullptr);
	void DelRoute(const AmsNetId &ams);
	AmsConnection *GetConnection(const AmsNetId &pAddr);
	long AdsRequest(AmsRequest &request) override;

	void OnConnectionLost(AmsConnection &from) override;
	void OnReconnected(AmsConnection &from) override;

    private:
	AmsNetId localAddr;
	std::recursive_mutex mutex;
	std::condition_variable_any connection_attempt_events;
	std::map<AmsNetId, std::tuple<> > connection_attempts;
	std::unordered_set<std::shared_ptr<AmsConnection> > connections;
	std::map<AmsNetId, AmsConnection *> mapping;

	void
	AwaitConnectionAttempts(const AmsNetId &ams,
				std::unique_lock<std::recursive_mutex> &lock);
	void DeleteIfLastConnection(const AmsConnection *conn);
	std::shared_ptr<AmsConnection> SharedConnection(const AmsNetId &ams);
	Subscriptions::Link Resolve(uint16_t port, const AmsAddr &target);

	std::array<AmsPort, NUM_PORTS_MAX> ports;
	// notifications with own handles, after the connections: their
	// entries refer to them
	Subscriptions subscriptions;

	// creates notifications again after reconnecting; last, it uses the rest
	std::mutex workerMutex;
	std::condition_variable workerWake;
	bool stopWorker;
	std::thread worker;
	void Work();
};
