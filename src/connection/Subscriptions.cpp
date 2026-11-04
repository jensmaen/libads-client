// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2026 Jens Fresenborg
 */

#include "Subscriptions.h"
#include "Log.h"

#include <cstring>

Subscriptions::Subscriptions(Resolve __resolve)
	: resolve(std::move(__resolve))
	, next(1)
{
}

long Subscriptions::Create(uint32_t hNotify, const Entry &e, const Link &link,
			   uint32_t &hServer, SharedDispatcher &dispatcher)
{
	uint8_t buffer[sizeof(uint32_t)];
	AmsRequest request{ e.target,
			    e.port,
			    AoEHeader::ADD_DEVICE_NOTIFICATION,
			    sizeof(buffer),
			    buffer,
			    nullptr,
			    e.request.size() };
	request.frame.prepend(e.request.data(), e.request.size());
	const long status = link.connection->AdsRequest(request, link.tmms);
	if (status) {
		return status;
	}
	hServer = bhf::ads::letoh<uint32_t>(buffer);
	auto notify = std::make_shared<Notification>(
		e.callback, e.hUser, e.length, e.target, link.sourcePort);
	dispatcher = link.connection->CreateNotifyMapping(hServer, notify);
	// the callback shall see our handle, not the one of the target
	notify->hNotify(hNotify);
	return 0;
}

bool Subscriptions::Settle(Entry &e, uint32_t hServer,
			   SharedDispatcher dispatcher,
			   std::shared_ptr<AmsConnection> &drop)
{
	// Called with the mutex locked, after a successful Create().
	e.busy = false;
	if (e.lost) {
		// The connection was lost meanwhile: the handle is gone with
		// it, the notification waits to be created again.
		dispatcher->Forget(hServer);
		e.lost = false;
		drop = std::move(e.connection);
		e.connection.reset();
		return false;
	}
	e.hServer = hServer;
	e.dispatcher = std::move(dispatcher);
	return true;
}

long Subscriptions::Add(const AmsRequest &request,
			std::shared_ptr<Notification> notify,
			uint32_t *pNotification)
{
	const auto link = resolve(request.port, request.destAddr);
	if (!link.connection) {
		return link.offlineError;
	}

	Entry e;
	e.port = request.port;
	e.target = request.destAddr;
	e.request.assign(request.frame.data(),
			 request.frame.data() + request.frame.size());
	e.callback = notify->Callback();
	e.hUser = notify->User();
	e.length = notify->Size();
	e.hServer = 0;
	e.connection = link.connection;
	e.stale = false;
	e.busy = true;
	e.lost = false;

	uint32_t hNotify;
	{
		std::lock_guard<std::mutex> lock(mutex);
		do {
			hNotify = next++;
		} while (!hNotify || entries.count(hNotify));
		entries.emplace(hNotify, e);
	}

	uint32_t hServer = 0;
	SharedDispatcher dispatcher;
	const long status = Create(hNotify, e, link, hServer, dispatcher);

	std::shared_ptr<AmsConnection> drop;
	std::unique_lock<std::mutex> lock(mutex);
	const auto it = entries.find(hNotify);
	if (status) {
		if (it != entries.end()) {
			drop = std::move(it->second.connection);
			entries.erase(it);
		}
		return status;
	}
	if (it == entries.end()) {
		// port closed meanwhile
		lock.unlock();
		dispatcher->Erase(hServer, link.tmms);
		return ADSERR_CLIENT_PORTNOTOPEN;
	}
	Settle(it->second, hServer, std::move(dispatcher), drop);
	*pNotification = hNotify;
	return 0;
}

long Subscriptions::Delete(uint16_t port, const AmsAddr &target,
			   uint32_t hNotify, uint32_t tmms)
{
	// the connection must outlive the dispatcher's call into it
	std::shared_ptr<AmsConnection> connection;
	SharedDispatcher dispatcher;
	uint32_t hServer;
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto it = entries.find(hNotify);
		if ((it == entries.end()) || (it->second.port != port) ||
		    memcmp(&it->second.target, &target, sizeof(target))) {
			return ADSERR_CLIENT_REMOVEHASH;
		}
		connection = std::move(it->second.connection);
		dispatcher = std::move(it->second.dispatcher);
		hServer = it->second.hServer;
		entries.erase(it);
	}
	if (!dispatcher) {
		return 0; // not on the target at the moment
	}
	return dispatcher->Erase(hServer, tmms);
}

void Subscriptions::ClosePort(uint16_t port, uint32_t tmms)
{
	struct Active {
		std::shared_ptr<AmsConnection> connection;
		SharedDispatcher dispatcher;
		uint32_t hServer;
	};
	std::vector<Active> active;
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (auto it = entries.begin(); it != entries.end();) {
			if (it->second.port != port) {
				++it;
				continue;
			}
			if (it->second.dispatcher) {
				active.push_back(
					{ std::move(it->second.connection),
					  std::move(it->second.dispatcher),
					  it->second.hServer });
			}
			it = entries.erase(it);
		}
	}
	for (auto &a : active) {
		a.dispatcher->Erase(a.hServer, tmms);
	}
}

void Subscriptions::MarkStale(uint16_t port)
{
	std::lock_guard<std::mutex> lock(mutex);
	for (auto &e : entries) {
		if ((e.second.port == port) && e.second.dispatcher) {
			e.second.stale = true;
		}
	}
}

void Subscriptions::Detach(const AmsConnection &connection)
{
	std::vector<std::shared_ptr<AmsConnection> > connections;
	std::vector<SharedDispatcher> dispatchers;
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (auto &entry : entries) {
			auto &e = entry.second;
			if (e.connection.get() != &connection) {
				continue;
			}
			if (e.dispatcher) {
				e.dispatcher->Forget(e.hServer);
				dispatchers.push_back(std::move(e.dispatcher));
				e.dispatcher.reset();
			}
			e.hServer = 0;
			e.stale = false;
			if (e.busy) {
				e.lost = true;
			}
			connections.push_back(std::move(e.connection));
			e.connection.reset();
		}
	}
	// the dispatchers before the connection they call back into
	dispatchers.clear();
	connections.clear();
}

void Subscriptions::Lost(const AmsConnection &connection)
{
	// The connection keeps its dispatchers (and lives on), so releasing
	// ours destroys nothing here.
	std::vector<SharedDispatcher> dispatchers;
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (auto &entry : entries) {
			auto &e = entry.second;
			if (e.connection.get() != &connection) {
				continue;
			}
			if (e.dispatcher) {
				e.dispatcher->Forget(e.hServer);
				dispatchers.push_back(std::move(e.dispatcher));
				e.dispatcher.reset();
			}
			e.hServer = 0;
			e.stale = false;
			if (e.busy) {
				e.lost = true;
			}
		}
	}
}

void Subscriptions::Resubscribe()
{
	std::vector<uint32_t> due;
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (const auto &e : entries) {
			if (!e.second.busy &&
			    (!e.second.dispatcher || e.second.stale)) {
				due.push_back(e.first);
			}
		}
	}
	for (const auto hNotify : due) {
		Resubscribe(hNotify);
	}
}

void Subscriptions::Resubscribe(uint32_t hNotify)
{
	uint16_t port;
	AmsAddr target;
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto it = entries.find(hNotify);
		if ((it == entries.end()) || it->second.busy) {
			return;
		}
		port = it->second.port;
		target = it->second.target;
	}
	const auto link = resolve(port, target);
	if (!link.connection || !link.connection->IsConnected()) {
		return; // no way to the target yet, try again next time
	}

	Entry e;
	std::shared_ptr<AmsConnection> staleConnection;
	SharedDispatcher stale;
	uint32_t hStale;
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto it = entries.find(hNotify);
		if ((it == entries.end()) || it->second.busy ||
		    (it->second.dispatcher && !it->second.stale)) {
			return; // deleted or created meanwhile
		}
		staleConnection = std::move(it->second.connection);
		stale = std::move(it->second.dispatcher);
		hStale = it->second.hServer;
		it->second.dispatcher.reset();
		it->second.hServer = 0;
		it->second.stale = false;
		it->second.busy = true;
		it->second.lost = false;
		it->second.connection = link.connection;
		e = it->second;
	}
	if (stale) {
		// may still exist if the device itself kept running
		stale->Erase(hStale, link.tmms);
		stale.reset();
	}
	staleConnection.reset();

	uint32_t hServer = 0;
	SharedDispatcher dispatcher;
	const long status = Create(hNotify, e, link, hServer, dispatcher);

	std::shared_ptr<AmsConnection> drop;
	std::unique_lock<std::mutex> lock(mutex);
	const auto it = entries.find(hNotify);
	if (status) {
		if (it != entries.end()) {
			it->second.busy = false;
			it->second.lost = false;
			drop = std::move(it->second.connection);
			it->second.connection.reset();
		}
		LOG_VERBOSE("Creating notification " << std::dec << hNotify
						     << " again failed: 0x"
						     << std::hex << status);
		return; // try again next time
	}
	if (it == entries.end()) {
		// deleted by the program meanwhile, so delete it on the target
		lock.unlock();
		dispatcher->Erase(hServer, link.tmms);
		return;
	}
	if (Settle(it->second, hServer, std::move(dispatcher), drop)) {
		LOG_INFO("Notification " << std::dec << hNotify << " of port "
					 << port << " created again");
	}
}
