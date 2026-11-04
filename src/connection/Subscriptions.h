// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2026 Jens Fresenborg
 */

#pragma once

#include "AmsConnection.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

/**
 * Notifications with handles of our own.
 * Every notification is remembered with the request that created it, so it
 * can be created again on its target after the target restarted behind the
 * connection (MarkStale()) or after the connection was lost and came back
 * (Detach(), then Resubscribe()). The program keeps its handle; the callback
 * sees it instead of the handle of the target.
 *
 * Used by both parts of the library: the local router (one connection per
 * port) and the route way (one connection per target, shared by the ports).
 * The owner tells over Resolve which connection a notification goes over.
 *
 * Locking: the mutex of this class is taken last. Resolve, the connections
 * and the dispatchers are called without it, except Forget() of a
 * dispatcher, so the owner may call in with its own lock held, as long as
 * Resolve does not need a lock the caller of Add()/Resubscribe() holds.
 */
class Subscriptions {
    public:
	struct Link {
		/** null: no way to the target at the moment */
		std::shared_ptr<AmsConnection> connection;
		uint32_t tmms;
		/** port the target sends the notifications to */
		uint16_t sourcePort;
		/** returned by Add() if there is no connection */
		long offlineError;
	};
	using Resolve =
		std::function<Link(uint16_t port, const AmsAddr &target)>;

	explicit Subscriptions(Resolve resolve);

	/**
	 * Creates the notification on the target.
	 * @param[in] request AddDeviceNotification request of the program
	 * @param[out] pNotification our handle
	 */
	long Add(const AmsRequest &request, std::shared_ptr<Notification> notify,
		 uint32_t *pNotification);

	/** Deletes the notification, on the target too if it exists there. */
	long Delete(uint16_t port, const AmsAddr &target, uint32_t hNotify,
		    uint32_t tmms);

	/** Deletes all notifications of the port, on their targets too. */
	void ClosePort(uint16_t port, uint32_t tmms);

	/**
	 * The targets of the port restarted behind its connection: delete the
	 * notifications there (they may have survived) and create them again.
	 */
	void MarkStale(uint16_t port);

	/**
	 * The connection is lost: its handles are forgotten, the notifications
	 * wait to be created again. Call it before the connection is destroyed,
	 * the dispatchers call back into it.
	 */
	void Detach(const AmsConnection &connection);

	/**
	 * The connection is down but reconnects: its handles are
	 * forgotten, the notifications wait to be created again on the same
	 * connection. Safe from its receive thread (OnConnectionLost()).
	 */
	void Lost(const AmsConnection &connection);

	/**
	 * Creates the notifications again that are not on their target at the
	 * moment, over connections that are up. Sends requests and waits for
	 * them: never call it from the receive thread of a connection.
	 */
	void Resubscribe();

    private:
	struct Entry {
		uint16_t port;
		AmsAddr target;
		std::vector<uint8_t> request; // AdsAddDeviceNotificationRequest
		NotificationCallback callback;
		uint32_t hUser;
		uint32_t length;
		uint32_t hServer; // handle on the target, valid with dispatcher
		SharedDispatcher dispatcher; // set while created on the target
		std::shared_ptr<AmsConnection> connection; // the one in use
		bool stale; // target restarted: delete and create again
		bool busy; // being created right now
		bool lost; // connection lost while busy
	};

	const Resolve resolve;
	std::mutex mutex;
	std::map<uint32_t, Entry> entries; // key: our handle
	uint32_t next;

	static long Create(uint32_t hNotify, const Entry &e, const Link &link,
			   uint32_t &hServer, SharedDispatcher &dispatcher);
	static bool Settle(Entry &e, uint32_t hServer,
			   SharedDispatcher dispatcher,
			   std::shared_ptr<AmsConnection> &drop);
	void Resubscribe(uint32_t hNotify);
};
