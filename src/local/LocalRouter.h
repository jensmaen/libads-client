// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2026 Jens Fresenborg
 */

#pragma once

#include "AdsBackend.h"
#include "Subscriptions.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
 * ADS through the local TwinCAT router over TCP (127.0.0.1:48898).
 * Every port has its own TCP connection; the router assigns the port number
 * and the local AmsNetId.
 *
 * When the router reports "started" (TwinCAT restarted behind the open
 * connection), the notifications of the port are created again on their
 * targets. Their handles are assigned by this
 * class, not by the target, so the program keeps them. When the connection
 * to the router is lost, the port stays open but every request fails with
 * ROUTERERR_NOTREGISTERED until the program closes it.
 */
struct LocalRouter : Router, AdsBackend {
	static const uint16_t CMD_PORT_CLOSE = 0x0001;
	static const uint16_t CMD_PORT_CONNECT = 0x1000;
	static const uint16_t CMD_ROUTER_NOTE = 0x1001;

	static const uint32_t ROUTER_STOPPED = 0;
	static const uint32_t ROUTER_STARTED = 1;
	static const uint32_t ROUTER_REMOVED = 2;

	/** @param[in] host address of the router, "host" or "host:port" */
	explicit LocalRouter(std::string host = "127.0.0.1:48898");
	~LocalRouter();

	/** @return the port handle, 0 on failure */
	uint16_t OpenPort();

	/** Port is open and its connection to the router is alive */
	bool IsConnected(uint16_t port) const;

	/**
	 * Receives router events (AmsRouterEvent) per port, called from the
	 * worker thread, so it may call back into this class.
	 */
	using RouterEventHandler = std::function<void(uint16_t port, uint32_t event)>;
	void SetRouterEventHandler(RouterEventHandler handler);

	bool OwnsPort(uint16_t port) override;
	long ClosePort(uint16_t port) override;
	long GetLocalAddress(uint16_t port, AmsAddr *pAddr) override;
	long GetTimeout(uint16_t port, uint32_t &timeout) override;
	long SetTimeout(uint16_t port, uint32_t timeout) override;
	long AdsRequest(AmsRequest &request) override;
	long AddNotification(AmsRequest &request, uint32_t *pNotification,
			     std::shared_ptr<Notification> notify) override;
	long DelNotification(uint16_t port, const AmsAddr *pAddr,
			     uint32_t hNotification) override;

	int PortSlot(uint16_t port) const override;
	void OnRouterCommand(AmsConnection &from, uint16_t command,
			     const uint8_t *data, size_t length) override;
	void OnConnectionLost(AmsConnection &from) override;

    private:
	struct Port {
		AmsAddr addr; // currently assigned by the router
		uint32_t tmms;
		std::shared_ptr<AmsConnection> connection; // null once lost
		bool lost;
		uint32_t state; // last router state reported (ROUTER_*)
	};

	const std::string host;
	mutable std::mutex mutex;
	std::map<uint16_t, std::shared_ptr<Port> > ports; // key: port handle
	Subscriptions subscriptions;

	RouterEventHandler routerEvent;
	std::deque<std::pair<uint16_t, uint32_t> > events;
	std::condition_variable wake;
	bool stopWorker;
	std::thread worker;

	std::shared_ptr<Port> Find(uint16_t port) const;
	uint16_t FindHandle(const AmsConnection &connection) const;
	std::shared_ptr<AmsConnection> Connect(AmsAddr &addr);
	Subscriptions::Link Resolve(uint16_t port) const;
	void Work();
	void Report(uint16_t handle, Port &p, uint32_t state);
	void Release(uint16_t handle);
	static AmsAddr Register(const AmsTransport &socket);
};
