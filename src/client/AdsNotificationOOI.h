// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2020 - 2022 Beckhoff Automation GmbH & Co. KG
 */

#pragma once

#include "AdsDevice.h"

// PAdsNotificationFuncExConst is in StandaloneAdsDef.h now; the legacy
// callback is the one of the TcAdsDll.
typedef PAdsNotificationFuncEx PAdsNotificationFuncExLegacy;

struct AdsNotification {
	AdsNotification(const AdsDevice &route, const std::string &symbolName,
			const AdsNotificationAttrib &notificationAttributes,
			PAdsNotificationFuncExConst callback, uint32_t hUser)
		: m_Symbol(route.GetHandle(symbolName))
		, m_Notification(route.GetHandle(
			  ADSIGRP_SYM_VALBYHND, *m_Symbol,
			  notificationAttributes,
			  callback, hUser))
	{
	}

	AdsNotification(const AdsDevice &route, const std::string &symbolName,
			const AdsNotificationAttrib &notificationAttributes,
			PAdsNotificationFuncExLegacy callback, uint32_t hUser)
		: m_Symbol(route.GetHandle(symbolName))
		, m_Notification(route.GetHandle(
			  ADSIGRP_SYM_VALBYHND, *m_Symbol,
			  notificationAttributes,
			  callback, hUser))
	{
	}

	AdsNotification(const AdsDevice &route, uint32_t indexGroup,
			uint32_t indexOffset,
			const AdsNotificationAttrib &notificationAttributes,
			PAdsNotificationFuncExConst callback, uint32_t hUser)
		: m_Symbol{ route.GetHandle(indexOffset) }
		, m_Notification(route.GetHandle(
			  indexGroup, indexOffset, notificationAttributes,
			  callback, hUser))
	{
	}

	AdsNotification(const AdsDevice &route, uint32_t indexGroup,
			uint32_t indexOffset,
			const AdsNotificationAttrib &notificationAttributes,
			PAdsNotificationFuncExLegacy callback, uint32_t hUser)
		: m_Symbol{ route.GetHandle(indexOffset) }
		, m_Notification(route.GetHandle(
			  indexGroup, indexOffset, notificationAttributes,
			  callback, hUser))
	{
	}

    private:
	AdsHandle m_Symbol;
	AdsHandle m_Notification;
};
