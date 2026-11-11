// SPDX-License-Identifier: MIT
/**
   Copyright (c) Beckhoff Automation GmbH & Co. KG
 */

#include "AdsLib.h"
#include "AdsBackend.h"
#include "AmsRouter.h"
#include "Log.h"
#include "Sockets.h"
#if LIBADS_CLIENT_SECURE
#include "TlsTransport.h"
#endif
#include "wrap_endian.h"
#include <cstring>
#include <limits>

/** The route way, made known to the C-API of libads on first use */
static AmsRouter &GetRouter()
{
	static AmsRouter router;
	static const bool registered = (bhf::ads::RegisterBackend(router), true);
	(void)registered;
	return router;
}

namespace bhf
{
namespace ads
{
long AddLocalRoute(const AmsNetId ams, const char *ip)
{
	try {
		return GetRouter().AddRoute(ams, ip);
	} catch (const std::bad_alloc &) {
		return GLOBALERR_NO_MEMORY;
	} catch (const std::runtime_error &) {
		return GLOBALERR_TARGET_PORT;
	}
}

long AddLocalRouteSecure(const AmsNetId ams, const char *ip,
			 const TlsSettings &tls)
{
	try {
		return GetRouter().AddRoute(ams, ip, &tls);
#if LIBADS_CLIENT_SECURE
	} catch (const TlsError &e) {
		LOG_ERROR(__FUNCTION__ << "(): " << e.what());
		return e.adsError;
#endif
	} catch (const Socket::TimeoutEx &) {
		return ADSERR_CLIENT_SYNCTIMEOUT;
	} catch (const std::bad_alloc &) {
		return GLOBALERR_NO_MEMORY;
	} catch (const std::runtime_error &e) {
		LOG_ERROR(__FUNCTION__ << "(): " << e.what());
		return GLOBALERR_TARGET_PORT;
	}
}

void DelLocalRoute(const AmsNetId ams)
{
	GetRouter().DelRoute(ams);
}

void SetLocalAddress(const AmsNetId ams)
{
	GetRouter().SetLocalAddress(ams);
}

static bool PrependUdpLenTagId(Frame &frame, const uint16_t length,
			       const uint16_t tagId)
{
	frame.prepend(htole(length));
	frame.prepend(htole(tagId));
	return true;
}

static bool PrependUdpTag(Frame &frame, const std::string &value,
			  const uint16_t tagId)
{
	if (value.length() + 1 > std::numeric_limits<uint16_t>::max()) {
		LOG_WARN(__FUNCTION__
			 << "(): value is too long, skipping tagId ("
			 << std::dec << tagId << ")\n");
		return false;
	}
	const auto length = static_cast<uint16_t>(value.length() + 1);
	frame.prepend(value.data(), length);
	return PrependUdpLenTagId(frame, length, tagId);
}

static bool PrependUdpTag(Frame &frame, const AmsNetId &value,
			  const uint16_t tagId)
{
	const uint16_t length = sizeof(value);
	frame.prepend(&value, length);
	return PrependUdpLenTagId(frame, length, tagId);
}

enum UdpTag : uint16_t {
	PASSWORD = 2,
	COMPUTERNAME = 5,
	NETID = 7,
	ROUTENAME = 12,
	USERNAME = 13,
};

enum UdpServiceId : uint32_t {
	SERVERINFO = 1,
	ADDROUTE = 6,
	RESPONSE = 0x80000000,
};

static long SendRecv(const std::string &remote, Frame &f,
		     const uint32_t serviceId)
{
	f.prepend(htole(serviceId));

	static const uint32_t invokeId = 0;
	f.prepend(htole(invokeId));

	static const uint32_t UDP_COOKIE = 0x71146603;
	f.prepend(htole(UDP_COOKIE));

	const auto addresses = GetListOfAddresses(remote, "48899");
	UdpSocket s{ addresses.get() };
	s.write(f);
	f.reset();

	static constexpr auto headerLength =
		sizeof(serviceId) + sizeof(invokeId) + sizeof(UDP_COOKIE);
	timeval timeout{ 5, 0 };
	s.read(f, &timeout);
	if (headerLength > f.size()) {
		LOG_ERROR(__FUNCTION__
			  << "(): frame too short to be AMS response '0x"
			  << std::hex << f.size() << "'\n");
		return ADSERR_DEVICE_INVALIDSIZE;
	}

	const auto cookie = f.pop_letoh<uint32_t>();
	if (UDP_COOKIE != cookie) {
		LOG_ERROR(__FUNCTION__
			  << "(): response contains invalid cookie '" << cookie
			  << "'\n");
		return ADSERR_DEVICE_INVALIDDATA;
	}
	const auto invoke = f.pop_letoh<uint32_t>();
	if (invokeId != invoke) {
		LOG_ERROR(__FUNCTION__
			  << "(): response contains invalid invokeId '"
			  << invoke << "'\n");
		return ADSERR_DEVICE_INVALIDDATA;
	}
	const auto service = f.pop_letoh<uint32_t>();
	if ((UdpServiceId::RESPONSE | serviceId) != service) {
		LOG_ERROR(__FUNCTION__
			  << "(): response contains invalid serviceId '"
			  << std::hex << service << "'\n");
		return ADSERR_DEVICE_INVALIDDATA;
	}
	return 0;
}

// Returns an ADS error code instead of an exception, as AddLocalRoute() does.
template <class Function>
static long ReturnError(const char *function, Function f)
{
	try {
		return f();
	} catch (const Socket::TimeoutEx &) {
		return ADSERR_CLIENT_SYNCTIMEOUT;
	} catch (const std::bad_alloc &) {
		return GLOBALERR_NO_MEMORY;
	} catch (const std::exception &e) {
		LOG_ERROR(function << "(): " << e.what() << '\n');
		return GLOBALERR_TARGET_PORT;
	}
}

static long AddRemoteRouteImpl(const std::string &remote, AmsNetId destNetId,
			       const std::string &destAddr,
			       const std::string &routeName,
			       const std::string &remoteUsername,
			       const std::string &remotePassword)
{
	Frame f{ 256 };
	uint32_t tagCount = 0;
	tagCount += PrependUdpTag(f, destAddr, UdpTag::COMPUTERNAME);
	tagCount += PrependUdpTag(f, remotePassword, UdpTag::PASSWORD);
	tagCount += PrependUdpTag(f, remoteUsername, UdpTag::USERNAME);
	tagCount += PrependUdpTag(f, destNetId, UdpTag::NETID);
	tagCount += PrependUdpTag(f, routeName.empty() ? destAddr : routeName,
				  UdpTag::ROUTENAME);
	f.prepend(htole(tagCount));

	const auto myAddr = AmsAddr{ destNetId, 0 };
	f.prepend(&myAddr, sizeof(myAddr));

	const auto status = SendRecv(remote, f, UdpServiceId::ADDROUTE);
	if (status) {
		return status;
	}

	// We expect at least the AmsAddr and count fields
	if (sizeof(AmsAddr) + sizeof(uint32_t) > f.size()) {
		LOG_ERROR(__FUNCTION__
			  << "(): frame too short to be AMS response '0x"
			  << std::hex << f.size() << "'\n");
		return ADSERR_DEVICE_INVALIDSIZE;
	}

	// ignore AmsAddr in response
	f.remove(sizeof(AmsAddr));

	// process UDP discovery tags
	auto count = f.pop_letoh<uint32_t>();
	while (count--) {
		uint16_t tag;
		uint16_t len;
		if (sizeof(tag) + sizeof(len) > f.size()) {
			LOG_ERROR(
				__FUNCTION__
				<< "(): frame too short to be AMS response '0x"
				<< std::hex << f.size() << "'\n");
			return ADSERR_DEVICE_INVALIDSIZE;
		}

		tag = f.pop_letoh<uint16_t>();
		len = f.pop_letoh<uint16_t>();
		if (1 != tag) {
			LOG_WARN(__FUNCTION__
				 << "(): response contains tagId '0x"
				 << std::hex << tag << "' -> ignoring\n");
			f.remove(len);
			continue;
		}
		if (sizeof(uint32_t) != len) {
			LOG_ERROR(
				__FUNCTION__
				<< "(): response contains invalid tag length '"
				<< std::hex << len << "'\n");
			return ADSERR_DEVICE_INVALIDSIZE;
		}
		if (len > f.size()) {
			LOG_ERROR(__FUNCTION__
				  << "(): response too short for tag length '0x"
				  << std::hex << len << "'\n");
			return ADSERR_DEVICE_INVALIDSIZE;
		}
		return f.pop_letoh<uint32_t>();
	}
	return ADSERR_DEVICE_INVALIDDATA;
}

long AddRemoteRoute(const std::string &remote, AmsNetId destNetId,
		    const std::string &destAddr, const std::string &routeName,
		    const std::string &remoteUsername,
		    const std::string &remotePassword)
{
	return ReturnError(__FUNCTION__, [&] {
		return AddRemoteRouteImpl(remote, destNetId, destAddr,
					  routeName, remoteUsername,
					  remotePassword);
	});
}

static long GetRemoteAddressImpl(const std::string &remote, AmsNetId &netId)
{
	Frame f{ 128 };

	const uint32_t tagCount = 0;
	f.prepend(htole(tagCount));

	const auto myAddr = AmsAddr{ {}, 0 };
	f.prepend(&myAddr, sizeof(myAddr));

	const auto status = SendRecv(remote, f, UdpServiceId::SERVERINFO);
	if (status) {
		return status;
	}

	// We expect at least the AmsAddr
	if (sizeof(netId) > f.size()) {
		LOG_ERROR(__FUNCTION__
			  << "(): frame too short to be AMS response '0x"
			  << std::hex << f.size() << "'\n");
		return ADSERR_DEVICE_INVALIDSIZE;
	}
	memcpy(&netId, f.data(), sizeof(netId));
	return 0;
}

long GetRemoteAddress(const std::string &remote, AmsNetId &netId)
{
	return ReturnError(__FUNCTION__, [&] {
		return GetRemoteAddressImpl(remote, netId);
	});
}

long AddRemoteRouteSecure(const std::string &remote, const AmsNetId ownNetId,
			  const std::string &remoteUsername,
			  const std::string &remotePassword,
			  const TlsSettings &tls, AmsNetId *const remoteNetId)
{
#if LIBADS_CLIENT_SECURE
	return ReturnError(__FUNCTION__, [&]() -> long {
		try {
			const auto addresses = GetListOfAddresses(remote, "8016");
			const auto netId = TlsTransport::AddRoute(
				addresses.get(), remote, tls, ownNetId,
				remoteUsername, remotePassword);
			if (remoteNetId) {
				*remoteNetId = netId;
			}
			return 0;
		} catch (const TlsError &e) {
			LOG_ERROR("AddRemoteRouteSecure(): " << e.what());
			return e.adsError;
		}
	});
#else
	(void)remote, (void)ownNetId, (void)remoteUsername,
		(void)remotePassword, (void)tls, (void)remoteNetId;
	return ADSERR_DEVICE_SRVNOTSUPP; // built without Secure ADS
#endif
}
}
}

long AdsAddRoute(const AmsNetId ams, const char *ip)
{
	return bhf::ads::AddLocalRoute(ams, ip);
}

static bhf::ads::TlsSettings TlsSettingsFrom(const char *directory,
					     const char *name)
{
	bhf::ads::TlsSettings tls;
	tls.directory = directory ? directory : "";
	tls.name = name ? name : "";
	return tls;
}

long AdsAddRemoteRouteSecure(const char *remote, const AmsNetId ownNetId,
			     const char *remoteUsername,
			     const char *remotePassword,
			     const char *tlsDirectory, const char *tlsName,
			     AmsNetId *remoteNetId)
{
	if (!remote || !remoteUsername || !remotePassword) {
		return ADSERR_CLIENT_INVALIDPARM;
	}
	return bhf::ads::AddRemoteRouteSecure(remote, ownNetId, remoteUsername,
					      remotePassword,
					      TlsSettingsFrom(tlsDirectory, tlsName),
					      remoteNetId);
}

long AdsAddRouteSecure(const AmsNetId ams, const char *ip,
		       const char *tlsDirectory)
{
	if (!ip) {
		return ADSERR_CLIENT_INVALIDPARM;
	}
	return bhf::ads::AddLocalRouteSecure(ams, ip,
					     TlsSettingsFrom(tlsDirectory, nullptr));
}

long AdsAddRoutePsk(const AmsNetId ams, const char *ip, const char *pskIdentity,
		    const char *pskPassword)
{
	if (!ip || !pskIdentity || !*pskIdentity || !pskPassword) {
		return ADSERR_CLIENT_INVALIDPARM;
	}
	auto tls = TlsSettingsFrom(nullptr, nullptr);
	tls.pskIdentity = pskIdentity;
	tls.pskPassword = pskPassword;
	return bhf::ads::AddLocalRouteSecure(ams, ip, tls);
}

long AdsAddRouteSca(const AmsNetId ams, const char *ip, const char *caFile,
		    const char *certFile, const char *keyFile,
		    const char *serverName)
{
	if (!ip || !caFile || !*caFile || !certFile || !keyFile) {
		return ADSERR_CLIENT_INVALIDPARM;
	}
	auto tls = TlsSettingsFrom(nullptr, nullptr);
	tls.caFile = caFile;
	tls.certFile = certFile;
	tls.keyFile = keyFile;
	tls.serverName = serverName ? serverName : "";
	return bhf::ads::AddLocalRouteSecure(ams, ip, tls);
}

void AdsDelRoute(const AmsNetId ams)
{
	bhf::ads::DelLocalRoute(ams);
}

void AdsSetLocalAddress(const AmsNetId ams)
{
	bhf::ads::SetLocalAddress(ams);
}

ads_i32 AdsPortOpenRouteEx()
{
	const auto port = GetRouter().OpenPort();
	bhf::ads::SetLastError(port ? 0 : ROUTERERR_NOMOREQUEUES);
	return static_cast<ads_i32>(port);
}
