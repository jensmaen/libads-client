// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2020 - 2022 Beckhoff Automation GmbH & Co. KG
 */

#pragma once

#include "StandaloneAdsLib.h"
#include "TlsSettings.h"

#ifdef BHF_ADS_EXPORT_C
extern "C" {
#endif
long AdsAddRoute(AmsNetId ams, const char *ip);
void AdsDelRoute(AmsNetId ams);
void AdsSetLocalAddress(AmsNetId ams);

/**
 * C form of bhf::ads::AddRemoteRouteSecure() (Secure ADS).
 * @param[in] tlsDirectory folder for the certificates, NULL: default (TlsSettings)
 * @param[in] tlsName name for a newly created own certificate, NULL: computer name
 * @param[out] remoteNetId may be NULL
 */
long AdsAddRemoteRouteSecure(const char *remote, AmsNetId ownNetId,
			     const char *remoteUsername,
			     const char *remotePassword,
			     const char *tlsDirectory, const char *tlsName,
			     AmsNetId *remoteNetId);
/** C form of bhf::ads::AddLocalRouteSecure(); tlsDirectory as above. */
long AdsAddRouteSecure(AmsNetId ams, const char *ip, const char *tlsDirectory);
/** Like AdsAddRouteSecure(), but with a pre-shared key (TlsSettings::pskIdentity). */
long AdsAddRoutePsk(AmsNetId ams, const char *ip, const char *pskIdentity,
		    const char *pskPassword);
/** Like AdsAddRouteSecure(), but with certificates of an own CA (TlsSettings::caFile); serverName may be NULL. */
long AdsAddRouteSca(AmsNetId ams, const char *ip, const char *caFile,
		    const char *certFile, const char *keyFile,
		    const char *serverName);

/**
 * Establishes a communication port that reaches its targets directly over
 * ADS routes added with bhf::ads::AddLocalRoute() (Beckhoff's standalone
 * AdsLib behaviour of AdsPortOpenEx()).
 * @return port number of a new Ads port or 0 if no more ports available
 */
ads_i32 AdsPortOpenRouteEx();
#ifdef BHF_ADS_EXPORT_C
}
#endif

namespace bhf
{
namespace ads
{
/**
 * Add new ams route to target system
 * @param[in] ams address of the target system
 * @param[in] ip address of the target system
 * @return [ADS Return Code](https://infosys.beckhoff.com/content/1031/tcadscommon/html/ads_returncodes.htm?id=1666172286265530469)
 */
long AddLocalRoute(AmsNetId ams, const char *ip);

/**
 * Delete ams route that had previously been added with AddLocalRoute().
 * @param[in] ams address of the target system
 */
void DelLocalRoute(AmsNetId ams);

/**
 * Change local NetId
 * @param[in] ams local AmsNetId
 */
void SetLocalAddress(AmsNetId ams);

/**
 * Add an ADS route to a remote TwinCAT system
 * @param[in] remote hostname or ip address of the remote TwinCAT system
 * @param[in] destNetId AmsNetId of the routes destination
 * @param[in] destAddr hostname or ip address of the routes destination
 * @param[in] routeName name of the new route
 * @param[in] remoteUsername username on the remote TwinCAT system
 * @param[in] remotePassword password for the user on the remote TwinCAT system
 * @return [ADS Return Code](https://infosys.beckhoff.com/content/1031/tcadscommon/html/ads_returncodes.htm?id=1666172286265530469),
 *         no exceptions: ADSERR_CLIENT_SYNCTIMEOUT if the remote does not
 *         answer, GLOBALERR_TARGET_PORT for an unknown host or a socket error
 */
long AddRemoteRoute(const std::string &remote, AmsNetId destNetId,
		    const std::string &destAddr, const std::string &routeName,
		    const std::string &remoteUsername,
		    const std::string &remotePassword);

/**
 * Read AmsNetId of some TwinCAT remote host
 * @param[in] remote hostname or ip address of the remote TwinCAT system
 * @param[out] netId on success the AmsNetId of the remote TwinCAT system is written here
 * @return [ADS Return Code](https://infosys.beckhoff.com/content/1031/tcadscommon/html/ads_returncodes.htm?id=1666172286265530469),
 *         no exceptions (as AddRemoteRoute())
 */
long GetRemoteAddress(const std::string &remote, AmsNetId &netId);

/**
 * Secure ADS (TLS on TCP 8016, self-signed certificate): add a route to us on
 * a remote TwinCAT system, like "Add Route" with "Secure ADS" in the XAE. The
 * own certificate is created on first use (TlsSettings), the remote's
 * certificate is stored; AddLocalRouteSecure() later insists on it.
 * @param[in] remote ip address (or hostname) of the remote TwinCAT system
 * @param[in] ownNetId AmsNetId the route on the remote leads to (us);
 *            0.0.0.0.0.0: own IP + .1.1
 * @param[in] remoteUsername user on the remote system, e.g. Administrator
 * @param[in] remotePassword password of that user, sent only inside TLS
 * @param[out] remoteNetId if not nullptr, the AmsNetId of the remote system
 * @return [ADS Return Code](https://infosys.beckhoff.com/content/1031/tcadscommon/html/ads_returncodes.htm?id=1666172286265530469),
 *         no exceptions: ADSERR_DEVICE_INVALIDACCESS for a wrong user or
 *         password, ADSERR_DEVICE_CERTIFICATEINVALID for certificate problems,
 *         ADSERR_CLIENT_SYNCTIMEOUT, GLOBALERR_TARGET_PORT otherwise
 */
long AddRemoteRouteSecure(const std::string &remote, AmsNetId ownNetId,
			  const std::string &remoteUsername,
			  const std::string &remotePassword,
			  const TlsSettings &tls = {},
			  AmsNetId *remoteNetId = nullptr);

/**
 * Like AddLocalRoute(), but the connection to ip uses Secure ADS. The target
 * must have a route to us with our certificate (AddRemoteRouteSecure()).
 * @return ADS return code as AddRemoteRouteSecure()
 */
long AddLocalRouteSecure(AmsNetId ams, const char *ip,
			 const TlsSettings &tls = {});
}
}
