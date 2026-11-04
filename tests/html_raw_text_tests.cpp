#include "features/providers/infrastructure/html_raw_text.hpp"
#include <cassert>
#include <iostream>
#include <string>

using fog::foghub::withoutRawTextContent;

int main() {
    // FogHub's infinite-scroll script holds markup in JavaScript strings; the
    // Switch libxml2 2.9.8 (HTML4) parser ended the script at "</div>".
    const std::string ias = "<main><p>x</p></main><script>ext({\"html\":\"<div class=\\\"ias\\\"><a>x</a></div>\"});</script><footer>f</footer>";
    assert(withoutRawTextContent(ias) == "<main><p>x</p></main><script></script><footer>f</footer>");
    // Case-insensitive tags, attributes, style, several elements; other markup untouched.
    assert(withoutRawTextContent("<SCRIPT type='x'>a</div></SCRIPT ><b>keep</b><style>p{}</style>") ==
           "<SCRIPT type='x'></SCRIPT ><b>keep</b><style></style>");
    assert(withoutRawTextContent("<script>1</script><p>2</p><script src=x></script>") ==
           "<script></script><p>2</p><script src=x></script>");
    // Only real script/style start tags count.
    assert(withoutRawTextContent("<scripts>a</scripts><styles>b</styles>") == "<scripts>a</scripts><styles>b</styles>");
    assert(withoutRawTextContent("<p>if (a <script) b</p>") == "<p>if (a <script) b</p>");
    // A script ends at its first end tag (HTML5), even if JavaScript continues.
    assert(withoutRawTextContent("<script>s = '</script>'; x</script>") == "<script></script>'; x</script>");
    // Unterminated element: the rest is its text; a malformed start tag is left for the parser to reject.
    assert(withoutRawTextContent("<p>a</p><script>never closed <div>") == "<p>a</p><script>");
    assert(withoutRawTextContent("<p>a</p><script") == "<p>a</p><script");
    assert(withoutRawTextContent("").empty() && withoutRawTextContent("plain") == "plain");
    std::cout << "HTML raw text: script/style contents blanked (case, attributes, HTML5 end, unterminated), other markup kept PASS\n";
}
