#include <Diagnostics.hpp>
#include <xml.h>
#include <errors.h>
#include "features/streaming/infrastructure/moonlight/resume_retry.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

void randomGet(void* target,std::size_t size) { std::memset(target,0,size); }
void gs_set_error(std::string) {} // Deliberately discard server-controlled text.

int main() {
    using Retry = moonlight::detail::ResumeRetry;
    for (const auto* payload : {"<root status_code='503'><gamesession>0</gamesession></root>",
        "<root status_code='503junk'><gamesession>0</gamesession></root>",
        "<root status_code='503'>"}) {
        moonlight_diagnostics::beginCommand(moonlight_diagnostics::CommandOperation::Resume);
        Data data(reinterpret_cast<unsigned char*>(const_cast<char*>(payload)),std::strlen(payload));
        const int result = xml_status(data);
        moonlight_diagnostics::commandXmlResult(result);
        const auto snapshot = moonlight_diagnostics::commandSnapshot();
        const bool retry = Retry::retryable(0,200,snapshot.xmlStatus,snapshot.xmlResult,true,false);
        if (std::strstr(payload,"503junk")) assert(snapshot.xmlStatus == 0 && !retry);
        else if (std::strstr(payload,"</root>")) assert(result == GS_ERROR && snapshot.xmlStatus == 503 && retry);
        else assert(result == GS_INVALID && !retry);
        moonlight_diagnostics::finishCommand(result,false);
    }
    std::cout << "Moonlight actual XML diagnostic parsing tests passed\n";
}
