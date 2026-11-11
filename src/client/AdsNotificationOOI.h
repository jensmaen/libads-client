// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2020 - 2022 Beckhoff Automation GmbH & Co. KG
 */

#pragma once

#include "AdsDevice.h"

// PAdsNotificationFuncExConst is in StandaloneAdsDef.h now; the legacy
// callback is the one of the TcAdsDll.
typedef PAdsNotificationFuncEx PAdsNotificationFuncExLegacy;

/**
 * A notification that is deleted on the target with the object. The device
 * must outlive it.
 */
struct AdsNotification {
	/** On a symbol by its name, e.g. "MAIN.nCounter" or a property path */
	AdsNotification(const AdsDevice &route, const std::string &symbolName,
			const AdsNotificationAttrib &notificationAttributes,
			PAdsNotificationFuncExConst callback, uint32_t hUser)
		: m_Notification(ByName(route, symbolName,
					notificationAttributes, callback,
					hUser))
	{
	}

	AdsNotification(const AdsDevice &route, const std::string &symbolName,
			const AdsNotificationAttrib &notificationAttributes,
			PAdsNotificationFuncExLegacy callback, uint32_t hUser)
		: m_Notification(ByName(route, symbolName,
					notificationAttributes, callback,
					hUser))
	{
	}

	/** On an address, e.g. of an AdsElement */
	AdsNotification(const AdsDevice &route, uint32_t indexGroup,
			uint32_t indexOffset,
			const AdsNotificationAttrib &notificationAttributes,
			PAdsNotificationFuncExConst callback, uint32_t hUser)
		: m_Notification(route.GetHandle(indexGroup, indexOffset,
						 notificationAttributes,
						 callback, hUser))
	{
	}

	AdsNotification(const AdsDevice &route, uint32_t indexGroup,
			uint32_t indexOffset,
			const AdsNotificationAttrib &notificationAttributes,
			PAdsNotificationFuncExLegacy callback, uint32_t hUser)
		: m_Notification(route.GetHandle(indexGroup, indexOffset,
						 notificationAttributes,
						 callback, hUser))
	{
	}

    private:
	/**
	 * The symbol handle is needed only to create the notification, which
	 * keeps working without it: it is released at once, not with the
	 * notification (one request less when the target is gone).
	 */
	template <class Callback>
	static AdsHandle ByName(const AdsDevice &route,
				const std::string &symbolName,
				const AdsNotificationAttrib &attributes,
				Callback callback, uint32_t hUser)
	{
		const AdsHandle symbol = route.GetHandle(symbolName);
		return route.GetHandle(ADSIGRP_SYM_VALBYHND, *symbol,
				       attributes, callback, hUser);
	}

	AdsHandle m_Notification;
};
