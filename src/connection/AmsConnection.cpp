// SPDX-License-Identifier: MIT
/**
   Copyright (c) Beckhoff Automation GmbH & Co. KG
 */

#include "AmsConnection.h"
#include "Log.h"

// C++14 needs this definition: std::chrono::seconds takes the value by
// reference, which odr-uses the constant (unoptimized builds fail to link)
const int AmsConnection::RECONNECT_INTERVAL_S;

AmsResponse::AmsResponse()
	: request(nullptr)
	, wasWritten(false)
{
}

void AmsResponse::Notify(const uint32_t error)
{
	std::unique_lock<std::mutex> lock(mutex);
	errorCode = error;
	wasWritten = true;
	cv.notify_all();
}

uint32_t AmsResponse::Wait()
{
	std::unique_lock<std::mutex> lock(mutex);

	cv.wait_until(lock, request.load()->deadline,
		      [&]() { return !invokeId.load(); });

	if (invokeId.exchange(0)) {
		/* invokeId wasn't consumed -> AmsConnection::recv() didn't got a valid response until now */
		return ADSERR_CLIENT_SYNCTIMEOUT;
	}

	/* AmsConnection::recv() is currently processing a response and using the user supplied buffer, we need to wait until that finished */
	cv.wait(lock, [&]() { return wasWritten; });
	return errorCode;
}

SharedDispatcher
AmsConnection::DispatcherListAdd(const VirtualConnection &connection)
{
	const auto dispatcher = DispatcherListGet(connection);
	if (dispatcher) {
		return dispatcher;
	}
	std::lock_guard<std::recursive_mutex> lock(dispatcherListMutex);
	return dispatcherList
		.emplace(connection,
			 std::make_shared<NotificationDispatcher>(std::bind(
				 &AmsConnection::DeleteNotification, this,
				 connection.second, std::placeholders::_1,
				 std::placeholders::_2, connection.first)))
		.first->second;
}

SharedDispatcher
AmsConnection::DispatcherListGet(const VirtualConnection &connection)
{
	std::lock_guard<std::recursive_mutex> lock(dispatcherListMutex);

	const auto it = dispatcherList.find(connection);
	if (it != dispatcherList.end()) {
		return it->second;
	}
	return {};
}

AmsConnection::AmsConnection(Router &__router,
			     const struct addrinfo *const destination,
			     const BeforeReceive &beforeReceive)
	: AmsConnection(__router,
			std::unique_ptr<AmsTransport>(new TcpTransport{ destination }),
			beforeReceive)
{
}

AmsConnection::AmsConnection(Router &__router,
			     std::unique_ptr<AmsTransport> transport,
			     const BeforeReceive &beforeReceive)
	: AmsConnection(__router, std::move(transport), beforeReceive, {}, 0)
{
}

namespace
{
// The first connect runs before the object exists, nothing can abort it.
const std::atomic<bool> &NeverAbort()
{
	static const std::atomic<bool> never{ false };
	return never;
}
}

AmsConnection::AmsConnection(Router &__router, Connector connect,
			     const uint32_t __offlineError)
	: AmsConnection(__router, connect(NeverAbort()), {}, connect,
			__offlineError)
{
}

AmsConnection::AmsConnection(Router &__router,
			     std::unique_ptr<AmsTransport> transport,
			     const BeforeReceive &beforeReceive,
			     Connector connect, const uint32_t __offlineError)
	: router(__router)
	, connector(std::move(connect))
	, offlineError(__offlineError)
	, aborting(false)
	, socket(std::move(transport))
	, state(State::CONNECTED)
	, stopping(false)
	, closeExpected(false)
	, refCount(0)
	, invokeId(0)
	, ownIp(socket->OwnIp())
{
	if (beforeReceive) {
		beforeReceive(*socket);
	}
	receiver = std::thread(&AmsConnection::TryRecv, this);
}

std::shared_ptr<AmsTransport> AmsConnection::Transport() const
{
	std::lock_guard<std::mutex> lock(stateMutex);
	return socket;
}

bool AmsConnection::IsConnected() const
{
	std::lock_guard<std::mutex> lock(stateMutex);
	return state == State::CONNECTED;
}

void AmsConnection::SetState(const State next)
{
	std::lock_guard<std::mutex> lock(stateMutex);
	state = next;
	stateChanged.notify_all();
}

std::shared_ptr<AmsTransport>
AmsConnection::AwaitConnected(const Timepoint &deadline)
{
	std::unique_lock<std::mutex> lock(stateMutex);
	stateChanged.wait_until(lock, deadline, [&]() {
		return (state != State::DOWN) || stopping;
	});
	if ((state == State::CONNECTED) && !stopping) {
		return socket;
	}
	return {};
}

bool AmsConnection::WriteRouterCommand(const uint16_t command,
				       const void *const data,
				       const size_t length)
{
	const auto tx = Transport();
	if (!tx->HasTcpHeader() || !IsConnected()) {
		return false;
	}
	Frame frame{ sizeof(AmsTcpHeader) + length };
	frame.prepend(data, length);
	frame.prepend(AmsTcpHeader{ static_cast<uint32_t>(length), command });
	return frame.size() == tx->write(frame);
}

void AmsConnection::ExpectClose()
{
	closeExpected = true;
}

AmsConnection::~AmsConnection()
{
	std::shared_ptr<AmsTransport> current;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		stopping = true;
		current = socket;
		stateChanged.notify_all();
	}
	// ends a running connect or handshake of the connector at once
	aborting = true;
	// ends a running read
	current->Shutdown();
	receiver.join();
}

SharedDispatcher
AmsConnection::CreateNotifyMapping(uint32_t hNotify,
				   std::shared_ptr<Notification> notification)
{
	auto dispatcher = DispatcherListAdd(notification->connection);
	notification->hNotify(hNotify);
	dispatcher->Emplace(hNotify, notification);
	return dispatcher;
}

long AmsConnection::DeleteNotification(const AmsAddr &amsAddr, uint32_t hNotify,
				       uint32_t tmms, uint16_t port)
{
	{
		// A target that does not answer would let every delete wait for
		// its timeout, e.g. closing a port with many notifications. The
		// caller forgets the notification anyway.
		std::lock_guard<std::mutex> lock(silentMutex);
		if (silentTargets.count(amsAddr.netId)) {
			return ADSERR_CLIENT_SYNCTIMEOUT;
		}
	}
	AmsRequest request{ amsAddr,
			    port,
			    AoEHeader::DEL_DEVICE_NOTIFICATION,
			    0,
			    nullptr,
			    nullptr,
			    sizeof(hNotify) };
	request.frame.prepend(bhf::ads::htole(hNotify));
	return AdsRequest(request, tmms);
}

bool AmsConnection::IsConnectedTo(const struct addrinfo *targetAddresses) const
{
	return Transport()->IsConnectedTo(targetAddresses);
}

AmsResponse *AmsConnection::Write(AmsTransport &socket, AmsRequest &request,
				  const AmsAddr srcAddr, bool &notSent)
{
	notSent = false;
	const AoEHeader aoeHeader{ request.destAddr.netId,
				   request.destAddr.port,
				   srcAddr.netId,
				   srcAddr.port,
				   request.cmdId,
				   static_cast<uint32_t>(request.frame.size()),
				   GetInvokeId() };
	request.frame.prepend<AoEHeader>(aoeHeader);

	if (socket.HasTcpHeader()) {
		const AmsTcpHeader header{ static_cast<uint32_t>(
			request.frame.size()) };
		request.frame.prepend<AmsTcpHeader>(header);
	}

	auto response = Reserve(&request, srcAddr.port);

	if (!response) {
		return nullptr;
	}

	response->invokeId.store(aoeHeader.invokeId());
	if (request.frame.size() != socket.write(request.frame)) {
		Free(response);
		notSent = true;
		return nullptr;
	}
	return response;
}

long AmsConnection::AdsRequest(AmsRequest &request, const uint32_t timeout)
{
	AmsAddr srcAddr;
	const auto status = router.GetLocalAddress(request.port, &srcAddr);
	if (status) {
		return status;
	}
	request.SetDeadline(timeout);
	std::shared_ptr<AmsTransport> tx;
	if (connector && offlineError) {
		tx = Transport();
		if (!IsConnected()) {
			return offlineError;
		}
	} else if (connector) {
		// like the TcAdsDll with a lost target: wait for the connection
		tx = AwaitConnected(request.deadline);
		if (!tx) {
			return ADSERR_CLIENT_SYNCTIMEOUT;
		}
	} else {
		tx = Transport();
	}
	bool notSent;
	AmsResponse *response = Write(*tx, request, srcAddr, notSent);
	if (response) {
		const auto errorCode = response->Wait();

		Free(response);
		if (errorCode == ADSERR_CLIENT_SYNCTIMEOUT) {
			std::lock_guard<std::mutex> lock(silentMutex);
			silentTargets.insert(request.destAddr.netId);
		}
		return errorCode;
	}
	if (notSent && connector && !offlineError) {
		// Not sent, the connection broke just now. The frame already got
		// its headers, so it cannot be sent again: wait out the timeout
		// like a request that got no answer.
		std::unique_lock<std::mutex> lock(stateMutex);
		stateChanged.wait_until(lock, request.deadline,
					[&]() { return stopping; });
		return ADSERR_CLIENT_SYNCTIMEOUT;
	}
	if (!notSent && (std::chrono::steady_clock::now() >= request.deadline)) {
		return ADSERR_CLIENT_SYNCTIMEOUT; // the port stayed busy
	}
	return -1;
}

uint32_t AmsConnection::GetInvokeId()
{
	uint32_t result;
	do {
		result = invokeId.fetch_add(1);
	} while (!result);
	return result;
}

AmsResponse *AmsConnection::GetPending(const uint32_t id, const uint16_t port)
{
	const int portIndex = router.PortSlot(port);
	if ((portIndex < 0) || (portIndex >= (int)Router::NUM_PORTS_MAX)) {
		LOG_WARN("Port 0x" << std::hex << port << " is out of range");
		return nullptr;
	}

	auto currentId = id;
	if (queue[portIndex].invokeId.compare_exchange_strong(currentId, 0)) {
		return &queue[portIndex];
	}
	LOG_WARN("InvokeId mismatch: waiting for 0x" << std::hex << currentId
						     << " received 0x" << id);
	return nullptr;
}

AmsResponse *AmsConnection::Reserve(AmsRequest *request, const uint16_t port)
{
	const int portIndex = router.PortSlot(port);
	if ((portIndex < 0) || (portIndex >= (int)Router::NUM_PORTS_MAX)) {
		LOG_WARN("Port 0x" << std::hex << port << " is out of range");
		return nullptr;
	}
	AmsRequest *isFree = nullptr;
	std::unique_lock<std::mutex> lock(slotMutex);
	if (!slotFreed.wait_until(lock, request->deadline, [&]() {
		    isFree = nullptr;
		    return queue[portIndex].request.compare_exchange_strong(
			    isFree, request);
	    })) {
		LOG_WARN("Port: " << port << " still in use as " << isFree);
		return nullptr;
	}
	return &queue[portIndex];
}

void AmsConnection::Free(AmsResponse *response)
{
	{
		std::lock_guard<std::mutex> lock(slotMutex);
		response->Release();
	}
	slotFreed.notify_all();
}

void AmsResponse::Release()
{
	wasWritten = false;
	request.store(nullptr);
}

void AmsConnection::Receive(const AmsTransport &rx, void *buffer, size_t bytesToRead,
			    timeval *timeout) const
{
	auto pos = reinterpret_cast<uint8_t *>(buffer);
	while (bytesToRead) {
		const size_t bytesRead = rx.read(pos, bytesToRead, timeout);
		bytesToRead -= bytesRead;
		pos += bytesRead;
	}
}

void AmsConnection::Receive(const AmsTransport &rx, void *buffer, size_t bytesToRead,
			    const Timepoint &deadline) const
{
	const auto now = std::chrono::steady_clock::now();
	const auto usec = std::chrono::duration_cast<std::chrono::microseconds>(
				  deadline - now)
				  .count();
	if (usec <= 0) {
		throw Socket::TimeoutEx("deadline reached already!!!");
	}

	timeval timeout{ (long)(usec / 1000000), (int)(usec % 1000000) };
	Receive(rx, buffer, bytesToRead, &timeout);
}

void AmsConnection::ReceiveJunk(const AmsTransport &rx, size_t bytesToRead) const
{
	uint8_t buffer[1024];
	while (bytesToRead > sizeof(buffer)) {
		Receive(rx, buffer, sizeof(buffer));
		bytesToRead -= sizeof(buffer);
	}
	Receive(rx, buffer, bytesToRead);
}

template <class T>
void AmsConnection::ReceiveFrame(const AmsTransport &rx,
				 AmsResponse *const response, size_t bytesLeft,
				 uint32_t aoeError) const
{
	AmsRequest *const request = response->request.load();
	const auto responseId = response->invokeId.load();
	T header;

	if (aoeError) {
		response->Notify(aoeError);
		ReceiveJunk(rx, bytesLeft);
		return;
	}

	if (bytesLeft < sizeof(header)) {
		LOG_WARN("Frame too short: " << std::dec << bytesLeft << '<'
					     << sizeof(header));
		response->Notify(ADSERR_DEVICE_INVALIDSIZE);
		ReceiveJunk(rx, bytesLeft);
		return;
	}

	if (bytesLeft > sizeof(header) + request->bufferLength) {
		LOG_WARN("Frame too long: "
			 << std::dec << bytesLeft << '>'
			 << sizeof(header) + request->bufferLength);
		response->Notify(ADSERR_DEVICE_INVALIDSIZE);
		ReceiveJunk(rx, bytesLeft);
		return;
	}

	try {
		Receive(rx, &header, sizeof(header), request->deadline);
		bytesLeft -= sizeof(header);
		Receive(rx, request->buffer, bytesLeft, request->deadline);

		if (request->bytesRead) {
			// We already checked bytesLeft <= request->bufferLength
			const auto v = static_cast<std::remove_pointer<decltype(
				AmsRequest::bytesRead)>::type>(bytesLeft);
			*(request->bytesRead) = v;
		}
		response->Notify(header.result());
	} catch (const Socket::TimeoutEx &) {
		LOG_WARN("InvokeId of response: " << std::dec << responseId
						  << " timed out");
		response->Notify(ADSERR_CLIENT_SYNCTIMEOUT);
		ReceiveJunk(rx, bytesLeft);
	}
}

bool AmsConnection::ReceiveNotification(const AmsTransport &rx,
					const AoEHeader &header)
{
	const auto dispatcher = DispatcherListGet(
		VirtualConnection{ header.targetPort(), header.sourceAms() });
	if (!dispatcher) {
		ReceiveJunk(rx, header.length());
		LOG_WARN("No dispatcher found for notification");
		return false;
	}

	auto &ring = dispatcher->ring;
	auto bytesLeft = header.length();
	const size_t bytesNeeded = sizeof(bytesLeft) + bytesLeft;
	if ((bytesNeeded < bytesLeft) || (bytesNeeded > ring.BytesFree())) {
		ReceiveJunk(rx, bytesLeft);
		LOG_WARN("port " << std::dec << header.targetPort()
				 << " receive buffer was full");
		return false;
	}

	/** store AoEHeader.length() in ring buffer to support notification parsing */
	for (size_t i = 0; i < sizeof(bytesLeft); ++i) {
		*ring.write = (bytesLeft >> (8 * i)) & 0xFF;
		ring.Write(1);
	}

	auto chunk = ring.WriteChunk();
	while (bytesLeft > chunk) {
		Receive(rx, ring.write, chunk);
		ring.Write(chunk);
		// We already checked bytesLeft > chunk, well it was not obvious enough for MSVC
		bytesLeft -= static_cast<decltype(bytesLeft)>(chunk);
		chunk = ring.WriteChunk();
	}
	Receive(rx, ring.write, bytesLeft);
	ring.Write(bytesLeft);
	dispatcher->Notify();
	return true;
}

void AmsConnection::TryRecv()
{
	for (;;) {
		const auto rx = Transport();
		bool failed = false;
		try {
			Recv(*rx);
		} catch (const std::runtime_error &e) {
			// an end on purpose (destroyed, or closed by the peer
			// as expected) is no news
			std::lock_guard<std::mutex> lock(stateMutex);
			if (!stopping && !closeExpected) {
				LOG_INFO(e.what());
			}
			failed = true;
		}
		if (!connector || !failed) {
			router.OnConnectionLost(*this);
			return;
		}
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			if (stopping) {
				// destroyed: no callbacks, the owner may hold its lock
				return;
			}
			state = State::DOWN;
			stateChanged.notify_all();
		}
		router.OnConnectionLost(*this);
		if (!Reconnect()) {
			return;
		}
		router.OnReconnected(*this);
	}
}

bool AmsConnection::Reconnect()
{
	for (bool first = true;; first = false) {
		{
			// the first attempt at once: a router that restarts is
			// often back immediately
			std::unique_lock<std::mutex> lock(stateMutex);
			if (!first) {
				stateChanged.wait_for(
					lock,
					std::chrono::seconds(RECONNECT_INTERVAL_S),
					[&]() { return stopping; });
			}
			if (stopping) {
				return false;
			}
		}
		std::unique_ptr<AmsTransport> next;
		bool gaveUp = false;
		try {
			next = connector(aborting);
		} catch (const GiveUp &e) {
			LOG_WARN("Reconnecting given up: " << e.what());
			gaveUp = true;
		} catch (const std::exception &e) {
			LOG_VERBOSE("Reconnecting failed: " << e.what());
		}
		std::unique_lock<std::mutex> lock(stateMutex);
		if (stopping) {
			return false; // a new transport closes with next
		}
		if (gaveUp) {
			state = State::GAVE_UP;
			stateChanged.notify_all();
			lock.unlock();
			router.OnGaveUp(*this);
			return false;
		}
		if (!next) {
			continue;
		}
		socket = std::move(next);
		state = State::CONNECTED;
		stateChanged.notify_all();
		LOG_INFO("Connection established again");
		return true;
	}
}

void AmsConnection::Recv(const AmsTransport &rx)
{
	AmsTcpHeader amsTcpHeader;
	AoEHeader aoeHeader;
	const bool hasTcpHeader = rx.HasTcpHeader();
	for (; ownIp;) {
		if (hasTcpHeader) {
			Receive(rx, amsTcpHeader);
			size_t bytesLeft = amsTcpHeader.length();
			if (amsTcpHeader.command()) {
				/* router command of the local TwinCAT router, no AoE frame */
				if (bytesLeft > 1024) {
					LOG_WARN("Router command 0x"
						 << std::hex << amsTcpHeader.command()
						 << " too long: " << std::dec << bytesLeft);
					ReceiveJunk(rx, bytesLeft);
					continue;
				}
				uint8_t payload[1024];
				Receive(rx, payload, bytesLeft);
				router.OnRouterCommand(*this, amsTcpHeader.command(),
						       payload, bytesLeft);
				continue;
			}
			if (sizeof(aoeHeader) > bytesLeft) {
				LOG_WARN("Frame to short to be AoE");
				ReceiveJunk(rx, bytesLeft);
				continue;
			}

			Receive(rx, aoeHeader);
			bytesLeft -= sizeof(aoeHeader);
			if (aoeHeader.length() != bytesLeft) {
				LOG_WARN("AoE length " << std::dec << aoeHeader.length()
						       << " doesn't match the frame: "
						       << std::dec << bytesLeft);
				ReceiveJunk(rx, bytesLeft);
				continue;
			}
		} else {
			/* Secure ADS: the AoE header alone tells the frame length */
			Receive(rx, aoeHeader);
		}

		{
			// the target answers again
			std::lock_guard<std::mutex> lock(silentMutex);
			silentTargets.erase(aoeHeader.sourceAddr());
		}

		if (aoeHeader.cmdId() == AoEHeader::DEVICE_NOTIFICATION) {
			ReceiveNotification(rx, aoeHeader);
			continue;
		}

		auto response = GetPending(aoeHeader.invokeId(),
					   aoeHeader.targetPort());
		if (!response) {
			LOG_WARN("No response pending");
			ReceiveJunk(rx, aoeHeader.length());
			continue;
		}

		switch (aoeHeader.cmdId()) {
		case AoEHeader::READ_DEVICE_INFO:
		case AoEHeader::WRITE:
		case AoEHeader::READ_STATE:
		case AoEHeader::WRITE_CONTROL:
		case AoEHeader::ADD_DEVICE_NOTIFICATION:
		case AoEHeader::DEL_DEVICE_NOTIFICATION:
			ReceiveFrame<AoEResponseHeader>(rx, response,
							aoeHeader.length(),
							aoeHeader.errorCode());
			continue;

		case AoEHeader::READ:
		case AoEHeader::READ_WRITE:
			ReceiveFrame<AoEReadResponseHeader>(rx, 
				response, aoeHeader.length(),
				aoeHeader.errorCode());
			continue;

		default:
			LOG_WARN("Unkown AMS command id");
			response->Notify(ADSERR_CLIENT_SYNCRESINVALID);
			ReceiveJunk(rx, aoeHeader.length());
		}
	}
}
