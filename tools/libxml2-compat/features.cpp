#include <libxml/parser.h>
#include <cstdio>
#include <cstring>
int main() {
    if (LIBXML_VERSION != 20908 || std::strcmp(xmlParserVersion, "20908") != 0) return 1;
    std::printf("version=%s runtime=%s\n", LIBXML_DOTTED_VERSION, xmlParserVersion);
    for (int i = 1; i <= 33; ++i) std::printf("feature.%d=%d\n", i, xmlHasFeature(static_cast<xmlFeature>(i)));
}