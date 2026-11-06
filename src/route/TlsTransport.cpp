// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2026 Jens Fresenborg
 */

#include "TlsTransport.h"
#include "Log.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h> // only for the MBEDTLS_ERR_NET_* codes
#include <mbedtls/oid.h>
#include <mbedtls/pem.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace
{
// TlsConnectInfo flags and size (64-byte message after the TLS handshake)
const uint16_t FLAG_RESPONSE = 0x01;
const uint16_t FLAG_AMS_ALLOWED = 0x02;
const uint16_t FLAG_SELF_SIGNED = 0x10;
const uint16_t FLAG_IP_ADDR = 0x20;
const uint16_t FLAG_ADD_REMOTE = 0x80;
const size_t CONNECT_INFO_SIZE = 64;
const size_t CONNECT_INFO_MAX = 512;
const size_t NAME_MAX_LENGTH = 31;
// how long one TLS record may take to arrive once it has begun
const uint32_t READ_TIMEOUT_MS = 5000;
// the PSK suites TwinCAT accepts (no GCM, no DHE/ECDHE-PSK)
const int PSK_CIPHERSUITES[] = { MBEDTLS_TLS_PSK_WITH_AES_256_CBC_SHA384,
				 MBEDTLS_TLS_PSK_WITH_AES_128_CBC_SHA256,
				 MBEDTLS_TLS_PSK_WITH_AES_256_CBC_SHA,
				 MBEDTLS_TLS_PSK_WITH_AES_128_CBC_SHA, 0 };

std::string MbedError(int ret)
{
	char text[160];
	mbedtls_strerror(ret, text, sizeof(text));
	std::ostringstream s;
	s << "-0x" << std::hex << -ret << ' ' << text;
	return s.str();
}

/** One random generator for all connections; without MBEDTLS_THREADING_C it needs its own lock. */
struct Rng {
	std::mutex mutex;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;

	Rng()
	{
		const auto status = psa_crypto_init();
		if (status != PSA_SUCCESS) {
			throw std::runtime_error("psa_crypto_init() failed");
		}
		mbedtls_entropy_init(&entropy);
		mbedtls_ctr_drbg_init(&drbg);
		const int ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func,
						      &entropy, nullptr, 0);
		if (ret) {
			throw std::runtime_error("random generator: " +
						 MbedError(ret));
		}
	}
	~Rng()
	{
		mbedtls_ctr_drbg_free(&drbg);
		mbedtls_entropy_free(&entropy);
	}
	static int Random(void *p, unsigned char *out, size_t length)
	{
		auto *const rng = static_cast<Rng *>(p);
		std::lock_guard<std::mutex> lock(rng->mutex);
		return mbedtls_ctr_drbg_random(&rng->drbg, out, length);
	}
};

/**
 * Never destroyed on purpose: the global router is created before the first
 * TLS connection and therefore destroyed after a function-local static. Its
 * connections still send close_notify then, which needs the generator.
 */
Rng &GetRng()
{
	static Rng *const rng = new Rng;
	return *rng;
}

std::string DefaultDirectory()
{
#if defined(_WIN32)
	const char *const programData = std::getenv("ProgramData");
	return std::string(programData ? programData : "C:\\ProgramData") +
	       "\\libads-client";
#else
	return "/etc/libads-client";
#endif
}

std::string Join(const std::string &directory, const std::string &file)
{
#if defined(_WIN32)
	return directory + '\\' + file;
#else
	return directory + '/' + file;
#endif
}

void MakeDirectory(const std::string &directory)
{
#if defined(_WIN32)
	const int ret = _mkdir(directory.c_str());
#else
	const int ret = mkdir(directory.c_str(), 0755);
#endif
	if (ret && errno != EEXIST) {
		throw TlsError(ADSERR_DEVICE_INVALIDACCESS,
			       "cannot create folder " + directory + ": " +
				       std::strerror(errno));
	}
}

bool FileExists(const std::string &path)
{
	FILE *const f = std::fopen(path.c_str(), "rb");
	if (f) {
		std::fclose(f);
	}
	return f != nullptr;
}

void WriteFile(const std::string &path, const unsigned char *data,
	       size_t length, bool secret)
{
	FILE *const f = std::fopen(path.c_str(), "wb");
	if (!f) {
		throw TlsError(ADSERR_DEVICE_INVALIDACCESS,
			       "cannot write " + path + ": " +
				       std::strerror(errno));
	}
	const bool ok = std::fwrite(data, 1, length, f) == length;
	if (std::fclose(f) || !ok) {
		throw TlsError(ADSERR_DEVICE_INVALIDACCESS,
			       "cannot write " + path);
	}
#if !defined(_WIN32)
	// readable for the group of users that may use the library
	chmod(path.c_str(), secret ? 0640 : 0644);
#else
	(void)secret;
#endif
}

bool ValidName(const std::string &name)
{
	if (name.empty() || name.size() > NAME_MAX_LENGTH) {
		return false;
	}
	return std::all_of(name.begin(), name.end(), [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		       (c >= '0' && c <= '9') || c == '-' || c == '_' ||
		       c == '.';
	});
}

/** Computer name, made fit for a certificate name (sockets are initialized by then). */
std::string ComputerName()
{
	char buffer[256] = {};
	if (gethostname(buffer, sizeof(buffer) - 1)) {
		return "libads-client";
	}
	std::string name = buffer;
	for (auto &c : name) {
		if (!ValidName(std::string(1, c))) {
			c = '-';
		}
	}
	if (name.size() > NAME_MAX_LENGTH) {
		name.resize(NAME_MAX_LENGTH);
	}
	return name.empty() ? "libads-client" : name;
}

/** CN of a certificate */
std::string CommonName(const mbedtls_x509_crt &crt)
{
	for (auto *n = &crt.subject; n; n = n->next) {
		if (!MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &n->oid)) {
			return std::string(reinterpret_cast<const char *>(n->val.p),
					   n->val.len);
		}
	}
	return {};
}
}

struct TlsTransport::Impl {
	/** TcpSocket with access to the native socket for mbedTLS */
	struct Tcp : TcpSocket {
		Tcp(const struct addrinfo *host, const std::atomic<bool> *abort)
			: TcpSocket(host, abort)
		{
		}
		SOCKET Fd() const
		{
			return m_Socket;
		}
		bool Wait(timeval *timeout) const
		{
			return Select(timeout);
		}
	};

	Tcp tcp;
	std::string directory;
	std::string ownName;
	mbedtls_pk_context key;
	mbedtls_x509_crt own;
	mbedtls_x509_crt ca; // SCA
	mbedtls_ssl_config conf;
	mbedtls_ssl_context ssl;
	// every mbedtls_ssl_* call on one connection: receive thread and writers
	std::mutex mutex;
	bool established = false;
	bool closed = false;
	bool psk = false;
	bool sca = false;
	std::string serverName;
	std::string host;
	AmsNetId targetNetId;

	Impl(const struct addrinfo *destination, const std::atomic<bool> *abort)
		: tcp(destination, abort)
	{
		mbedtls_pk_init(&key);
		mbedtls_x509_crt_init(&own);
		mbedtls_x509_crt_init(&ca);
		mbedtls_ssl_config_init(&conf);
		mbedtls_ssl_init(&ssl);
	}

	~Impl()
	{
		mbedtls_ssl_free(&ssl);
		mbedtls_ssl_config_free(&conf);
		mbedtls_x509_crt_free(&ca);
		mbedtls_x509_crt_free(&own);
		mbedtls_pk_free(&key);
	}

	static int Send(void *ctx, const unsigned char *buffer, size_t length)
	{
		auto *const self = static_cast<Impl *>(ctx);
		int flags = 0;
#ifdef MSG_NOSIGNAL
		flags = MSG_NOSIGNAL;
#endif
		const int n = ::send(self->tcp.Fd(),
				     reinterpret_cast<const char *>(buffer),
				     static_cast<int>(std::min<size_t>(length, INT_MAX)),
				     flags);
		return n >= 0 ? n : MBEDTLS_ERR_NET_SEND_FAILED;
	}

	static int Recv(void *ctx, unsigned char *buffer, size_t length,
			uint32_t timeoutMs)
	{
		auto *const self = static_cast<Impl *>(ctx);
		if (timeoutMs) {
			timeval timeout{ static_cast<long>(timeoutMs / 1000),
					 static_cast<int>((timeoutMs % 1000) * 1000) };
			try {
				if (!self->tcp.Wait(&timeout)) {
					return MBEDTLS_ERR_NET_RECV_FAILED;
				}
			} catch (const Socket::TimeoutEx &) {
				return MBEDTLS_ERR_SSL_TIMEOUT;
			} catch (const std::exception &) {
				return MBEDTLS_ERR_NET_RECV_FAILED;
			}
		}
		const int n = ::recv(self->tcp.Fd(), reinterpret_cast<char *>(buffer),
				     static_cast<int>(std::min<size_t>(length, INT_MAX)),
				     0);
		return n >= 0 ? n : MBEDTLS_ERR_NET_RECV_FAILED;
	}

	/** Loads the own key and certificate, creates them on first use. */
	void LoadOwnCertificate(const bhf::ads::TlsSettings &settings)
	{
		directory = settings.directory.empty() ? DefaultDirectory() :
							 settings.directory;
		const auto keyPath = Join(directory, "own-key.pem");
		const auto certPath = Join(directory, "own-cert.pem");
		auto &rng = GetRng();

		if (FileExists(keyPath) || FileExists(certPath)) {
			int ret = mbedtls_pk_parse_keyfile(&key, keyPath.c_str(),
							   nullptr, Rng::Random,
							   &rng);
			if (ret) {
				throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
					       "cannot read " + keyPath + ": " +
						       MbedError(ret));
			}
			ret = mbedtls_x509_crt_parse_file(&own, certPath.c_str());
			if (ret) {
				throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
					       "cannot read " + certPath + ": " +
						       MbedError(ret));
			}
			ownName = CommonName(own);
			return;
		}

		ownName = settings.name.empty() ? ComputerName() : settings.name;
		if (!ValidName(ownName)) {
			throw TlsError(ADSERR_CLIENT_INVALIDPARM,
				       "invalid certificate name '" + ownName + "'");
		}
		MakeDirectory(directory);
		CreateCertificate(rng);

		std::vector<unsigned char> pem(16000);
		int ret = mbedtls_pk_write_key_pem(&key, pem.data(), pem.size());
		if (ret) {
			throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
				       "cannot encode the key: " + MbedError(ret));
		}
		WriteFile(keyPath, pem.data(),
			  std::strlen(reinterpret_cast<const char *>(pem.data())),
			  true);
		mbedtls_platform_zeroize(pem.data(), pem.size());

		size_t length = 0;
		ret = mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n",
					       "-----END CERTIFICATE-----\n",
					       own.raw.p, own.raw.len, pem.data(),
					       pem.size(), &length);
		if (ret) {
			throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
				       "cannot encode the certificate: " +
					       MbedError(ret));
		}
		WriteFile(certPath, pem.data(), length - 1, false);
		LOG_INFO("Secure ADS: created certificate CN=" << ownName << " in "
							      << directory);
	}

	/** Self-signed like the TwinCAT router's: RSA 2048, CA, valid 2000-2060. */
	void CreateCertificate(Rng &rng)
	{
		int ret = mbedtls_pk_setup(&key,
					   mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
		if (!ret) {
			ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), Rng::Random,
						  &rng, 2048, 65537);
		}
		if (ret) {
			throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
				       "cannot create a key: " + MbedError(ret));
		}

		mbedtls_x509write_cert writer;
		mbedtls_x509write_crt_init(&writer);
		const std::string subject = "CN=" + ownName + ",O=libads-client";
		unsigned char serial[16];
		Rng::Random(&rng, serial, sizeof(serial));
		serial[0] &= 0x7f; // positive
		serial[0] |= 0x01; // no leading zero byte
		mbedtls_x509_san_list san{};
		san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
		san.node.san.unstructured_name.p = reinterpret_cast<unsigned char *>(
			const_cast<char *>(ownName.data()));
		san.node.san.unstructured_name.len = ownName.size();

		mbedtls_x509write_crt_set_version(&writer,
						  MBEDTLS_X509_CRT_VERSION_3);
		mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
		mbedtls_x509write_crt_set_subject_key(&writer, &key);
		mbedtls_x509write_crt_set_issuer_key(&writer, &key);
		std::vector<unsigned char> der(4096);
		int length = 0;
		if ((ret = mbedtls_x509write_crt_set_subject_name(&writer,
								  subject.c_str())) ||
		    (ret = mbedtls_x509write_crt_set_issuer_name(&writer,
								 subject.c_str())) ||
		    (ret = mbedtls_x509write_crt_set_serial_raw(&writer, serial,
								sizeof(serial))) ||
		    (ret = mbedtls_x509write_crt_set_validity(&writer,
							      "20000101000000",
							      "20601231235959")) ||
		    (ret = mbedtls_x509write_crt_set_subject_alternative_name(
			     &writer, &san)) ||
		    (ret = mbedtls_x509write_crt_set_basic_constraints(&writer, 1,
								       -1)) ||
		    (ret = mbedtls_x509write_crt_set_subject_key_identifier(
			     &writer)) ||
		    (ret = mbedtls_x509write_crt_set_authority_key_identifier(
			     &writer))) {
			mbedtls_x509write_crt_free(&writer);
			throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
				       "certificate fields: " + MbedError(ret));
		}
		length = mbedtls_x509write_crt_der(&writer, der.data(), der.size(),
						   Rng::Random, &rng);
		mbedtls_x509write_crt_free(&writer);
		if (length < 0) {
			throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
				       "cannot create the certificate: " +
					       MbedError(length));
		}
		// mbedtls writes DER at the end of the buffer
		ret = mbedtls_x509_crt_parse_der(&own, der.data() + der.size() - length,
						 static_cast<size_t>(length));
		if (ret) {
			throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
				       "cannot read the new certificate: " +
					       MbedError(ret));
		}
	}

	/** PSK: key = SHA-256(UPPER(identity) + password), as TwinCAT derives it */
	void ConfigurePsk(const bhf::ads::TlsSettings &settings)
	{
		std::string input = settings.pskIdentity;
		for (auto &c : input) {
			if (c >= 'a' && c <= 'z') {
				c = static_cast<char>(c - 'a' + 'A');
			}
		}
		input += settings.pskPassword;
		unsigned char key[32];
		int ret = mbedtls_sha256(
			reinterpret_cast<const unsigned char *>(input.data()),
			input.size(), key, 0);
		mbedtls_platform_zeroize(&input[0], input.size());
		if (!ret) {
			ret = mbedtls_ssl_conf_psk(
				&conf, key, sizeof(key),
				reinterpret_cast<const unsigned char *>(
					settings.pskIdentity.data()),
				settings.pskIdentity.size());
		}
		mbedtls_platform_zeroize(key, sizeof(key));
		if (ret) {
			throw TlsError(ADSERR_CLIENT_INVALIDPARM,
				       "PSK: " + MbedError(ret));
		}
		mbedtls_ssl_conf_ciphersuites(&conf, PSK_CIPHERSUITES);
	}

	/** SCA: CA, own certificate and key from the given files */
	void ConfigureCa(const bhf::ads::TlsSettings &settings)
	{
		int ret = mbedtls_x509_crt_parse_file(&ca, settings.caFile.c_str());
		if (ret) {
			throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
				       "cannot read " + settings.caFile + ": " +
					       MbedError(ret));
		}
		ret = mbedtls_x509_crt_parse_file(&own, settings.certFile.c_str());
		if (ret) {
			throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
				       "cannot read " + settings.certFile + ": " +
					       MbedError(ret));
		}
		ret = mbedtls_pk_parse_keyfile(&key, settings.keyFile.c_str(),
					       nullptr, Rng::Random, &GetRng());
		if (ret) {
			throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
				       "cannot read " + settings.keyFile + ": " +
					       MbedError(ret));
		}
		ownName = CommonName(own);
		serverName = settings.serverName;
		mbedtls_ssl_conf_ca_chain(&conf, &ca, nullptr);
	}

	void Configure(const bhf::ads::TlsSettings &settings)
	{
		psk = !settings.pskIdentity.empty();
		sca = !settings.caFile.empty();
		if (psk && sca) {
			throw TlsError(ADSERR_CLIENT_INVALIDPARM,
				       "TlsSettings: either PSK or CA, not both");
		}
		const int ret = mbedtls_ssl_config_defaults(
			&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
			MBEDTLS_SSL_PRESET_DEFAULT);
		if (ret) {
			throw TlsError(GLOBALERR_TARGET_PORT,
				       "TLS configuration: " + MbedError(ret));
		}
		if (psk) {
			ConfigurePsk(settings);
			ownName = settings.name.empty() ? ComputerName() :
							  settings.name;
			if (!ValidName(ownName)) {
				throw TlsError(ADSERR_CLIENT_INVALIDPARM,
					       "invalid name '" + ownName + "'");
			}
		} else {
			if (sca) {
				ConfigureCa(settings);
			} else {
				LoadOwnCertificate(settings);
			}
			const int r = mbedtls_ssl_conf_own_cert(&conf, &own, &key);
			if (r) {
				throw TlsError(GLOBALERR_TARGET_PORT,
					       "TLS configuration: " + MbedError(r));
			}
		}
	}

	void Handshake()
	{
		auto &rng = GetRng();
		int ret = 0;
		// SSC: the target's certificate is self-signed; it is compared with
		// the stored one instead (CheckPeer()). SCA: signed by the CA.
		mbedtls_ssl_conf_authmode(&conf, sca ? MBEDTLS_SSL_VERIFY_REQUIRED :
						       MBEDTLS_SSL_VERIFY_NONE);
		mbedtls_ssl_conf_rng(&conf, Rng::Random, &rng);
		mbedtls_ssl_conf_read_timeout(&conf, READ_TIMEOUT_MS);
		// TwinCAT speaks TLS 1.2 only (DHE-RSA-AES256-SHA256 for SSC)
		mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
		mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
		if ((ret = mbedtls_ssl_setup(&ssl, &conf))) {
			throw TlsError(GLOBALERR_TARGET_PORT,
				       "TLS configuration: " + MbedError(ret));
		}
		mbedtls_ssl_set_bio(&ssl, this, Send, nullptr, Recv);
		if (sca) {
			// nullptr: explicitly no name check, only the CA
			ret = mbedtls_ssl_set_hostname(
				&ssl, serverName.empty() ? nullptr : serverName.c_str());
			if (ret) {
				throw TlsError(ADSERR_CLIENT_INVALIDPARM,
					       "server name: " + MbedError(ret));
			}
		}

		do {
			ret = mbedtls_ssl_handshake(&ssl);
		} while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
			 ret == MBEDTLS_ERR_SSL_WANT_WRITE);
		if (ret == MBEDTLS_ERR_SSL_TIMEOUT) {
			throw Socket::TimeoutEx("TLS handshake timed out");
		}
		if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
			char info[512];
			mbedtls_x509_crt_verify_info(info, sizeof(info), "",
						     mbedtls_ssl_get_verify_result(&ssl));
			throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
				       "certificate of target " + host +
					       " rejected: " + info);
		}
		if (ret) {
			throw TlsError(GLOBALERR_TARGET_PORT,
				       "TLS handshake failed: " + MbedError(ret) +
					       (psk ? " (wrong PSK identity or password?)" :
						      ""));
		}
		established = true;
	}

	/** One file per target address (what the user gives, e.g. an IP); the
	 *  AmsNetId in the TlsConnectInfo answer can be 0.0.0.0.0.0. */
	std::string PeerPath() const
	{
		std::string file = host;
		for (auto &c : file) {
			if (!ValidName(std::string(1, c))) {
				c = '_';
			}
		}
		return Join(directory, "target-" + file + ".pem");
	}

	const mbedtls_x509_crt &Peer()
	{
		const auto *const peer = mbedtls_ssl_get_peer_cert(&ssl);
		if (!peer) {
			throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
				       "target sent no certificate");
		}
		return *peer;
	}

	/** The target has to show the certificate stored when the route was added. */
	void CheckPeer()
	{
		const auto &peer = Peer();
		const auto path = PeerPath();
		mbedtls_x509_crt stored;
		mbedtls_x509_crt_init(&stored);
		const int ret = mbedtls_x509_crt_parse_file(&stored, path.c_str());
		const bool same = !ret && stored.raw.len == peer.raw.len &&
				  !std::memcmp(stored.raw.p, peer.raw.p, peer.raw.len);
		mbedtls_x509_crt_free(&stored);
		if (ret) {
			throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
				       "no certificate of target " + host +
					       " in " + path +
					       ", add the route with AddRemoteRouteSecure() first");
		}
		if (!same) {
			throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
				       "certificate of target " + host +
					       " differs from " + path);
		}
	}

	void SavePeer()
	{
		const auto &peer = Peer();
		std::vector<unsigned char> pem(4 * peer.raw.len + 128);
		size_t length = 0;
		const int ret = mbedtls_pem_write_buffer(
			"-----BEGIN CERTIFICATE-----\n", "-----END CERTIFICATE-----\n",
			peer.raw.p, peer.raw.len, pem.data(), pem.size(), &length);
		if (ret) {
			throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
				       "cannot encode the target's certificate: " +
					       MbedError(ret));
		}
		WriteFile(PeerPath(), pem.data(), length - 1, false);
	}

	void WriteAll(const uint8_t *data, size_t length)
	{
		while (length) {
			const int ret = mbedtls_ssl_write(&ssl, data, length);
			if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
			    ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
				continue;
			}
			if (ret <= 0) {
				throw TlsError(GLOBALERR_TARGET_PORT,
					       "TLS write failed: " + MbedError(ret));
			}
			data += ret;
			length -= static_cast<size_t>(ret);
		}
	}

	void ReadAll(uint8_t *data, size_t length)
	{
		while (length) {
			const int ret = mbedtls_ssl_read(&ssl, data, length);
			if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
			    ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
				continue;
			}
			if (ret == MBEDTLS_ERR_SSL_TIMEOUT) {
				throw Socket::TimeoutEx("no TlsConnectInfo answer");
			}
			if (ret <= 0) {
				throw TlsError(GLOBALERR_TARGET_PORT,
					       "target closed the connection during "
					       "TlsConnectInfo: " +
						       MbedError(ret));
			}
			data += ret;
			length -= static_cast<size_t>(ret);
		}
	}

	/** Sends TlsConnectInfo and checks the answer; returns the target's AmsNetId. */
	AmsNetId Exchange(uint16_t flags, const AmsNetId &ownNetId,
			  const std::string &user, const std::string &password)
	{
		if (user.size() > 255 || password.size() > 255) {
			throw TlsError(ADSERR_CLIENT_INVALIDPARM,
				       "user or password longer than 255 bytes");
		}
		std::vector<uint8_t> request(CONNECT_INFO_SIZE + user.size() +
					     password.size());
		const auto total = static_cast<uint16_t>(request.size());
		request[0] = total & 0xff;
		request[1] = total >> 8;
		request[2] = flags & 0xff;
		request[3] = flags >> 8;
		request[4] = 1; // version
		std::memcpy(&request[6], ownNetId.b, sizeof(ownNetId.b));
		request[12] = static_cast<uint8_t>(user.size());
		request[13] = static_cast<uint8_t>(password.size());
		std::memcpy(&request[32], ownName.data(),
			    std::min(ownName.size(), NAME_MAX_LENGTH));
		std::memcpy(&request[CONNECT_INFO_SIZE], user.data(), user.size());
		std::memcpy(&request[CONNECT_INFO_SIZE + user.size()],
			    password.data(), password.size());
		try {
			WriteAll(request.data(), request.size());
		} catch (...) {
			mbedtls_platform_zeroize(request.data(), request.size());
			throw;
		}
		mbedtls_platform_zeroize(request.data(), request.size());

		uint8_t answer[CONNECT_INFO_MAX];
		ReadAll(answer, CONNECT_INFO_SIZE);
		const size_t length = answer[0] | (answer[1] << 8);
		if (length < CONNECT_INFO_SIZE || length > CONNECT_INFO_MAX) {
			throw TlsError(ADSERR_DEVICE_INVALIDDATA,
				       "TlsConnectInfo answer with length " +
					       std::to_string(length));
		}
		ReadAll(answer + CONNECT_INFO_SIZE, length - CONNECT_INFO_SIZE);

		const uint16_t answerFlags = answer[2] | (answer[3] << 8);
		const uint8_t error = answer[5];
		AmsNetId target;
		std::memcpy(target.b, &answer[6], sizeof(target.b));
		const std::string targetName(
			reinterpret_cast<const char *>(&answer[32]),
			strnlen(reinterpret_cast<const char *>(&answer[32]), 32));
		LOG_VERBOSE("TlsConnectInfo answer: flags 0x"
			    << std::hex << answerFlags << ", error " << std::dec
			    << int(error) << ", " << target << ' ' << targetName);

		if (!(answerFlags & FLAG_RESPONSE)) {
			throw TlsError(ADSERR_DEVICE_INVALIDDATA,
				       "TlsConnectInfo answer without RESPONSE flag");
		}
		switch (error) {
		case 0:
			break;
		case 1:
			throw TlsError(ADSERR_DEVICE_INVALIDDATA,
				       "target does not support TlsConnectInfo version 1");
		case 2:
			throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
				       "target: name in the certificate does not match '" +
					       ownName + "'");
		case 3:
			throw TlsError(ADSERR_DEVICE_CERTIFICATEINVALID,
				       "target " + targetName +
					       " does not know our certificate (no route?)");
		case 4:
			throw TlsError(ADSERR_DEVICE_INVALIDACCESS,
				       "target " + targetName +
					       ": unknown user or wrong password");
		default:
			throw TlsError(ADSERR_DEVICE_INVALIDDATA,
				       "TlsConnectInfo error " +
					       std::to_string(int(error)));
		}
		if (!(flags & FLAG_ADD_REMOTE) && !(answerFlags & FLAG_AMS_ALLOWED)) {
			throw TlsError(ADSERR_DEVICE_ACCESSDENIED,
				       "target " + targetName + " does not allow AMS");
		}
		return target;
	}

	size_t ReadLocked(uint8_t *buffer, size_t maxBytes)
	{
		const int ret = mbedtls_ssl_read(&ssl, buffer,
						 std::min<size_t>(maxBytes, INT_MAX));
		if (ret > 0) {
			return static_cast<size_t>(ret);
		}
		if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
		    ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
		    ret == MBEDTLS_ERR_SSL_TIMEOUT) {
			return 0;
		}
		closed = true;
		if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
		    ret == MBEDTLS_ERR_SSL_CONN_EOF ||
		    ret == MBEDTLS_ERR_NET_RECV_FAILED ||
		    ret == MBEDTLS_ERR_NET_CONN_RESET) {
			throw std::runtime_error("connection closed by remote");
		}
		throw std::runtime_error("TLS read failed: " + MbedError(ret));
	}
};

TlsTransport::TlsTransport(const struct addrinfo *const destination,
			   const std::string &host,
			   const bhf::ads::TlsSettings &settings,
			   const std::atomic<bool> *const abort)
	: d(new Impl{ destination, abort })
{
	d->host = host;
	d->Configure(settings);
	d->Handshake();
}

TlsTransport::TlsTransport(const struct addrinfo *const destination,
			   const std::string &host,
			   const bhf::ads::TlsSettings &settings,
			   AmsNetId ownNetId, const AmsNetId target,
			   const std::atomic<bool> *const abort)
	: TlsTransport(destination, host, settings, abort)
{
	if (!d->psk && !d->sca) {
		d->CheckPeer();
	}
	if (!ownNetId) {
		ownNetId = AmsNetId{ OwnIp() };
	}
	// PSK/SCA: the entry on the target is the route, no flags (as the Kotlin client)
	d->targetNetId = d->Exchange(d->psk || d->sca ?
					     0 :
					     FLAG_SELF_SIGNED | FLAG_IP_ADDR,
				     ownNetId, {}, {});
	// with PSK/SCA TwinCAT reports 0.0.0.0.0.0; AmsNetId's operator bool does
	// not help here, its empty value is 0.0.0.0.1.1
	static const uint8_t zero[sizeof(target.b)] = {};
	if (std::memcmp(d->targetNetId.b, zero, sizeof(zero)) &&
	    std::memcmp(d->targetNetId.b, target.b, sizeof(target.b))) {
		LOG_WARN("Secure ADS: target " << target << " reports AmsNetId "
					       << d->targetNetId);
	}
}

AmsNetId TlsTransport::AddRoute(const struct addrinfo *const destination,
				const std::string &host,
				const bhf::ads::TlsSettings &settings,
				AmsNetId ownNetId, const std::string &user,
				const std::string &password)
{
	if (!settings.pskIdentity.empty() || !settings.caFile.empty()) {
		throw TlsError(ADSERR_CLIENT_INVALIDPARM,
			       "with PSK or CA there is no route to add; the entry "
			       "<Server><Tls> on the target is the route");
	}
	TlsTransport transport{ destination, host, settings };
	if (!ownNetId) {
		ownNetId = AmsNetId{ transport.OwnIp() };
	}
	const auto target = transport.d->Exchange(
		FLAG_ADD_REMOTE | FLAG_SELF_SIGNED | FLAG_IP_ADDR, ownNetId, user,
		password);
	transport.d->SavePeer();
	return target;
}

TlsTransport::~TlsTransport()
{
	if (d) {
		Shutdown();
	}
}

size_t TlsTransport::read(uint8_t *const buffer, const size_t maxBytes,
			  timeval *const timeout) const
{
	{
		std::lock_guard<std::mutex> lock(d->mutex);
		if (mbedtls_ssl_get_bytes_avail(&d->ssl)) {
			return d->ReadLocked(buffer, maxBytes);
		}
	}
	// wait outside the lock, so that writers are not blocked meanwhile
	if (!d->tcp.Wait(timeout)) {
		return 0;
	}
	std::lock_guard<std::mutex> lock(d->mutex);
	return d->ReadLocked(buffer, maxBytes);
}

size_t TlsTransport::write(const Frame &frame) const
{
	std::lock_guard<std::mutex> lock(d->mutex);
	if (d->closed) {
		return 0;
	}
	try {
		d->WriteAll(frame.data(), frame.size());
	} catch (const std::exception &e) {
		LOG_ERROR(e.what());
		return 0;
	}
	return frame.size();
}

void TlsTransport::Shutdown()
{
	{
		// the receive thread may hold the lock while it waits for a record
		std::unique_lock<std::mutex> lock(d->mutex, std::try_to_lock);
		if (lock.owns_lock() && d->established && !d->closed) {
			mbedtls_ssl_close_notify(&d->ssl);
			d->closed = true;
		}
	}
	d->tcp.Shutdown();
}

uint32_t TlsTransport::OwnIp() const
{
	return d->tcp.Connect();
}

bool TlsTransport::IsConnectedTo(const struct addrinfo *const targetAddresses) const
{
	return d->tcp.IsConnectedTo(targetAddresses);
}

AmsNetId TlsTransport::TargetNetId() const
{
	return d->targetNetId;
}
