// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2015 - 2022 Beckhoff Automation GmbH & Co. KG
 */

#pragma once

#include "AmsHeader.h"
#include "NotificationDispatcher.h"
#include "AmsTransport.h"
#include "Router.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

using Timepoint = std::chrono::steady_clock::time_point;

struct AmsRequest {
	Frame frame;
	const AmsAddr &destAddr;
	uint16_t port;
	uint16_t cmdId;
	uint32_t bufferLength;
	void *buffer;
	uint32_t *bytesRead;
	Timepoint deadline;

	AmsRequest(const AmsAddr &ams, uint16_t __port, uint16_t __cmdId,
		   uint32_t __bufferLength = 0, void *__buffer = nullptr,
		   uint32_t *__bytesRead = nullptr, size_t payloadLength = 0)
		: frame(sizeof(AmsTcpHeader) + sizeof(AoEHeader) +
			payloadLength)
		, destAddr(ams)
		, port(__port)
		, cmdId(__cmdId)
		, bufferLength(__bufferLength)
		, buffer(__buffer)
		, bytesRead(__bytesRead)
	{
	}

	void SetDeadline(uint32_t tmms)
	{
		deadline = std::chrono::steady_clock::now();
		deadline += std::chrono::milliseconds(tmms);
	}
};

struct AmsResponse {
	std::atomic<AmsRequest *> request;
	std::atomic<uint32_t> invokeId;

	AmsResponse();
	void Notify(uint32_t error);
	void Release();

	// wait for response or timeout and return received errorCode or ADSERR_CLIENT_SYNCTIMEOUT
	uint32_t Wait();

    private:
	std::mutex mutex;
	std::condition_variable cv;
	// errorCode is only valid, when wasWritten == true
	uint32_t errorCode;
	bool wasWritten;
};

struct AmsConnection {
	using BeforeReceive = std::function<void(const AmsTransport &)>;

	/**
	 * Returns a connected transport, ready for ADS frames (e.g. with the
	 * port registered at the local router), or throws. Throwing GiveUp ends
	 * reconnecting for good.
	 * abort becomes true when the connection is destroyed: hand it to the
	 * transport (TcpTransport, TlsTransport), so that a running connect or
	 * handshake stops at once; it outlives the transport.
	 */
	using Connector = std::function<std::unique_ptr<AmsTransport>(
		const std::atomic<bool> &abort)>;
	struct GiveUp : std::runtime_error {
		using std::runtime_error::runtime_error;
	};

	/** Seconds between two attempts to reconnect */
	static const int RECONNECT_INTERVAL_S = 1;

	/**
	 * Plain ADS over TCP (TcpTransport).
	 * @param[in] beforeReceive runs on the connected transport before the
	 *            receive thread starts, e.g. to register a port at the local
	 *            TwinCAT router. An exception from it aborts the construction.
	 */
	AmsConnection(Router &__router,
		      const struct addrinfo *destination = nullptr,
		      const BeforeReceive &beforeReceive = {});
	/** Any transport, e.g. Secure ADS (TlsTransport). */
	AmsConnection(Router &__router, std::unique_ptr<AmsTransport> transport,
		      const BeforeReceive &beforeReceive = {});
	/**
	 * Reconnecting connection: connect() makes the first transport
	 * here (its exception aborts the construction) and a new one after the
	 * connection was lost, at once and then every RECONNECT_INTERVAL_S,
	 * until it succeeds, throws GiveUp or the connection is destroyed.
	 * @param[in] offlineError 0: a request while the connection is down
	 *            waits until it is up again or until its timeout
	 *            (ADSERR_CLIENT_SYNCTIMEOUT), like the TcAdsDll with a lost
	 *            target; else the request fails at once with this error.
	 */
	AmsConnection(Router &__router, Connector connect,
		      uint32_t offlineError = 0);
	~AmsConnection();

	/** false while the connection is down or after it gave up */
	bool IsConnected() const;

	/**
	 * Sends an AMS/TCP frame with a router command and its payload.
	 * @return false, if the transport has no AMS/TCP header (Secure ADS)
	 */
	bool WriteRouterCommand(uint16_t command, const void *data,
				size_t length);

	/**
	 * The peer will close the connection on purpose, e.g. the local router
	 * after a port close: its end is not logged.
	 */
	void ExpectClose();

	SharedDispatcher
	CreateNotifyMapping(uint32_t hNotify,
			    std::shared_ptr<Notification> notification);
	long DeleteNotification(const AmsAddr &amsAddr, uint32_t hNotify,
				uint32_t tmms, uint16_t port);
	long AdsRequest(AmsRequest &request, uint32_t timeout);

	/**
     * Confirm if this AmsConnection is connected to one of the target addresses.
     * @param[in] targetAddresses pointer to a previously allocated list of
     *           "struct addrinfo" returned by getaddrinfo(3).
     * @return true, this connection can be used to reach one of the targetAddresses.
     */
	bool IsConnectedTo(const struct addrinfo *targetAddresses) const;

    private:
	friend struct AmsRouter;
	enum class State { CONNECTED, DOWN, GAVE_UP };

	AmsConnection(Router &__router, std::unique_ptr<AmsTransport> transport,
		      const BeforeReceive &beforeReceive, Connector connect,
		      uint32_t offlineError);

	Router &router;
	const Connector connector; // empty: no reconnecting (Beckhoff's behaviour)
	const uint32_t offlineError;
	// set by the destructor; before socket, the transports refer to it
	std::atomic<bool> aborting;
	// The transport is replaced when reconnecting; the last one stays until
	// then (IsConnectedTo). Users take a copy of the pointer under the lock.
	std::shared_ptr<AmsTransport> socket;
	State state;
	bool stopping;
	std::atomic<bool> closeExpected; // ExpectClose()
	// Targets that let a request run into its timeout and sent nothing
	// since: deleting a notification there does not wait again.
	std::set<AmsNetId> silentTargets;
	std::mutex silentMutex;
	mutable std::mutex stateMutex; // guards state, stopping and socket
	std::condition_variable stateChanged;
	std::thread receiver;
	std::atomic<size_t> refCount;
	std::atomic<uint32_t> invokeId;
	std::array<AmsResponse, Router::NUM_PORTS_MAX> queue;

	std::shared_ptr<AmsTransport> Transport() const;
	/** @return the transport once connected, null when the deadline passed or the connection gave up */
	std::shared_ptr<AmsTransport> AwaitConnected(const Timepoint &deadline);
	void SetState(State next);
	bool Reconnect();

	// The receive thread works on its own copy of the transport (rx), so
	// that replacing it does not pull it away under a running read.
	template <class T>
	void ReceiveFrame(const AmsTransport &rx, AmsResponse *response,
			  size_t length, uint32_t aoeError) const;
	bool ReceiveNotification(const AmsTransport &rx,
				 const AoEHeader &header);
	void ReceiveJunk(const AmsTransport &rx, size_t bytesToRead) const;
	void Receive(const AmsTransport &rx, void *buffer, size_t bytesToRead,
		     timeval *timeout = nullptr) const;
	void Receive(const AmsTransport &rx, void *buffer, size_t bytesToRead,
		     const Timepoint &deadline) const;
	template <class T> void Receive(const AmsTransport &rx, T &buffer) const
	{
		Receive(rx, &buffer, sizeof(T));
	}
	AmsResponse *Write(AmsTransport &tx, AmsRequest &request,
			   const AmsAddr srcAddr, bool &notSent);
	void Recv(const AmsTransport &rx);
	void TryRecv();
	uint32_t GetInvokeId();
	AmsResponse *Reserve(AmsRequest *request, uint16_t port);
	void Free(AmsResponse *response);
	// One request per port at a time: a second one waits for the slot
	// (until its deadline) instead of failing at once. Notifications are
	// created again on the program's port while it may send itself.
	std::mutex slotMutex;
	std::condition_variable slotFreed;
	AmsResponse *GetPending(uint32_t id, uint16_t port);

	std::map<VirtualConnection, SharedDispatcher> dispatcherList;
	std::recursive_mutex dispatcherListMutex;
	SharedDispatcher DispatcherListAdd(const VirtualConnection &connection);
	SharedDispatcher DispatcherListGet(const VirtualConnection &connection);

    public:
	const uint32_t ownIp;
};
