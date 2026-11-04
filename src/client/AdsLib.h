// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2020 - 2022 Beckhoff Automation GmbH & Co. KG
 */

#pragma once

#include "StandaloneAdsLib.h"
#include "TlsSettings.h"

#include <type_traits>

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
 * Reads data synchronously from an ADS server.
 * @param[in] port port number of an Ads port that had previously been opened with AdsPortOpenEx().
 * @param[in] pAddr Structure with NetId and port number of the ADS server.
 * @param[in] indexGroup Index Group.
 * @param[in] indexOffset Index Offset.
 * @param[in] bufferLength Length of the data in bytes.
 * @param[out] buffer Pointer to a data buffer that will receive the data.
 * @param[out] bytesRead pointer to a variable. If successful, this variable will return the number of actually read data bytes.
 * @return [ADS Return Code](https://infosys.beckhoff.com/content/1031/tcadscommon/html/ads_returncodes.htm?id=1666172286265530469)
 */
ads_i32 AdsSyncReadReqEx2(ads_i32 port, const AmsAddr *pAddr, ads_ui32 indexGroup,
		       ads_ui32 indexOffset, ads_ui32 bufferLength,
		       void *buffer, ads_ui32 *bytesRead);

/**
 * Reads the identification and version number of an ADS server.
 * @param[in] port port number of an Ads port that had previously been opened with AdsPortOpenEx().
 * @param[in] pAddr Structure with NetId and port number of the ADS server.
 * @param[out] devName Pointer to a character string of at least 16 bytes, that will receive the name of the ADS device.
 * @param[out] version Address of a variable of type AdsVersion, which will receive the version number, revision number and the build number.
 * @return [ADS Return Code](https://infosys.beckhoff.com/content/1031/tcadscommon/html/ads_returncodes.htm?id=1666172286265530469)
 */
ads_i32 AdsSyncReadDeviceInfoReqEx(ads_i32 port, const AmsAddr *pAddr, char *devName,
				AdsVersion *version);

/**
 * Reads the ADS status and the device status from an ADS server.
 * @param[in] port port number of an Ads port that had previously been opened with AdsPortOpenEx().
 * @param[in] pAddr Structure with NetId and port number of the ADS server.
 * @param[out] adsState Address of a variable that will receive the ADS status (see data type [ADSSTATE](https://infosys.beckhoff.com/content/1031/tcadsdll2/html/tcadsdll_enumadsstate.htm?id=2714257434501002224).
 * @param[out] devState Address of a variable that will receive the device status.
 * @return [ADS Return Code](https://infosys.beckhoff.com/content/1031/tcadscommon/html/ads_returncodes.htm?id=1666172286265530469)
 */
ads_i32 AdsSyncReadStateReqEx(ads_i32 port, const AmsAddr *pAddr, uint16_t *adsState,
			   uint16_t *devState);

/**
 * Writes data synchronously into an ADS server and receives data back from the ADS server.
 * @param[in] port  port number of an Ads port that had previously been opened with AdsPortOpenEx().
 * @param[in] pAddr Structure with NetId and port number of the ADS server.
 * @param[in] indexGroup Index Group.
 * @param[in] indexOffset Index Offset.
 * @param[in] readLength Length, in bytes, of the read buffer readData.
 * @param[out] readData Buffer for data read from the ADS server.
 * @param[in] writeLength Length of the data, in bytes, send to the ADS server.
 * @param[in] writeData Buffer with data send to the ADS server.
 * @param[out] bytesRead pointer to a variable. If successful, this variable will return the number of actually read data bytes.
 * @return [ADS Return Code](https://infosys.beckhoff.com/content/1031/tcadscommon/html/ads_returncodes.htm?id=1666172286265530469)
 */
ads_i32 AdsSyncReadWriteReqEx2(ads_i32 port, const AmsAddr *pAddr,
			    ads_ui32 indexGroup, ads_ui32 indexOffset,
			    ads_ui32 readLength, void *readData,
			    ads_ui32 writeLength, const void *writeData,
			    ads_ui32 *bytesRead);

/**
 * Writes data synchronously to an ADS server.
 * @param[in] port port number of an Ads port that had previously been opened with AdsPortOpenEx().
 * @param[in] pAddr Structure with NetId and port number of the ADS server.
 * @param[in] indexGroup Index Group.
 * @param[in] indexOffset Index Offset.
 * @param[in] bufferLength Length of the data, in bytes, send to the ADS server.
 * @param[in] buffer Buffer with data send to the ADS server.
 * @return [ADS Return Code](https://infosys.beckhoff.com/content/1031/tcadscommon/html/ads_returncodes.htm?id=1666172286265530469)
 */
ads_i32 AdsSyncWriteReqEx(ads_i32 port, const AmsAddr *pAddr, ads_ui32 indexGroup,
		       ads_ui32 indexOffset, ads_ui32 bufferLength,
		       const void *buffer);

/**
 * Changes the ADS status and the device status of an ADS server.
 * @param[in] port port number of an Ads port that had previously been opened with AdsPortOpenEx().
 * @param[in] pAddr Structure with NetId and port number of the ADS server.
 * @param[in] adsState New ADS status.
 * @param[in] devState New device status.
 * @param[in] bufferLength Length of the additional data, in bytes, send to the ADS server.
 * @param[in] buffer Buffer with additional data send to the ADS server.
 * @return [ADS Return Code](https://infosys.beckhoff.com/content/1031/tcadscommon/html/ads_returncodes.htm?id=1666172286265530469)
 */
ads_i32 AdsSyncWriteControlReqEx(ads_i32 port, const AmsAddr *pAddr,
			      uint16_t adsState, uint16_t devState,
			      ads_ui32 bufferLength, const void *buffer);

/**
 * A notification is defined within an ADS server (e.g. PLC). When a
 * certain event occurs a function (the callback function) is invoked in
 * the ADS client (C program).
 * @param[in] port port number of an Ads port that had previously been opened with AdsPortOpenEx().
 * @param[in] pAddr Structure with NetId and port number of the ADS server.
 * @param[in] indexGroup Index Group.
 * @param[in] indexOffset Index Offset.
 * @param[in] pAttrib Pointer to the structure that contains further information.
 * @param[in] pFunc Pointer to the structure describing the callback function.
 * @param[in] hUser 32-bit value that is passed to the callback function.
 * @param[out] pNotification Address of the variable that will receive the handle of the notification.
 * @return [ADS Return Code](https://infosys.beckhoff.com/content/1031/tcadscommon/html/ads_returncodes.htm?id=1666172286265530469)
 */
ads_i32 AdsSyncAddDeviceNotificationReqEx(ads_i32 port, const AmsAddr *pAddr,
				       ads_ui32 indexGroup,
				       ads_ui32 indexOffset,
				       const AdsNotificationAttrib *pAttrib,
				       PAdsNotificationFuncEx pFunc,
				       ads_ui32 hUser, ads_ui32 *pNotification);

/**
 * A notification defined previously is deleted from an ADS server.
 * @param[in] port port number of an Ads port that had previously been opened with AdsPortOpenEx().
 * @param[in] pAddr Structure with NetId and port number of the ADS server.
 * @param[in] hNotification Address of the variable that contains the handle of the notification.
 * @return [ADS Return Code](https://infosys.beckhoff.com/content/1031/tcadscommon/html/ads_returncodes.htm?id=1666172286265530469)
 */
ads_i32 AdsSyncDelDeviceNotificationReqEx(ads_i32 port, const AmsAddr *pAddr,
				       ads_ui32 hNotification);

/**
 * Read the configured timeout for the ADS functions. The standard value is 5000 ms.
 * @param[in] port port number of an Ads port that had previously been opened with AdsPortOpenEx().
 * @param[out] timeout Buffer to store timeout value in ms.
 * @return [ADS Return Code](https://infosys.beckhoff.com/content/1031/tcadscommon/html/ads_returncodes.htm?id=1666172286265530469)
 */
ads_i32 AdsSyncGetTimeoutEx(ads_i32 port, ads_i32 *timeout);
#ifdef BHF_ADS_EXPORT_C
}
#endif

/**
 * Same as above for a callback function as in Beckhoff's standalone AdsLib
 * (const parameters).
 */
ads_i32 AdsSyncAddDeviceNotificationReqEx(ads_i32 port, const AmsAddr *pAddr,
					  ads_ui32 indexGroup,
					  ads_ui32 indexOffset,
					  const AdsNotificationAttrib *pAttrib,
					  PAdsNotificationFuncExConst pFunc,
					  ads_ui32 hUser,
					  ads_ui32 *pNotification);

#if defined(_WIN32) && !defined(POSIX)
/**
 * @name uint32_t outputs as in Beckhoff's standalone AdsLib
 * On Windows ads_ui32 is unsigned long, not uint32_t. These templates take
 * uint32_t pointers as well; for nullptr or NULL the functions above are
 * chosen, since a template cannot deduce from them.
 */
///@{
namespace bhf
{
namespace ads
{
template <class T> using IfUint32 = typename std::enable_if<
	std::is_same<T, uint32_t>::value, ads_i32>::type;
}
}

template <class T>
bhf::ads::IfUint32<T>
AdsSyncReadReqEx2(ads_i32 port, const AmsAddr *pAddr, ads_ui32 indexGroup,
		  ads_ui32 indexOffset, ads_ui32 bufferLength, void *buffer,
		  T *bytesRead)
{
	ads_ui32 n = 0;
	const auto status = AdsSyncReadReqEx2(port, pAddr, indexGroup,
					      indexOffset, bufferLength,
					      buffer, &n);
	if (bytesRead) {
		*bytesRead = n;
	}
	return status;
}

template <class T>
bhf::ads::IfUint32<T>
AdsSyncReadWriteReqEx2(ads_i32 port, const AmsAddr *pAddr,
		       ads_ui32 indexGroup, ads_ui32 indexOffset,
		       ads_ui32 readLength, void *readData,
		       ads_ui32 writeLength, const void *writeData,
		       T *bytesRead)
{
	ads_ui32 n = 0;
	const auto status = AdsSyncReadWriteReqEx2(
		port, pAddr, indexGroup, indexOffset, readLength, readData,
		writeLength, writeData, &n);
	if (bytesRead) {
		*bytesRead = n;
	}
	return status;
}

template <class T, class Callback>
bhf::ads::IfUint32<T> AdsSyncAddDeviceNotificationReqEx(
	ads_i32 port, const AmsAddr *pAddr, ads_ui32 indexGroup,
	ads_ui32 indexOffset, const AdsNotificationAttrib *pAttrib,
	Callback pFunc, ads_ui32 hUser, T *pNotification)
{
	ads_ui32 h = 0;
	const auto status = AdsSyncAddDeviceNotificationReqEx(
		port, pAddr, indexGroup, indexOffset, pAttrib, pFunc, hUser,
		pNotification ? &h : nullptr);
	if (pNotification) {
		*pNotification = h;
	}
	return status;
}

template <class T>
bhf::ads::IfUint32<T> AdsSyncGetTimeoutEx(ads_i32 port, T *timeout)
{
	ads_i32 t = 0;
	const auto status = AdsSyncGetTimeoutEx(port, timeout ? &t : nullptr);
	if (timeout && !status) {
		*timeout = static_cast<uint32_t>(t);
	}
	return status;
}
///@}
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
