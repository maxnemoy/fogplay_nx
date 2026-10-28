#!/usr/bin/env python3
"""Copy the pinned HTTP client and apply explicit, fail-closed probe fixes."""
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / "third_party/moonlight"
SOURCE = BASE / "moonlight-switch/app/src"
DEST = BASE / "prepared"

for directory in ("libgamestream", "crypto"):
    shutil.copytree(SOURCE / directory, DEST / directory, dirs_exist_ok=True)
(DEST / "core").mkdir(parents=True, exist_ok=True)
for name in ("Connection.c", "VideoStream.c", "RtpVideoQueue.c",
             "VideoDepacketizer.c", "ControlStream.c", "LinkedBlockingQueue.c", "PlatformSockets.c"):
    shutil.copy2(BASE / "moonlight-switch/extern/moonlight-common-c/src" / name,
                 DEST / "core" / name)


def replace(file, old, new, count=1):
    path = DEST / file
    text = path.read_text()
    if text.count(old) != count:
        raise RuntimeError(f"Pinned source mismatch: {file}: {old[:70]!r}")
    path.write_text(text.replace(old, new))


# Probe callbacks only publish state, and never call LiStopConnection(). Keep
# termination delivery on the originating core thread so normal core teardown
# joins it; an untracked detached callback could otherwise outlive the NRO.
replace("core/Connection.c", '#include "Limelight-internal.h"',
        '#include "Limelight-internal.h"\n#include <stdatomic.h>')
replace("core/Connection.c", """static bool alreadyTerminated;
static PLT_THREAD terminationCallbackThread;
static int terminationCallbackErrorCode;""", "static atomic_bool alreadyTerminated;")
replace("core/Connection.c", """static void terminationCallbackThreadFunc(void* context)
{
    // Invoke the client's termination callback
    originalTerminationCallback(terminationCallbackErrorCode);
}

// This shim callback runs the client's connectionTerminated() callback on a
// separate thread. This is neccessary because other internal threads directly
// invoke this callback. That can result in a deadlock if the client
// calls LiStopConnection() in the callback when the cleanup code
// attempts to join the thread that the termination callback (and LiStopConnection)
// is running on.
static void ClInternalConnectionTerminated(int errorCode)
{
    int err;

    // Avoid recursion and issuing multiple callbacks
    if (alreadyTerminated || ConnectionInterrupted) {
        return;
    }

    terminationCallbackErrorCode = errorCode;
    alreadyTerminated = true;

    // Invoke the termination callback on a separate thread
    err = PltCreateThread("AsyncTerm", terminationCallbackThreadFunc, NULL, &terminationCallbackThread);
    if (err != 0) {
        // Nothing we can safely do here, so we'll just assert on debug builds
        Limelog("Failed to create termination thread: %d\\n", err);
        LC_ASSERT(err == 0);
    }

    // Detach the thread since we never wait on it
    PltDetachThread(&terminationCallbackThread);
}""", """// Isolated probe contract: callbacks only publish state/log; they must never
// stop the connection here. The owner's worker calls LiStopConnection(), whose
// stream-thread joins now also wait for termination callback completion.
static void ClInternalConnectionTerminated(int errorCode)
{
    if (ConnectionInterrupted ||
        atomic_exchange_explicit(&alreadyTerminated, true, memory_order_relaxed)) {
        return;
    }
    originalTerminationCallback(errorCode);
}""")

# libnx randomGet obtains OS cryptographic entropy (unlike upstream rand()).
replace("crypto/Data.cpp", "#include <cstdlib>", "#include <cstdlib>\n#include <switch.h>")
random_source = (DEST / "crypto/Data.cpp").read_text()
start = random_source.index("    unsigned char* bytes =", random_source.index("Data Data::random_bytes"))
end = random_source.index("\n}\n", start)
random_source = random_source[:start] + "    Data random_data(size);\n    randomGet(random_data.bytes(), size);\n    return random_data;" + random_source[end:]
(DEST / "crypto/Data.cpp").write_text(random_source)

replace("crypto/MbedTLSCryptoManager.cpp", "    // TODO\n    return true;", """    mbedtls_x509_crt certificate;
    mbedtls_x509_crt_init(&certificate);
    unsigned char hash[32];
    int result = mbedtls_x509_crt_parse(&certificate, cert.bytes(), cert.size() + 1);
    if (result == 0) {
        result = _mbedtls_sha256(data.bytes(), data.size(), hash);
    }
    if (result == 0) {
        result = mbedtls_pk_verify(&certificate.pk, MBEDTLS_MD_SHA256,
                                   hash, sizeof(hash), signature.bytes(), signature.size());
    }
    mbedtls_x509_crt_free(&certificate);
    return result == 0;""")

# Authenticate the challenge as well as the signed server secret.
replace("libgamestream/client.cpp", "    Data clientPairingSecret = clientSecret.append(", """    if (serverChallengeRespHash.size() != serverResponse.size() ||
        memcmp(serverChallengeRespHash.bytes(), serverResponse.bytes(), serverResponse.size()) != 0) {
        gs_set_error("Pairing server challenge mismatch");
        return gs_pair_cleanup(GS_FAILED, server, &result);
    }

    Data clientPairingSecret = clientSecret.append(""")
replace("libgamestream/client.cpp", "(ret = gs_pair_validate(data, &result) != GS_OK)", "(ret = gs_pair_validate(data, &result)) != GS_OK", 5)
replace("libgamestream/client.cpp", "(ret = xml_status(data) != GS_OK)", "(ret = xml_status(data)) != GS_OK", 3)
replace("libgamestream/client.cpp", """    //    if (strcmp(*result, "1") != 0) {
    //        gs_error = "Pairing failed";
    //        ret = GS_FAILED;
    //    }""", """    if (*result != "1") {
        gs_set_error("Pairing acknowledgement missing or rejected");
        ret = GS_FAILED;
    }""")
replace("libgamestream/client.cpp", '    if (result == "0") {', '    if (result != "1") {', 2)
replace("libgamestream/client.cpp", """    if (stateText == "_SERVER_BUSY") {
        // After GFE 2.8, current game remains set even after streaming
        // has ended. We emulate the old behavior by forcing it to zero
        // if streaming is not active.
        server->currentGame = 0;
    }
""", "    // Preserve the reported app ID for the probe's ownership checks.\n")
replace("libgamestream/client.cpp", "    http_init(Settings::instance().key_dir());", """    // Bind the stable client ID to this probe's own certificate, avoiding the
    // upstream global ID shared by every fresh installation.
    Data identifier = CryptoManager::SHA256_hash_data(CryptoManager::cert_data()).subdata(0, 8).hex();
    unique_id.assign(reinterpret_cast<const char*>(identifier.bytes()), identifier.size());
    http_init(Settings::instance().key_dir());""")

# Secrets and server replies must never be emitted by libcurl's verbose output.
replace("libgamestream/http.cpp", '#include "http.h"', '#include "http.h"\n#include "Settings.hpp"\n#include "Diagnostics.hpp"\n#include <chrono>')
replace("libgamestream/http.cpp", "CURL* makeCurl();", """static int cancel_transfer(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return Settings::instance().request_cancelled() ? 1 : 0;
}

CURL* makeCurl();""")
replace("libgamestream/http.cpp", "    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);", """    curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancel_transfer);""")
replace("libgamestream/http.cpp", "    if (!curl) return GS_FAILED;", """    if (!curl) {
        free(http_data->memory);
        free(http_data);
        return GS_FAILED;
    }""")
replace("libgamestream/http.cpp", "        free(http_data);\n        return GS_FAILED;\n    } else", "        free(http_data);\n        freeCurl(curl);\n        return GS_FAILED;\n    } else")
replace("libgamestream/http.cpp", "        return GS_OUT_OF_MEMORY;", "        freeCurl(curl);\n        return GS_OUT_OF_MEMORY;")
replace("libgamestream/http.cpp", '    brls::Logger::info("Curl: Request:\\n{}", url.c_str());', """    if (Settings::instance().request_cancelled()) {
        gs_set_error("HTTP request cancelled or deadline reached");
        return GS_FAILED;
    }
    brls::Logger::info("Curl: Request");""")
replace("libgamestream/http.cpp", "    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);", """    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                     Settings::instance().remaining_timeout_ms(static_cast<long>(timeout) * 1000));""")
replace("libgamestream/http.cpp", "    CURLcode res = curl_easy_perform(curl);", """    const std::string operation = moonlight_diagnostics::operation(url);
    const std::string transport = url.compare(0, 8, "https://") == 0 ? "https" : "http";
    moonlight_diagnostics::emit("HTTP begin operation=" + operation + " transport=" + transport);
    const auto started = std::chrono::steady_clock::now();
    moonlight_diagnostics::httpDispatched = false;
    CURLcode res = curl_easy_perform(curl);
    long responseCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    moonlight_diagnostics::commandHttp(static_cast<int>(res),responseCode,
        static_cast<std::uint64_t>(elapsed),moonlight_diagnostics::httpDispatched,
        Settings::instance().request_cancelled());
    moonlight_diagnostics::emit("HTTP end operation=" + operation + " transport=" + transport +
        " curl=" + std::to_string(static_cast<int>(res)) + " http=" + std::to_string(responseCode) +
        " elapsed_ms=" + std::to_string(elapsed));""")
# Never forward response bodies or CURL_ERRORBUFFER: they can contain identity data.
replace("libgamestream/http.cpp", 'brls::Logger::info("Curl: Response:\\n{}", http_data->memory);',
        'brls::Logger::info("Curl: Response received (body omitted)");')
http = DEST / "libgamestream/http.cpp"
text = http.read_text()
start = text.index("#if LIBCURL_VERSION_NUM >= 0x075600")
end = text.index('        brls::Logger::info("Curl: {}", curl_version());', start)
text = text[:start] + """        const auto backend = curl_global_sslset(CURLSSLBACKEND_MBEDTLS, nullptr, nullptr);
        if (backend != CURLSSLSET_OK || curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
            moonlight_diagnostics::emit("TLS initialization failed; mbedTLS backend required");
            return GS_FAILED;
        }
        const auto* version = curl_version_info(CURLVERSION_NOW);
        if (!version || !version->ssl_version || !std::strstr(version->ssl_version, "mbedTLS")) {
            moonlight_diagnostics::emit("TLS backend mismatch; PEM identity requires mbedTLS");
            curl_global_cleanup();
            return GS_FAILED;
        }
        moonlight_diagnostics::emit("TLS backend=mbedTLS identity=PEM");
""" + text[end:]
http.write_text(text)
replace("libgamestream/client.cpp", "    http_init(Settings::instance().key_dir());",
        "    if (http_init(Settings::instance().key_dir()) != GS_OK) return GS_FAILED;")
replace("libgamestream/client.cpp", '#include "client.h"', '#include "client.h"\n#include "Diagnostics.hpp"')
for reason in ("MITM attack detected", "Pairing server challenge mismatch", "Pairing acknowledgement missing or rejected"):
    replace("libgamestream/client.cpp", f'gs_set_error("{reason}");',
            f'moonlight_diagnostics::emit("Pairing verification failed: {reason}");\n        gs_set_error("{reason}");')
# Avoid realloc losing the original allocation on failure.
replace("libgamestream/http.cpp", """    mem->memory = (char*)realloc(mem->memory, mem->size + realsize + 1);
    if (mem->memory == NULL)
        return 0;""", """    char* resized = (char*)realloc(mem->memory, mem->size + realsize + 1);
    if (resized == NULL)
        return 0;
    mem->memory = resized;""")

from production import apply
apply(DEST)

# Preserve only numeric command outcomes. Never copy request URLs, XML bodies,
# status messages or gs_error() into a diagnostic snapshot.
replace("libgamestream/http.cpp",
        '    if (type == CURLINFO_HEADER_OUT && context) Settings::instance().pairing_dispatched();',
        '''    if (type == CURLINFO_HEADER_OUT) moonlight_diagnostics::httpDispatched = true;
    if (type == CURLINFO_HEADER_OUT && context) Settings::instance().pairing_dispatched();''')
replace("libgamestream/http.cpp",
        '        gs_set_error("HTTP request cancelled or deadline reached");',
        '''        moonlight_diagnostics::commandHttp(42,0,0,false,true);
        gs_set_error("HTTP request cancelled or deadline reached");''')
replace("libgamestream/xml.cpp", '#include "xml.h"', '#include "xml.h"\n#include "Diagnostics.hpp"')
replace("libgamestream/xml.cpp", '                *status = atoi(atts[i + 1]);',
        '''                *status = atoi(atts[i + 1]);
                moonlight_diagnostics::commandXmlStatusText(atts[i + 1]);''')
client = DEST / "libgamestream/client.cpp"
text = client.read_text()
start = text.index("int gs_start_app(")
end = text.index("int gs_quit_app(",start)
command = text[start:end]
command = command.replace('    Data rand = Data::random_bytes(16);',
        '''    const bool resuming = server->currentGame != 0;
    moonlight_diagnostics::beginCommand(!resuming ?
        moonlight_diagnostics::CommandOperation::Launch : moonlight_diagnostics::CommandOperation::Resume);
    Data rand = Data::random_bytes(16);''',1)
if command.count('xml_search(data, "gamesession", &result)') != 1:
    raise RuntimeError("Pinned GameStream acknowledgement boundary changed")
command = command.replace('xml_search(data, "gamesession", &result)',
        'xml_search(data, resuming ? "resume" : "gamesession", &result)',1)
if command.count('if (xml_search(data, "sessionUrl0", &result) == GS_OK)') != 1:
    raise RuntimeError("Pinned GameStream session URL boundary changed")
command = command.replace('if (xml_search(data, "sessionUrl0", &result) == GS_OK)',
        'if (xml_search(data, "sessionUrl0", &result) == GS_OK && !result.empty())',1)
command = command.replace('    if ((ret = xml_status(data)) != GS_OK) {',
        '''    ret = xml_status(data);
    moonlight_diagnostics::commandXmlResult(ret);
    if (ret != GS_OK) {''',1)
command = command.replace('    if (result != "1") {',
        '''    moonlight_diagnostics::commandGameSession(result == "1" ? 1 : result == "0" ? 0 : -1);
    if (result != "1") {''',1)
command = command.replace('exit:\n    return ret;',
        '''exit:
    moonlight_diagnostics::finishCommand(ret,Settings::instance().request_cancelled());
    return ret;''',1)
if command.count("beginCommand(") != 1 or command.count("finishCommand(") != 1 or command.count("commandXmlResult(") != 1:
    raise RuntimeError("Pinned GameStream command diagnostic boundary changed")
client.write_text(text[:start] + command + text[end:])

from diagnostics import apply as apply_diagnostics
apply_diagnostics(DEST)

from recovery import apply as apply_recovery
apply_recovery(DEST)
