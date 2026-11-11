#pragma once

#include <libxml/tree.h>
#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>

// Shared structural checks of the FogHub HTML documents (foghub_html.cpp,
// foghub_review_html.cpp). Declarations are judged on the built tree, never on
// raw text: the same text also occurs inside script/style.
namespace fog::foghub::html_tree {

// libxml2 keeps the raw text of these elements as CDATA nodes (2.9.8:
// script/style; the HTML5 tokenizer of 2.14+ also the other ones).
inline bool rawTextElement(const xmlNode* node) {
    static const char* const names[] = {"script", "style", "xmp", "iframe", "noembed", "noframes", "plaintext"};
    return node && node->type == XML_ELEMENT_NODE &&
        std::any_of(std::begin(names), std::end(names), [&](const char* name) { return xmlStrEqual(node->name, BAD_CAST name); });
}

// FogHub pages need no DTD/entity declaration or CDATA section, and the HTML
// parser builds none from markup. Any other node kind fails closed.
inline bool allowedNode(const xmlNode* node) {
    switch (node->type) {
    case XML_ELEMENT_NODE: case XML_TEXT_NODE: case XML_COMMENT_NODE: case XML_PI_NODE: return true;
    case XML_CDATA_SECTION_NODE: return rawTextElement(node->parent);
    case XML_DTD_NODE: return node == reinterpret_cast<const xmlNode*>(node->doc->intSubset) && !node->children;
    default: return false;
    }
}

// The pages declare at most a plain "<!DOCTYPE html>". Both parsers skip an
// internal DTD subset ("<!DOCTYPE html [...]>") without declaring anything,
// but libxml2 2.9.8 exposes only the last parse error, so a later tolerated
// one hides this one: such a leading declaration fails closed. Only the
// prolog before it (BOM, whitespace, comments, processing instructions) is
// read, where no script/style text can occur.
inline bool leadingInternalSubset(const std::string& html) {
    std::size_t at = html.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    for (;;) {
        at = html.find_first_not_of(" \t\r\n\f", at);
        if (at == std::string::npos) return false;
        std::size_t end = std::string::npos;
        if (html.compare(at, 4, "<!--") == 0) end = html.find("-->", at + 4);
        else if (html.compare(at, 2, "<?") == 0) end = html.find('>', at + 2);
        else break;
        if (end == std::string::npos) return false;
        at = end + (html[end] == '>' ? 1 : 3);
    }
    static const std::string doctype = "<!doctype";
    if (html.size() - at < doctype.size()) return false;
    for (std::size_t i = 0; i < doctype.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(html[at + i])) != doctype[i]) return false;
    const auto end = html.find('>', at);
    return html.find('[', at) < end;
}

}  // namespace fog::foghub::html_tree
