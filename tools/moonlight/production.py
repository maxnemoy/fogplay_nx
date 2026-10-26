#!/usr/bin/env python3
"""Production additions to the prepared pinned sources; probe remains opt-in LAN."""
from pathlib import Path


def apply(dest: Path):
    def change(file, old, new, count=1):
        path = dest / file
        text = path.read_text()
        if text.count(old) != count:
            raise RuntimeError(f"Pinned production source mismatch: {file}: {old[:70]!r}")
        path.write_text(text.replace(old, new))

    # Cache identity by directory. A new rental must never silently reuse the
    # previous global certificate or the provider would receive a useless PIN.
    change("crypto/MbedTLSCryptoManager.cpp", "static Data m_cert;", "static Data m_cert;\nstatic std::string m_identity_directory;")
    change("crypto/MbedTLSCryptoManager.cpp", "bool MbedTLSCryptoManager::load_cert_key_pair() {", """bool MbedTLSCryptoManager::load_cert_key_pair() {
    if (m_identity_directory != Settings::instance().key_dir()) {
        m_cert = Data(); m_key = Data();
        m_identity_directory = Settings::instance().key_dir();
    }""")
    change("crypto/Data.cpp", "#include <cstdlib>", "#include <cstdlib>\n#include <stdexcept>")
    change("crypto/Data.cpp", "    if (start + size > m_size) {", "    if (start > m_size || size > m_size - start) {")
    change("crypto/Data.cpp", "        exit(-1);", '        throw std::runtime_error("Invalid Moonlight data length");')
    change("crypto/Data.cpp", "Data Data::hex_to_bytes() const {", """Data Data::hex_to_bytes() const {
    if (m_size % 2 != 0) throw std::runtime_error("Invalid Moonlight hex length");
    for (size_t index = 0; index < m_size; ++index) {
        unsigned char value = m_bytes[index];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F')))
            throw std::runtime_error("Invalid Moonlight hex value");
    }""")
    change("libgamestream/http.cpp", "    } else {\n        return GS_OK;\n    }\n\n    certificateFilePath", "    }\n\n    certificateFilePath")

    # The certificate is authenticated by gs_pair's signed secret and PIN hash.
    # Pin its SPKI, including the final HTTPS pairing acknowledgement. Hostname
    # checks cannot authenticate Sunshine's generated self-signed identity.
    change("libgamestream/http.h", "#pragma once", "#pragma once\nint http_pin_certificate(const Data& certificate);")
    # Data must be declared before the declaration (header layout is pinned).
    path = dest / "libgamestream/http.h"
    text = path.read_text().replace("int http_pin_certificate(const Data& certificate);", "class Data;\nint http_pin_certificate(const Data& certificate);")
    path.write_text(text)
    change("libgamestream/http.cpp", "#include <chrono>", """#include <chrono>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

int http_pin_certificate(const Data& certificate) {
    if (!Settings::instance().production()) return GS_OK;
    mbedtls_x509_crt cert; mbedtls_x509_crt_init(&cert);
    int rc = mbedtls_x509_crt_parse(&cert, certificate.bytes(), certificate.size() + 1);
    unsigned char der[4096], hash[32], encoded[64]; size_t size = 0;
    int length = rc == 0 ? mbedtls_pk_write_pubkey_der(&cert.pk, der, sizeof(der)) : -1;
    if (length > 0) {
#if MBEDTLS_VERSION_MAJOR >= 3
        rc = mbedtls_sha256(der + sizeof(der) - length, length, hash, 0);
#else
        rc = mbedtls_sha256_ret(der + sizeof(der) - length, length, hash, 0);
#endif
        if (rc == 0) rc = mbedtls_base64_encode(encoded, sizeof(encoded), &size, hash, sizeof(hash));
    } else rc = -1;
    mbedtls_x509_crt_free(&cert);
    if (rc != 0) return GS_FAILED;
    Settings::instance().set_pinned_key("sha256//" + std::string(reinterpret_cast<char*>(encoded), size));
    return GS_OK;
}

// libcurl's HEADER_OUT event means the request has entered transport dispatch;
// it is not a remote acknowledgement. Swallow every debug payload: URLs contain
// the client certificate and pairing secrets. No debug text reaches logging.
static int dispatch_event(CURL*, curl_infotype type, char*, size_t, void* context) {
    if (type == CURLINFO_HEADER_OUT && context) Settings::instance().pairing_dispatched();
    return 0;
}""")
    # Move helper after includes so curl/Data/error types are available.
    path = dest / "libgamestream/http.cpp"
    text = path.read_text()
    begin = text.index("int http_pin_certificate(")
    end = text.index('#include "CryptoManager.hpp"', begin)
    helper = text[begin:end]
    text = text[:begin] + text[end:]
    end = text.index("static bool curlGlobalInit")
    text = text[:end] + helper + "\n" + text[end:]
    path.write_text(text)
    change("libgamestream/http.cpp", "    curl_easy_setopt(curl, CURLOPT_WRITEDATA, http_data);", """    if (Settings::instance().production() && url.compare(0, 8, "https://") == 0) {
        if (Settings::instance().pinned_key().empty()) {
            free(http_data->memory); free(http_data); freeCurl(curl); return GS_FAILED;
        }
        curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, Settings::instance().pinned_key().c_str());
    }
    if (Settings::instance().production()) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, dispatch_event);
        curl_easy_setopt(curl, CURLOPT_DEBUGDATA,
            url.find("phrase=getservercert") != std::string::npos ? reinterpret_cast<void*>(1) : nullptr);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, http_data);""")
    change("libgamestream/http.cpp", "    auto* http_data = (HTTP_DATA*)malloc(sizeof(HTTP_DATA));\n    http_data->memory = (char*)malloc(1);", """    auto* http_data = (HTTP_DATA*)malloc(sizeof(HTTP_DATA));
    if (!http_data) return GS_OUT_OF_MEMORY;
    http_data->memory = (char*)malloc(1);
    if (!http_data->memory) { free(http_data); return GS_OUT_OF_MEMORY; }
    http_data->memory[0] = 0;""")
    change("libgamestream/http.cpp", "    size_t realsize = size * nmemb;", """    if (size && nmemb > SIZE_MAX / size) return 0;
    size_t realsize = size * nmemb;""")
    change("libgamestream/http.cpp", "    auto* mem = (HTTP_DATA*)userp;", """    auto* mem = (HTTP_DATA*)userp;
    if (mem->size > 8 * 1024 * 1024 || realsize > 8 * 1024 * 1024 - mem->size) return 0;""")
    change("libgamestream/client.cpp", '    brls::Logger::info("Client: Start pairing stage #5");', """    if ((ret = http_pin_certificate(plainCert.hex_to_bytes())) != GS_OK)
        return gs_pair_cleanup(ret, server, &result);
    brls::Logger::info("Client: Start pairing stage #5");""")
    change("libgamestream/client.cpp", "    if (server->currentGame != 0) {", "    if (server->currentGame != 0 && !Settings::instance().production()) {")
    change("libgamestream/client.cpp", "    // Modern GFE versions don't allow serverinfo to be fetched over HTTPS if the client", """    // Before PIN-authentication production uses unsigned HTTP only for discovery.
    // After authentication every refresh must succeed over the pinned HTTPS key.
    if (Settings::instance().production()) {
        ret = load_serverinfo(server, !Settings::instance().pinned_key().empty());
        if (ret != GS_OK) return ret;
        return server->serverMajorVersion >= MIN_SUPPORTED_GFE_VERSION &&
               server->serverMajorVersion <= MAX_SUPPORTED_GFE_VERSION ? GS_OK : GS_UNSUPPORTED_VERSION;
    }
    // Modern GFE versions don't allow serverinfo to be fetched over HTTPS if the client""")
    change("libgamestream/client.cpp", "    server->httpsPort = atoi(httpsPortText.c_str());", """    if (Settings::instance().production() && (httpsPortText.empty() ||
        httpsPortText.find_first_not_of("0123456789") != std::string::npos ||
        httpsPortText.size() > 5 || std::strtoul(httpsPortText.c_str(), nullptr, 10) == 0 ||
        std::strtoul(httpsPortText.c_str(), nullptr, 10) > 65535)) return GS_INVALID;
    server->httpsPort = atoi(httpsPortText.c_str());""")
    # URL authority and raw streaming host must differ for IPv6 literals.
    change("libgamestream/client.h", "    std::string address;", "    std::string address;\n    std::string urlAddress;")
    path = dest / "libgamestream/client.cpp"
    text = path.read_text().replace("server->serverInfo.address,", "server->urlAddress.c_str(),")
    start = text.index("    std::stringstream addressStream(address);")
    end = text.index("    if (!CryptoManager::load_cert_key_pair())", start)
    text = text[:start] + """    std::string host = address;
    unsigned short httpPort = 47989;
    if (!address.empty() && address.front() == '[') {
        auto close = address.find(']');
        if (close == std::string::npos) return GS_INVALID;
        host = address.substr(1, close - 1);
        if (close + 1 < address.size()) {
            if (address[close + 1] != ':') return GS_INVALID;
            httpPort = static_cast<unsigned short>(std::strtoul(address.c_str() + close + 2, nullptr, 10));
        }
    } else if (auto colon = address.find(':'); colon != std::string::npos) {
        host = address.substr(0, colon);
        httpPort = static_cast<unsigned short>(std::strtoul(address.c_str() + colon + 1, nullptr, 10));
    }
    if (host.empty() || !httpPort) return GS_INVALID;

""" + text[end:]
    text = text.replace("    server->address = seglist[0];", "    server->address = host;\n    server->urlAddress = host.find(':') != std::string::npos ? \"[\" + host + \"]\" : host;")
    path.write_text(text)
    # Reject missing RTSP URL and avoid leaking an old URL on resume/retry.
    change("libgamestream/client.cpp", "        server->serverInfo.rtspSessionUrl = new char[size + 1];", "        delete[] server->serverInfo.rtspSessionUrl;\n        server->serverInfo.rtspSessionUrl = new char[size + 1];")
    change("libgamestream/client.cpp", '        brls::Logger::error("sessionUrl0 not found");', '        brls::Logger::error("sessionUrl0 not found");\n        ret = GS_INVALID;')
