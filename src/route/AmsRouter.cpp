// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2015 - 2022 Beckhoff Automation GmbH & Co. KG
 */

#include "AmsRouter.h"
#include "Log.h"
#if LIBADS_CLIENT_SECURE
#include "TlsTransport.h"
#endif

#include <algorithm>

AmsRouter::AmsRouter(AmsNetId netId)
	: localAddr(netId)
	, subscriptions([this](uint16_t port, const AmsAddr &target) {
		return Resolve(port, target);
	})
	, stopWorker(false)
	, worker(&AmsRouter::Work, this)
{
}

AmsRouter::~AmsRouter()
{
	{
		std::lock_guard<std::mutex> lock(workerMutex);
		stopWorker = true;
	}
	workerWake.notify_all();
	worker.join();
}

void AmsRouter::Work()
{
	// Once a second, or at once after a reconnect: notifications whose
	// connection came back, or whose target was not ready yet.
	std::unique_lock<std::mutex> lock(workerMutex);
	while (!stopWorker) {
		workerWake.wait_for(lock, std::chrono::seconds(1));
		if (stopWorker) {
			return;
		}
		lock.unlock();
		subscriptions.Resubscribe();
		lock.lock();
	}
}

void AmsRouter::OnConnectionLost(AmsConnection &from)
{
	LOG_INFO("Route connection lost, reconnecting");
	subscriptions.Lost(from);
}

void AmsRouter::OnReconnected(AmsConnection &from)
{
	(void)from;
	LOG_INFO("Route connection is back");
	workerWake.notify_all();
}

long AmsRouter::AddRoute(AmsNetId ams, const IpV4 &ip)
{
	/**
     * We keep this madness only for backwards compatibility, to give
     * downstream projects time to migrate to the much saner interface.
     */
	struct in_addr addr;
	static_assert(sizeof(addr) == sizeof(ip),
		      "Oops sizeof(IpV4) doesn't match sizeof(in_addr)");
	memcpy(&addr, &ip.value, sizeof(addr));
	return AddRoute(ams, std::string(inet_ntoa(addr)));
}

long AmsRouter::AddRoute(AmsNetId ams, const std::string &host,
			 const bhf::ads::TlsSettings *const tls)
{
	/**
        DNS lookups are pretty time consuming, we shouldn't do them
        with a looked mutex! So instead we do the lookup first and
        use the results, later.
     */
#if !LIBADS_CLIENT_SECURE
	if (tls) {
		return ADSERR_DEVICE_SRVNOTSUPP; // built without Secure ADS
	}
#endif
	auto hostAddresses =
		bhf::ads::GetListOfAddresses(host, tls ? "8016" : "48898");

	std::unique_lock<std::recursive_mutex> lock(mutex);

	AwaitConnectionAttempts(ams, lock);

	const auto oldConnection = GetConnection(ams);
	if (oldConnection &&
	    !oldConnection->IsConnectedTo(hostAddresses.get())) {
		/**
           There is already a route for this AmsNetId, but with
           a different IP. The old route has to be deleted, first!
         */
		return ROUTERERR_PORTALREADYINUSE;
	}

	for (const auto &conn : connections) {
		if (conn->IsConnectedTo(hostAddresses.get())) {
			conn->refCount++;
			mapping[ams] = conn.get();
			return 0;
		}
	}

	connection_attempts[ams] = {};
	const auto ownNetId = localAddr;
	lock.unlock();

	// Makes the transport, the first time and after every loss. The
	// address is resolved again every time (DNS may change, an IP costs
	// nothing); a failure, TLS too, is retried: the target may just be
	// being configured.
	const std::string service = tls ? "8016" : "48898";
#if LIBADS_CLIENT_SECURE
	const auto settings =
		tls ? std::make_shared<const bhf::ads::TlsSettings>(*tls) :
		      nullptr;
#endif
	AmsConnection::Connector connect =
		[=](const std::atomic<bool> &abort)
		-> std::unique_ptr<AmsTransport> {
		const auto addresses =
			bhf::ads::GetListOfAddresses(host, service);
#if LIBADS_CLIENT_SECURE
		if (settings) {
			return std::unique_ptr<AmsTransport>(
				new TlsTransport{ addresses.get(), host,
						  *settings, ownNetId, ams,
						  &abort });
		}
#endif
		return std::unique_ptr<AmsTransport>(
			new TcpTransport{ addresses.get(), &abort });
	};
#if !LIBADS_CLIENT_SECURE
	(void)ownNetId;
#endif

	try {
		auto new_connection = std::make_shared<AmsConnection>(
			*this, std::move(connect));
		lock.lock();
		connection_attempts.erase(ams);
		connection_attempt_events.notify_all();

		auto conn = connections.emplace(std::move(new_connection));
		if (conn.second) {
			/** in case no local AmsNetId was set previously, we derive one */
			if (!localAddr) {
				localAddr =
					AmsNetId{ conn.first->get()->ownIp };
			}
			conn.first->get()->refCount++;
			mapping[ams] = conn.first->get();
			return !conn.first->get()->ownIp;
		}

		return -1;
	} catch (...) {
		if (!lock.owns_lock()) {
			lock.lock();
		}
		connection_attempts.erase(ams);
		connection_attempt_events.notify_all();
		throw;
	}
}

void AmsRouter::DelRoute(const AmsNetId &ams)
{
	std::unique_lock<std::recursive_mutex> lock(mutex);

	AwaitConnectionAttempts(ams, lock);

	auto route = mapping.find(ams);
	if (route != mapping.end()) {
		AmsConnection *conn = route->second;
		if (0 == --conn->refCount) {
			mapping.erase(route);
			DeleteIfLastConnection(conn);
		}
	}
}

void AmsRouter::DeleteIfLastConnection(const AmsConnection *const conn)
{
	if (conn) {
		for (const auto &r : mapping) {
			if (r.second == conn) {
				return;
			}
		}
		for (auto it = connections.begin(); it != connections.end();
		     ++it) {
			if (conn == it->get()) {
				const auto last = *it;
				connections.erase(it);
				// the dispatchers call back into the connection
				subscriptions.Detach(*last);
				return;
			}
		}
	}
}

uint16_t AmsRouter::OpenPort()
{
	std::lock_guard<std::recursive_mutex> lock(mutex);

	for (uint16_t i = 0; i < NUM_PORTS_MAX; ++i) {
		if (!ports[i].IsOpen()) {
			return ports[i].Open(PORT_BASE + i);
		}
	}
	return 0;
}

bool AmsRouter::OwnsPort(uint16_t port)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return (port >= PORT_BASE) && (port < PORT_BASE + NUM_PORTS_MAX) &&
	       ports[port - PORT_BASE].IsOpen();
}

long AmsRouter::ClosePort(uint16_t port)
{
	uint32_t tmms;
	if (!OwnsPort(port) || GetTimeout(port, tmms)) {
		return ADSERR_CLIENT_PORTNOTOPEN;
	}
	// deletes the notifications over the network, so not under the lock
	subscriptions.ClosePort(port, tmms);
	std::lock_guard<std::recursive_mutex> lock(mutex);
	ports[port - PORT_BASE].Close();
	return 0;
}

long AmsRouter::GetLocalAddress(uint16_t port, AmsAddr *pAddr)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if ((port < PORT_BASE) || (port >= PORT_BASE + NUM_PORTS_MAX)) {
		return ADSERR_CLIENT_PORTNOTOPEN;
	}

	if (ports[port - PORT_BASE].IsOpen()) {
		memcpy(&pAddr->netId, &localAddr, sizeof(localAddr));
		pAddr->port = port;
		return 0;
	}
	return ADSERR_CLIENT_PORTNOTOPEN;
}

void AmsRouter::SetLocalAddress(AmsNetId netId)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	localAddr = netId;
}

long AmsRouter::GetTimeout(uint16_t port, uint32_t &timeout)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if ((port < PORT_BASE) || (port >= PORT_BASE + NUM_PORTS_MAX)) {
		return ADSERR_CLIENT_PORTNOTOPEN;
	}

	timeout = ports[port - PORT_BASE].tmms;
	return 0;
}

long AmsRouter::SetTimeout(uint16_t port, uint32_t timeout)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if ((port < PORT_BASE) || (port >= PORT_BASE + NUM_PORTS_MAX)) {
		return ADSERR_CLIENT_PORTNOTOPEN;
	}

	ports[port - PORT_BASE].tmms = timeout;
	return 0;
}

AmsConnection *AmsRouter::GetConnection(const AmsNetId &amsDest)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	const auto it = mapping.find(amsDest);
	if (it != mapping.end()) {
		return it->second;
	}
	return nullptr;
}

long AmsRouter::AdsRequest(AmsRequest &request)
{
	if (request.bytesRead) {
		*request.bytesRead = 0;
	}

	auto ads = GetConnection(request.destAddr.netId);
	if (!ads) {
		return GLOBALERR_MISSING_ROUTE;
	}
	return ads->AdsRequest(request,
			       ports[request.port - Router::PORT_BASE].tmms);
}

long AmsRouter::AddNotification(AmsRequest &request, uint32_t *pNotification,
				std::shared_ptr<Notification> notify)
{
	if (request.bytesRead) {
		*request.bytesRead = 0;
	}

	if (!OwnsPort(request.port)) {
		return ADSERR_CLIENT_PORTNOTOPEN;
	}
	// Own handles: after reconnecting, the target assigns new ones.
	return subscriptions.Add(request, std::move(notify), pNotification);
}

long AmsRouter::DelNotification(uint16_t port, const AmsAddr *pAddr,
				uint32_t hNotification)
{
	uint32_t tmms;
	if (!OwnsPort(port) || GetTimeout(port, tmms)) {
		return ADSERR_CLIENT_PORTNOTOPEN;
	}
	return subscriptions.Delete(port, *pAddr, hNotification, tmms);
}

std::shared_ptr<AmsConnection> AmsRouter::SharedConnection(const AmsNetId &ams)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	const auto raw = GetConnection(ams);
	for (const auto &c : connections) {
		if (c.get() == raw) {
			return c;
		}
	}
	return nullptr;
}

Subscriptions::Link AmsRouter::Resolve(uint16_t port, const AmsAddr &target)
{
	Subscriptions::Link link{ nullptr, 0, port, GLOBALERR_MISSING_ROUTE };
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (!OwnsPort(port)) {
		link.offlineError = ADSERR_CLIENT_PORTNOTOPEN;
		return link;
	}
	link.tmms = ports[port - PORT_BASE].tmms;
	link.connection = SharedConnection(target.netId);
	return link;
}

void AmsRouter::AwaitConnectionAttempts(
	const AmsNetId &ams, std::unique_lock<std::recursive_mutex> &lock)
{
	connection_attempt_events.wait(lock, [&]() {
		return connection_attempts.find(ams) ==
		       connection_attempts.end();
	});
}
