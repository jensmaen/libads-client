// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2015 - 2022 Beckhoff Automation GmbH & Co. KG
 */

#pragma once

#include "AdsDef.h"

struct AmsConnection;

struct Router {
	static const size_t NUM_PORTS_MAX = 128;
	static const uint16_t PORT_BASE = 30000;
	static_assert(NUM_PORTS_MAX + PORT_BASE <= UINT16_MAX,
		      "Port limit is out of range");
	virtual ~Router()
	{
	}

	virtual long GetLocalAddress(uint16_t port, AmsAddr *pAddr) = 0;

	/**
	 * Slot of a port in the pending-response queue of an AmsConnection,
	 * -1 if the port is not served by this router.
	 */
	virtual int PortSlot(uint16_t port) const
	{
		if ((port < PORT_BASE) || (port >= PORT_BASE + NUM_PORTS_MAX)) {
			return -1;
		}
		return port - PORT_BASE;
	}

	/**
	 * Called from the receive thread of an AmsConnection for AMS/TCP
	 * frames with a command other than 0 (router commands of the local
	 * TwinCAT router: port registration, router notes).
	 */
	virtual void OnRouterCommand(AmsConnection &from, uint16_t command,
				     const uint8_t *data, size_t length)
	{
		(void)from;
		(void)command;
		(void)data;
		(void)length;
	}

	/**
	 * Called from the receive thread of an AmsConnection when it ends
	 * because the connection was closed or failed. The connection must not
	 * be destroyed from within this call.
	 */
	virtual void OnConnectionLost(AmsConnection &from)
	{
		(void)from;
	}

	/**
	 * Called from the receive thread of a reconnecting AmsConnection
	 * (constructed with a Connector) once it is connected again, before it
	 * receives anything on the new transport.
	 */
	virtual void OnReconnected(AmsConnection &from)
	{
		(void)from;
	}

	/**
	 * Called from the receive thread of a reconnecting AmsConnection when
	 * its Connector gave up for good (AmsConnection::GiveUp). The thread
	 * ends after this call.
	 */
	virtual void OnGaveUp(AmsConnection &from)
	{
		(void)from;
	}
};
