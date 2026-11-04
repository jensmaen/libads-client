// SPDX-License-Identifier: MIT
/** @file
   Copyright (c) 2015 - 2022 Beckhoff Automation GmbH & Co. KG
 */

#pragma once

#include "AdsDef.h"

#ifdef BHF_ADS_EXPORT_C
extern "C" {
#endif

/**
 * The connection (communication port) to the message router is
 * closed. The port to be closed must previously have been opened via
 * an AdsPortOpenEx() call.
 * @param[in] port port number of an Ads port that had previously been opened with AdsPortOpenEx().
 * @return [ADS Return Code](https://infosys.beckhoff.com/content/1031/tcadscommon/html/ads_returncodes.htm?id=1666172286265530469)
 */
ads_i32 AdsPortCloseEx(ads_i32 port);

/**
 * Establishes a connection (communication port) to the local TwinCAT
 * router (TCP 127.0.0.1:48898), like the TcAdsDll. The router assigns the
 * port number and the local AmsNetId. The port number returned by
 * AdsPortOpenEx() is required as parameter for further AdsLib function calls.
 * Unlike Beckhoff's standalone AdsLib, this does not use ADS routes; see
 * AdsPortOpenRouteEx().
 * @return port number of a new Ads port or 0 if the local router is not reachable
 */
ads_i32 AdsPortOpenEx();

/**
 * Establishes a communication port that reaches its targets directly over
 * ADS routes added with bhf::ads::AddLocalRoute() (Beckhoff's standalone
 * AdsLib behaviour of AdsPortOpenEx()).
 * @return port number of a new Ads port or 0 if no more ports available
 */
ads_i32 AdsPortOpenRouteEx();

/**
 * Returns the local NetId and port number.
 * @param[in] port port number of an Ads port that had previously been opened with AdsPortOpenEx().
 * @param[out] pAddr Pointer to the structure of type AmsAddr.
 * @return [ADS Return Code](https://infosys.beckhoff.com/content/1031/tcadscommon/html/ads_returncodes.htm?id=1666172286265530469)
 */
ads_i32 AdsGetLocalAddressEx(ads_i32 port, AmsAddr *pAddr);

/**
 * Alters the timeout for the ADS functions. The standard value is 5000 ms.
 * @param[in] port port number of an Ads port that had previously been opened with AdsPortOpenEx().
 * @param[in] timeout Timeout in ms.
 * @return [ADS Return Code](https://infosys.beckhoff.com/content/1031/tcadscommon/html/ads_returncodes.htm?id=1666172286265530469)
 */
ads_i32 AdsSyncSetTimeoutEx(ads_i32 port, ads_i32 timeout);

/**
 * @name Functions of the TcAdsDll
 * The functions without port parameter work on one default port per process,
 * opened with AdsPortOpen() at the local router, as with the TcAdsDll. They
 * return ADSERR_CLIENT_PORTNOTOPEN while it is not open.
 */
///@{

/** Version of this library, packed like an AdsVersion. */
ads_i32 AdsGetDllVersion(void);

/** Error code of the last call of this interface in the calling thread. */
ads_i32 AdsGetLastError(void);

/**
 * Opens the default port at the local router, or returns it if it is
 * already open.
 * @return port number, 0 if the local router is not reachable
 */
ads_i32 AdsPortOpen(void);

/** Closes the default port. */
ads_i32 AdsPortClose(void);

ads_i32 AdsGetLocalAddress(AmsAddr *pAddr);
ads_i32 AdsSyncReadReq(const AmsAddr *pAddr, ads_ui32 indexGroup,
		       ads_ui32 indexOffset, ads_ui32 length, void *pData);
ads_i32 AdsSyncReadReqEx(const AmsAddr *pAddr, ads_ui32 indexGroup,
			 ads_ui32 indexOffset, ads_ui32 length, void *pData,
			 ads_ui32 *pcbReturn);
ads_i32 AdsSyncWriteReq(const AmsAddr *pAddr, ads_ui32 indexGroup,
			ads_ui32 indexOffset, ads_ui32 length,
			const void *pData);
ads_i32 AdsSyncReadWriteReq(const AmsAddr *pAddr, ads_ui32 indexGroup,
			    ads_ui32 indexOffset, ads_ui32 cbReadLength,
			    void *pReadData, ads_ui32 cbWriteLength,
			    const void *pWriteData);
ads_i32 AdsSyncReadWriteReqEx(const AmsAddr *pAddr, ads_ui32 indexGroup,
			      ads_ui32 indexOffset, ads_ui32 cbReadLength,
			      void *pReadData, ads_ui32 cbWriteLength,
			      const void *pWriteData, ads_ui32 *pcbReturn);
ads_i32 AdsSyncReadDeviceInfoReq(const AmsAddr *pAddr, char *pDevName,
				 AdsVersion *pVersion);
ads_i32 AdsSyncWriteControlReq(const AmsAddr *pAddr, ads_ui16 adsState,
			       ads_ui16 deviceState, ads_ui32 length,
			       const void *pData);
ads_i32 AdsSyncReadStateReq(const AmsAddr *pAddr, ads_ui16 *pAdsState,
			    ads_ui16 *pDeviceState);
ads_i32 AdsSyncAddDeviceNotificationReq(const AmsAddr *pAddr,
					ads_ui32 indexGroup,
					ads_ui32 indexOffset,
					const AdsNotificationAttrib *pNoteAttrib,
					PAdsNotificationFuncEx pNoteFunc,
					ads_ui32 hUser, ads_ui32 *pNotification);
ads_i32 AdsSyncDelDeviceNotificationReq(const AmsAddr *pAddr,
					ads_ui32 hNotification);
ads_i32 AdsSyncSetTimeout(ads_i32 nMs);
ads_i32 AdsSyncGetTimeout(ads_i32 *pnMs);

/**
 * Tells whether the port can reach the router: open and, for a port at the
 * local router, still connected.
 */
ads_i32 AdsAmsPortEnabled(ads_bool *pbEnabled);
ads_i32 AdsAmsPortEnabledEx(ads_i32 nPort, ads_bool *pbEnabled);

/**
 * Calls pNoteFunc with an AmsRouterEvent when the local router stops, starts
 * or goes away. The call comes from a thread of the library, the functions of
 * this interface may be used in it. Only for ports at the local router.
 */
ads_i32 AdsAmsRegisterRouterNotification(PAmsRouterNotificationFuncEx pNoteFunc);
ads_i32 AdsAmsUnRegisterRouterNotification(void);
ads_i32 AdsAmsRegisterRouterNotificationEx2(ads_i32 nPort,
					    PAmsRouterNotificationFuncEx3 pNoteFunc,
					    void *hUser);
ads_i32 AdsAmsUnRegisterRouterNotificationEx2(ads_i32 nPort);
///@}

#ifdef BHF_ADS_EXPORT_C
}
#endif
