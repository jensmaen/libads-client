// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2026 Jens Fresenborg

   Settings for Secure ADS (TLS on TCP 8016).
 */

#pragma once

#include <string>

namespace bhf
{
namespace ads
{
struct TlsSettings {
	/**
	 * Folder with the own certificate (one per computer, like the TwinCAT
	 * router's) and the certificates of the targets. Empty: %ProgramData%\
	 * libads-client on Windows, /etc/libads-client elsewhere.
	 */
	std::string directory;
	/**
	 * Name in the own certificate, used only when it is created; the target
	 * sees it as hostname and route name. Letters, digits, '-', '_' and '.',
	 * at most 31 characters. Empty: the computer name.
	 */
	std::string name;
	/**
	 * Pre-shared key instead of certificates (PSK): identity and password of
	 * an entry <Server><Tls><Psk> in the target's StaticRoutes.xml; the key is
	 * SHA-256(UPPER(identity) + password). Empty identity: SSC. With PSK no
	 * route has to be added; the target must not have an SSC route for our IP
	 * (it picks the first route of an IP).
	 */
	std::string pskIdentity;
	std::string pskPassword;
	/**
	 * Certificates of an own CA instead (SCA), PEM files: caFile checks the
	 * target's certificate, certFile/keyFile are ours, signed by that CA
	 * (its CN is sent as our name). The target needs <Server><Tls><Ca>
	 * with the same CA. No route has to be added. Empty caFile: SSC or PSK.
	 */
	std::string caFile;
	std::string certFile;
	std::string keyFile;
	/** SCA only: name (or IP) the target's certificate must carry; empty: any certificate of the CA */
	std::string serverName;
};
}
}
