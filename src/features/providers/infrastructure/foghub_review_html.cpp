#include "features/providers/infrastructure/foghub_review_html.hpp"
#include "features/providers/infrastructure/foghub_html.hpp"
#include "core/domain/repository_error.hpp"
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <algorithm>
#include <cctype>
#include <memory>
#include <set>
#include <vector>

namespace fog::foghub {
namespace {
[[noreturn]] void invalid() { throw RepositoryError(RepositoryErrorKind::InvalidResponse,"FogHub review page could not be verified"); }
void require(bool value) { if (!value) invalid(); }
using Doc = std::unique_ptr<xmlDoc,decltype(&xmlFreeDoc)>;
using Nodes = std::vector<xmlNode*>;
std::string attr(xmlNode* node,const char* name) {
    auto* raw=xmlGetProp(node,BAD_CAST name); if (!raw) return {};
    std::string value(reinterpret_cast<const char*>(raw)); xmlFree(raw); return value;
}
bool has(xmlNode* node,const char* name) { return xmlHasProp(node,BAD_CAST name); }
std::string text(xmlNode* node) {
    auto* raw=xmlNodeGetContent(node); require(raw);
    std::string value(reinterpret_cast<const char*>(raw)); xmlFree(raw); return value;
}
std::string trim(std::string value) {
    const auto first=value.find_first_not_of(" \t\r\n"),last=value.find_last_not_of(" \t\r\n");
    return first==std::string::npos ? std::string{} : value.substr(first,last-first+1);
}
Nodes select(xmlNode* node,const char* query) {
    std::unique_ptr<xmlXPathContext,decltype(&xmlXPathFreeContext)> ctx(xmlXPathNewContext(node->doc),xmlXPathFreeContext);
    require(bool(ctx)); ctx->node=node;
    std::unique_ptr<xmlXPathObject,decltype(&xmlXPathFreeObject)> result(xmlXPathEvalExpression(BAD_CAST query,ctx.get()),xmlXPathFreeObject);
    require(result && result->type==XPATH_NODESET); Nodes nodes;
    if (result->nodesetval) for (int i=0;i<result->nodesetval->nodeNr;++i) nodes.push_back(result->nodesetval->nodeTab[i]);
    return nodes;
}
xmlNode* one(xmlNode* node,const char* query) { auto nodes=select(node,query); require(nodes.size()==1); return nodes.front(); }
void bounds(xmlNode* node,unsigned depth,std::size_t& count) {
    for (;node;node=node->next) { require(++count<=60000 && depth<=96); if(node->children) bounds(node->children,depth+1,count); }
}
bool tolerated(const xmlError* error) {
    return !error || error->level<XML_ERR_ERROR || error->code==XML_HTML_UNKNOWN_TAG ||
        (error->code==XML_ERR_TAG_NAME_MISMATCH && error->str1 && std::string(error->str1)=="p");
}
struct Errors { bool invalid=false; };
#if LIBXML_VERSION >= 21200
void parseError(void* data,const xmlError* error) {
#else
void parseError(void* data,xmlError* error) {
#endif
    auto* context=static_cast<xmlParserCtxt*>(data);
    if (!tolerated(error)) static_cast<Errors*>(context->_private)->invalid=true;
}
void quiet(void*,const char*,...) {}
#if LIBXML_VERSION >= 21300
void contextError(void* data,const xmlError* error) { if(!tolerated(error)) static_cast<Errors*>(data)->invalid=true; }
#endif
Doc document(const std::string& html) {
    require(!html.empty() && html.size()<=2*1024*1024 && html.find('\0')==std::string::npos);
    auto folded=html; for(auto& c:folded) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    require(folded.find("<!entity")==std::string::npos && folded.find("<![")==std::string::npos);
    std::unique_ptr<htmlParserCtxt,decltype(&htmlFreeParserCtxt)> context(htmlNewParserCtxt(),htmlFreeParserCtxt);
    require(bool(context)); Errors errors; context->_private=&errors;
    context->sax->serror=parseError; context->sax->warning=quiet; context->sax->error=quiet; context->sax->fatalError=quiet;
#if LIBXML_VERSION >= 21300
    xmlCtxtSetErrorHandler(context.get(),contextError,&errors);
#endif
    Doc doc(htmlCtxtReadMemory(context.get(),html.data(),static_cast<int>(html.size()),nullptr,"UTF-8",
        HTML_PARSE_NONET|HTML_PARSE_COMPACT|HTML_PARSE_NODEFDTD),xmlFreeDoc);
    require(doc && xmlDocGetRootElement(doc.get()) && !errors.invalid && tolerated(xmlCtxtGetLastError(context.get())));
    require(!doc->extSubset && (!doc->intSubset || !doc->intSubset->children));
    std::size_t count=0; bounds(xmlDocGetRootElement(doc.get()),0,count); return doc;
}
void id(const std::string& value) {
    require(!value.empty() && value.size()<=20 && value.front()!='0' &&
        std::all_of(value.begin(),value.end(),[](unsigned char c){return c>='0'&&c<='9';}));
}
void noOverrides(xmlNode* node) {
    for (auto* property=node->properties;property;property=property->next) {
        std::string name(reinterpret_cast<const char*>(property->name));
        require(name.rfind("on",0)!=0 && name!="form" && name!="formaction" && name!="formmethod" &&
            name!="formenctype" && name!="formtarget" && name!="formnovalidate" && name!="disabled");
    }
}
}
SessionReviewForm parseSessionReviewForm(const std::string& html,const std::string& rental) {
    id(rental); auto doc=document(html); auto* root=xmlDocGetRootElement(doc.get());
    if(!select(root,"//form[@id='login-form' or @action='/site/login']").empty())
        throw RepositoryError(RepositoryErrorKind::NotAuthenticated,"FogHub sign-in required");
    require(select(root,"//base | //*[@id='cf-challenge-running' or @id='challenge-form' or @id='captcha' or contains(@id,'recaptcha')]").empty());
    auto* logout=one(root,"//form[@action='/site/logout']"); require(attr(logout,"method")=="post");
    require(select(logout,".//input[@name='_csrf' and @type='hidden']").size()==1);
    auto* main=one(root,"//main");require(attr(main,"id")=="main" && attr(main,"role")=="main");
    require(trim(text(one(main,".//h1")))=="Оцените игру:");
    auto* form=one(main,".//form"); require(attr(form,"action")=="/session/review?id="+rental && attr(form,"method")=="post");
    require(!has(form,"target") && !has(form,"enctype") && !has(form,"novalidate")); noOverrides(form);
    require(select(root,"//form").size()==2);
    for(auto* control:select(root,"//*[@form]")) { (void)control; invalid(); }
    std::set<std::string> values; unsigned csrfCount=0,hidden=0,messages=0,checked=0;
    SessionReviewForm result;
    for(auto* node:select(form,".//*")) noOverrides(node);
    for(auto* node:select(form,".//input | .//select | .//textarea | .//button")) {
        const auto name=attr(node,"name"),type=attr(node,"type");
        const std::string tag(reinterpret_cast<const char*>(node->name));
        if(name.empty()) { require(tag=="button" && (type=="button" || type=="submit")); continue; }
        if(name=="_csrf") {
            require(tag=="input" && type=="hidden" && ++csrfCount==1);
            result.csrf=attr(node,"value"); require(!result.csrf.empty() && result.csrf.size()<=4096 &&
                std::none_of(result.csrf.begin(),result.csrf.end(),[](unsigned char c){return c<32||c==127;}));
        } else if(name=="Review[rating]") {
            require(tag=="input");
            if(type=="hidden") require(++hidden==1 && attr(node,"value").empty() && !has(node,"checked"));
            else { const auto value=attr(node,"value"); require(type=="radio" && value.size()==1 && value[0]>='1' && value[0]<='5' && values.insert(value).second);
                if(has(node,"checked")) {require(value=="5");++checked;} }
        } else if(name=="Review[message]") {
            require(tag=="textarea" && ++messages==1 && attr(node,"maxlength")=="2000" && !has(node,"required") && trim(text(node)).empty());
        } else invalid();
    }
    require(csrfCount==1 && hidden==1 && values.size()==5 && checked==1 && messages==1); return result;
}
std::map<std::string,std::optional<int>> sessionHistoryRatings(const std::string& html) {
    const auto history=parseHistory(html);
    std::map<std::string,std::optional<int>> ratings;
    for(const auto& row:history.rows) if(row.state==HistoryState::Finished) ratings.emplace(row.id,std::nullopt);
    auto doc=document(html); auto* root=xmlDocGetRootElement(doc.get()); std::set<std::string> seen;
    for(auto* row:select(root,"//tr[@data-key]")) {
        const auto rental=attr(row,"data-key");const auto found=ratings.find(rental);if(found==ratings.end())continue;
        require(seen.insert(rental).second);const auto cells=select(row,"./td");require(cells.size()==6);
        const auto review=select(cells[5],".//a[@href]");
        if(!review.empty()) { require(review.size()==1 && attr(review.front(),"href")=="/session/review?id="+rental);continue; }
        auto* div=one(cells[5],"./small/div"); std::string first;
        for(auto* node=div->children;node;node=node->next) {if(node->type==XML_ELEMENT_NODE)break;if(node->type==XML_TEXT_NODE&&node->content)first+=reinterpret_cast<const char*>(node->content);}
        first=trim(first); require(first.rfind("Оценка:",0)==0);const auto score=trim(first.substr(std::string("Оценка:").size()));
        require(score.size()==1 && score[0]>='1' && score[0]<='5');found->second=score[0]-'0';
    }
    require(seen.size()==ratings.size());return ratings;
}
std::optional<int> sessionHistoryRating(const std::string& html,const std::string& rental) {
    id(rental);const auto ratings=sessionHistoryRatings(html);const auto found=ratings.find(rental);
    require(found!=ratings.end());return found->second;
}
}
