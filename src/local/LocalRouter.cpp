// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2026 Jens Fresenborg
 */

#include "LocalRouter.h"
#include "Log.h"

#include <cstring>
#include <stdexcept>

LocalRouter::LocalRouter(std::string __host)
	: host(std::move(__host))
	, subscriptions([this](uint16_t port, const AmsAddr &) {
		return Resolve(port);
	})
	, stopWorker(false)
	, worker(&LocalRouter::Work, this)
{
}

LocalRouter::~LocalRouter()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopWorker = true;
	}
	wake.notify_all();
	worker.join();

	std::vector<uint16_t> open;
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (const auto &p : ports) {
			open.push_back(p.first);
		}
	}
	for (const auto port : open) {
		ClosePort(port);
	}
}

AmsAddr LocalRouter::Register(const AmsTransport &socket)
{
	Frame request{ sizeof(AmsTcpHeader) + sizeof(uint16_t) };
	request.prepend(bhf::ads::htole<uint16_t>(0)); // let the router choose the port
	request.prepend(AmsTcpHeader{ sizeof(uint16_t), CMD_PORT_CONNECT });
	if (request.size() != socket.write(request)) {
		throw std::runtime_error("port connect request not sent");
	}

	uint8_t answer[sizeof(AmsTcpHeader) + sizeof(AmsAddr)];
	size_t received = 0;
	while (received < sizeof(answer)) {
		timeval timeout{ 5, 0 };
		const auto n = socket.read(answer + received,
					   sizeof(answer) - received, &timeout);
		if (!n) {
			throw std::runtime_error("port connect answer not received");
		}
		received += n;
	}

	const AmsTcpHeader header{ answer };
	if ((header.command() != CMD_PORT_CONNECT) ||
	    (header.length() != sizeof(AmsAddr))) {
		throw std::runtime_error("unexpected port connect answer");
	}
	AmsAddr addr;
	memcpy(&addr.netId, answer + sizeof(header), sizeof(addr.netId));
	addr.port = bhf::ads::letoh<uint16_t>(answer + sizeof(header) +
					      sizeof(addr.netId));
	if (!addr.port) {
		throw std::runtime_error("router assigned port 0");
	}
	return addr;
}

std::shared_ptr<AmsConnection> LocalRouter::Connect(AmsAddr &addr)
{
	auto addresses = bhf::ads::GetListOfAddresses(host, "48898");
	return std::make_shared<AmsConnection>(
		*this, addresses.get(),
		[&addr](const AmsTransport &socket) { addr = Register(socket); });
}

uint16_t LocalRouter::OpenPort()
{
	try {
		auto p = std::make_shared<Port>();
		p->tmms = 5000;
		p->lost = false;
		p->state = ROUTER_STARTED;
		p->connection = Connect(p->addr);

		std::lock_guard<std::mutex> lock(mutex);
		// The first assigned port becomes the handle, unless an older
		// port already uses that number as its handle.
		uint16_t handle = p->addr.port;
		while (!handle || ports.count(handle)) {
			++handle;
		}
		ports.emplace(handle, p);
		return handle;
	} catch (const std::exception &e) {
		LOG_WARN("Opening a port at the local router " << host
							     << " failed: "
							     << e.what());
		return 0;
	}
}

bool LocalRouter::IsConnected(uint16_t port) const
{
	std::lock_guard<std::mutex> lock(mutex);
	const auto it = ports.find(port);
	return (it != ports.end()) && it->second->connection &&
	       !it->second->lost;
}

void LocalRouter::SetRouterEventHandler(RouterEventHandler handler)
{
	std::lock_guard<std::mutex> lock(mutex);
	routerEvent = std::move(handler);
}

void LocalRouter::Report(uint16_t handle, Port &p, uint32_t state)
{
	// Called with the mutex locked; delivered by the worker thread.
	p.state = state;
	events.emplace_back(handle, state);
	wake.notify_all();
}

std::shared_ptr<LocalRouter::Port> LocalRouter::Find(uint16_t port) const
{
	std::lock_guard<std::mutex> lock(mutex);
	const auto it = ports.find(port);
	return (it != ports.end()) ? it->second : nullptr;
}

uint16_t LocalRouter::FindHandle(const AmsConnection &connection) const
{
	for (const auto &p : ports) {
		if (p.second->connection.get() == &connection) {
			return p.first;
		}
	}
	return 0;
}

bool LocalRouter::OwnsPort(uint16_t port)
{
	return !!Find(port);
}

Subscriptions::Link LocalRouter::Resolve(uint16_t port) const
{
	std::lock_guard<std::mutex> lock(mutex);
	Subscriptions::Link link{ nullptr, 0, 0, ROUTERERR_NOTREGISTERED };
	const auto it = ports.find(port);
	if (it == ports.end()) {
		link.offlineError = ADSERR_CLIENT_PORTNOTOPEN;
		return link;
	}
	if (!it->second->lost) {
		link.connection = it->second->connection;
	}
	link.tmms = it->second->tmms;
	link.sourcePort = it->second->addr.port;
	return link;
}

long LocalRouter::ClosePort(uint16_t port)
{
	std::shared_ptr<Port> p;
	uint32_t tmms;
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto it = ports.find(port);
		if (it == ports.end()) {
			return ADSERR_CLIENT_PORTNOTOPEN;
		}
		p = it->second;
		tmms = p->tmms;
		ports.erase(it);
	}

	// Notifications are deleted over the connection, so before it goes away.
	subscriptions.ClosePort(port, tmms);
	if (p->connection) {
		// the router closes the connection of the port
		p->connection->ExpectClose();
		const auto lePort = bhf::ads::htole(p->addr.port);
		if (!p->connection->WriteRouterCommand(CMD_PORT_CLOSE, &lePort,
						       sizeof(lePort))) {
			LOG_WARN("Port close for " << std::dec << p->addr.port
						   << " not sent");
		}
	}
	p->connection.reset();
	return 0;
}

long LocalRouter::GetLocalAddress(uint16_t port, AmsAddr *pAddr)
{
	const auto p = Find(port);
	if (!p) {
		return ADSERR_CLIENT_PORTNOTOPEN;
	}
	std::lock_guard<std::mutex> lock(mutex);
	*pAddr = p->addr;
	return 0;
}

long LocalRouter::GetTimeout(uint16_t port, uint32_t &timeout)
{
	const auto p = Find(port);
	if (!p) {
		return ADSERR_CLIENT_PORTNOTOPEN;
	}
	std::lock_guard<std::mutex> lock(mutex);
	timeout = p->tmms;
	return 0;
}

long LocalRouter::SetTimeout(uint16_t port, uint32_t timeout)
{
	const auto p = Find(port);
	if (!p) {
		return ADSERR_CLIENT_PORTNOTOPEN;
	}
	std::lock_guard<std::mutex> lock(mutex);
	p->tmms = timeout;
	return 0;
}

long LocalRouter::AdsRequest(AmsRequest &request)
{
	if (request.bytesRead) {
		*request.bytesRead = 0;
	}
	std::shared_ptr<AmsConnection> connection;
	uint32_t tmms;
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto it = ports.find(request.port);
		if (it == ports.end()) {
			return ADSERR_CLIENT_PORTNOTOPEN;
		}
		connection = it->second->connection;
		tmms = it->second->tmms;
	}
	if (!connection) {
		return ROUTERERR_NOTREGISTERED; // connection to the router lost
	}
	return connection->AdsRequest(request, tmms);
}

long LocalRouter::AddNotification(AmsRequest &request, uint32_t *pNotification,
				  std::shared_ptr<Notification> notify)
{
	if (request.bytesRead) {
		*request.bytesRead = 0;
	}
	return subscriptions.Add(request, std::move(notify), pNotification);
}

long LocalRouter::DelNotification(uint16_t port, const AmsAddr *pAddr,
				  uint32_t hNotification)
{
	uint32_t tmms;
	if (GetTimeout(port, tmms)) {
		return ADSERR_CLIENT_PORTNOTOPEN;
	}
	return subscriptions.Delete(port, *pAddr, hNotification, tmms);
}

int LocalRouter::PortSlot(uint16_t port) const
{
	// One port per connection, so every connection needs only slot 0.
	// Frames carry the router port, requests the handle: accept both.
	(void)port;
	return 0;
}

void LocalRouter::OnRouterCommand(AmsConnection &from, uint16_t command,
				  const uint8_t *data, size_t length)
{
	switch (command) {
	case CMD_ROUTER_NOTE:
		if (length >= sizeof(uint32_t)) {
			const auto state = bhf::ads::letoh<uint32_t>(data);
			LOG_INFO("Local TwinCAT router state changed to "
				 << std::dec << state);
			std::lock_guard<std::mutex> lock(mutex);
			const auto handle = FindHandle(from);
			if (!handle) {
				return;
			}
			Report(handle, *ports[handle], state);
			if (state != ROUTER_STARTED) {
				return;
			}
			// TwinCAT restarted behind the open connection: the
			// notifications on its devices are gone.
			subscriptions.MarkStale(handle);
			wake.notify_all();
		}
		return;
	case CMD_PORT_CLOSE:
		LOG_INFO("Local TwinCAT router closed a port");
		return;
	default:
		LOG_WARN("Unknown router command 0x" << std::hex << command);
	}
}

void LocalRouter::OnConnectionLost(AmsConnection &from)
{
	std::lock_guard<std::mutex> lock(mutex);
	const auto handle = FindHandle(from);
	if (handle) {
		LOG_INFO("Connection of port " << std::dec << handle
					       << " to the local router lost");
		auto &p = *ports[handle];
		p.lost = true;
		if (p.state != ROUTER_STOPPED) {
			Report(handle, p, ROUTER_STOPPED);
		}
		wake.notify_all();
	}
}

void LocalRouter::Work()
{
	std::unique_lock<std::mutex> lock(mutex);
	while (!stopWorker) {
		wake.wait_for(lock, std::chrono::seconds(1));
		if (stopWorker) {
			return;
		}
		std::vector<uint16_t> handles;
		for (const auto &p : ports) {
			handles.push_back(p.first);
		}
		auto due = std::move(events);
		events.clear();
		const auto handler = routerEvent;
		lock.unlock();
		if (handler) {
			for (const auto &e : due) {
				handler(e.first, e.second);
			}
		}
		for (const auto handle : handles) {
			Release(handle);
		}
		subscriptions.Resubscribe();
		lock.lock();
	}
}

void LocalRouter::Release(uint16_t handle)
{
	// The connection is gone for good (as with the TcAdsDll): the port stays open, but every
	// request fails until the program closes it. The receive thread may not
	// destroy its own connection, so that happens here.
	std::shared_ptr<AmsConnection> old;
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto it = ports.find(handle);
		if ((it == ports.end()) || !it->second->lost) {
			return;
		}
		auto &p = *it->second;
		old = std::move(p.connection);
		p.lost = false;
	}
	if (old) {
		// the dispatchers call back into the connection
		subscriptions.Detach(*old);
	}
	old.reset();
}
