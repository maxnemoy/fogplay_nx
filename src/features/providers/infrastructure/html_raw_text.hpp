#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace fog::foghub {
// HTML5 ends a script/style element only at its own end tag ("</script" or
// "</style", any case). libxml2 2.9.8 (the Switch portlibs version) uses HTML4
// rules and stops a script at the first "</" + letter, so markup inside
// JavaScript strings (FogHub's infinite-scroll "<div ...>...</div>") becomes
// stray end tags and a parse error that the host libxml2 2.15 never reports.
// No FogHub parser reads script or style text: blank their contents (tags kept)
// so both libxml2 versions build the same document. An unterminated element
// is blanked to the end, as HTML5 treats the rest as its text.
inline std::string withoutRawTextContent(const std::string& html) {
    std::string folded = html;
    std::transform(folded.begin(), folded.end(), folded.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string result;
    result.reserve(html.size());
    std::size_t copied = 0, at = 0;
    while (at < folded.size()) {
        const auto open = folded.find('<', at);
        if (open == std::string::npos) break;
        const char* name = nullptr;
        for (const char* candidate : {"script", "style"}) {
            const std::string tag(candidate);
            if (folded.compare(open + 1, tag.size(), tag) != 0) continue;
            const auto next = open + 1 + tag.size();
            if (next < folded.size() && (folded[next] == '>' || folded[next] == '/' || std::isspace(static_cast<unsigned char>(folded[next]))))
                name = candidate;
        }
        if (!name) { at = open + 1; continue; }
        const auto startEnd = folded.find('>', open);
        if (startEnd == std::string::npos) break; // Malformed start tag: leave as is; the parser fails closed.
        const auto close = folded.find(std::string("</") + name, startEnd + 1);
        result.append(html, copied, startEnd + 1 - copied);
        if (close == std::string::npos) { copied = at = html.size(); break; }
        copied = at = close; // Keep the end tag itself.
    }
    result.append(html, copied, std::string::npos);
    return result;
}
}
