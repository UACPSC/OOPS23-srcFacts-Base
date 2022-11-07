/*
    srcFacts.cpp

    Produces a report with various measures of source code.
    Supports C++, C, Java, and C#.

    Input is an XML file in the srcML format.

    Output is a markdown table with the measures.

    Output performance statistics to stderr.

    Code includes an embedded XML parser:
    * No checking for well-formedness
    * No DTD declarations
*/

#include <iostream>
#include <locale>
#include <iterator>
#include <string>
#include <algorithm>
#include <sys/types.h>
#include <errno.h>
#include <string_view>
#include <optional>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <memory>
#include <stdlib.h>
#include <bitset>

#if !defined(_MSC_VER)
#include <sys/uio.h>
#include <unistd.h>
#define READ read
#else
#include <BaseTsd.h>
#include <io.h>
typedef SSIZE_T ssize_t;
#define READ _read
#endif

// provides literal string operator""sv
using namespace std::literals::string_view_literals;

const int BUFFER_SIZE = 16 * 16 * 4096;

const std::bitset<128> tagNameMask("00000111111111111111111111111110100001111111111111111111111111100000001111111111011000000000000000000000000000000000000000000000");

constexpr auto WHITESPACE = " \n\t\r"sv;

/*
    Refill the buffer preserving the unused data.
    Current content is shifted left and new data
    appended to the rest of the content.

    @param[in, out] content View of buffer
    @return Number of bytes read
    @retval 0 EOF
    @retval -1 Read error
*/
int refillBuffer(std::string_view& content) {

    // initialize the buffer at first use
    static std::string buffer(BUFFER_SIZE, ' ');

    // move unprocessed characters in content to start of the buffer
    std::copy(content.cbegin(), content.cend(), buffer.begin());

    // read in whole blocks
    ssize_t readBytes = 0;
    while (((readBytes = READ(0, static_cast<void*>(buffer.data() + content.size()),
        buffer.size() - content.size())) == -1) && (errno == EINTR)) {
    }
    if (readBytes == -1)
        // error in read
        return -1;
    if (readBytes == 0) {
        // EOF
        content = ""sv;
        return 0;
    }

    // set content to the start of the buffer
    content = std::string_view(&buffer[0], content.size() + readBytes);

    return readBytes;
}

// trace parsing
#ifdef TRACE
#undef TRACE
#define HEADER(m) std::clog << std::setw(10) << std::left << m << '\t'
#define FIELD(l, n) l << ":|" << n << "| "
#define TRACE0(m)
#define TRACE1(m, l1, n1) HEADER(m) << FIELD(l1,n1) << '\n';
#define TRACE2(m, l1, n1, l2, n2) HEADER(m) << FIELD(l1,n1) << FIELD(l2,n2) << '\n';
#define TRACE3(m, l1, n1, l2, n2, l3, n3) HEADER(m) << FIELD(l1,n1) << FIELD(l2,n2) << FIELD(l3,n3) << '\n';
#define TRACE4(m, l1, n1, l2, n2, l3, n3, l4, n4) HEADER(m) << FIELD(l1,n1) << FIELD(l2,n2) << FIELD(l3,n3) << FIELD(l4,n4) << '\n';
#define GET_TRACE(_1,_2,_3,_4,_5,_6,_7,_8,_9,NAME,...) NAME
#define TRACE(...) GET_TRACE(__VA_ARGS__, TRACE4, _UNUSED, TRACE3, _UNUSED, TRACE2, _UNUSED, TRACE1, _UNUSED, TRACE0)(__VA_ARGS__)
#else
#define TRACE(...)
#endif

int main() {
    const auto start = std::chrono::steady_clock::now();
    std::string url;
    int textsize = 0;
    int loc = 0;
    int exprCount = 0;
    int functionCount = 0;
    int classCount = 0;
    int unitCount = 0;
    int declCount = 0;
    int commentCount = 0;
    int depth = 0;
    long totalBytes = 0;
    bool inTag = false;
    bool inXMLComment = false;
    bool inCDATA = false;
    std::string inTagQName;
    std::string_view inTagPrefix;
    std::string_view inTagLocalName;
    bool isArchive = false;
    std::string_view content;
    TRACE("START DOCUMENT");
    while (true) {
        if (content.size() < 5) {
            // refill buffer and adjust iterator
            int bytesRead = refillBuffer(content);
            if (bytesRead < 0) {
                std::cerr << "parser error : File input error\n";
                return 1;
            }
            totalBytes += bytesRead;
            if (!inXMLComment && !inCDATA && content.empty())
                break;
        } else if (inTag && content[0] == 'x' && content[1] == 'm' && content[2] == 'l' && content[3] == 'n' && content[4] == 's' && (content[5] == ':' || content[5] == '=')) {
            // parse XML namespace
            content.remove_prefix("xmlns"sv.size());
            std::size_t nameEndPosition = content.find('=');
            if (nameEndPosition == content.npos) {
                std::cerr << "parser error : incomplete namespace\n";
                return 1;
            }
            std::size_t prefixSize = 0;
            if (content.front() == ':') {
                content.remove_prefix(":"sv.size());
                --nameEndPosition;
                prefixSize = nameEndPosition;
            }
            const std::string_view prefix(content.substr(0, prefixSize));
            content.remove_prefix(nameEndPosition + "="sv.size());
            content.remove_prefix(content.find_first_not_of(WHITESPACE));
            if (content.empty()) {
                std::cerr << "parser error : incomplete namespace\n";
                return 1;
            }
            const char delimiter = content.front();
            if (delimiter != '"' && delimiter != '\'') {
                std::cerr << "parser error : incomplete namespace\n";
                return 1;
            }
            content.remove_prefix("\""sv.size());
            std::size_t valueEndPosition = content.find(delimiter);
            if (valueEndPosition == content.npos) {
                std::cerr << "parser error : incomplete namespace\n";
                return 1;
            }
            const std::string_view uri(content.substr(0, valueEndPosition));
            TRACE("NAMESPACE", "prefix", prefix, "uri", uri);
            content.remove_prefix(valueEndPosition + "\""sv.size());
            content.remove_prefix(content.find_first_not_of(WHITESPACE));
            if (content[0] == '>') {
                content.remove_prefix(">"sv.size());
                inTag = false;
                ++depth;
            } else if (content[0] == '/' && content[1] == '>') {
                content.remove_prefix("/>"sv.size());
                TRACE("END TAG", "prefix", inTagPrefix, "qName", inTagQName, "localName", inTagLocalName);
                inTag = false;
            }
        } else if (inTag) {
            // parse attribute
            std::size_t nameEndPosition = std::distance(content.cbegin(), std::find_if_not(content.cbegin(), content.cend(), [] (char c) { return tagNameMask[c]; }));
            if (nameEndPosition == content.size()) {
                std::cerr << "parser error : Empty attribute name" << '\n';
                return 1;
            }
            const std::string_view qName(content.substr(0, nameEndPosition));
            size_t colonPosition = qName.find(':');
            if (colonPosition == 0) {
                std::cerr << "parser error : attribute" << qName  << " starts with a ':'\n";
                return 1;
            }
            if (colonPosition == qName.npos)
                colonPosition = 0;
            const std::string_view prefix(qName.substr(0, colonPosition));
            const std::string_view localName(qName.substr(colonPosition ? colonPosition + 1 : 0));
            content.remove_prefix(nameEndPosition);
            content.remove_prefix(content.find_first_not_of(WHITESPACE));
            if (content.empty()) {
                std::cerr << "parser error : attribute " << qName << " incomplete attribute\n";
                return 1;
            }
            if (content.front() != '=') {
                std::cerr << "parser error : attribute " << qName << " missing =\n";
                return 1;
            }
            content.remove_prefix("="sv.size());
            content.remove_prefix(content.find_first_not_of(WHITESPACE));
            const char delimiter = content.front();
            if (delimiter != '"' && delimiter != '\'') {
                std::cerr << "parser error : attribute " << qName << " missing delimiter\n";
                return 1;
            }
            content.remove_prefix("\""sv.size());
            std::size_t valueEndPosition = content.find(delimiter);
            if (valueEndPosition == content.npos) {
                std::cerr << "parser error : attribute " << qName << " missing delimiter\n";
                return 1;
            }
            const std::string_view value(content.substr(0, valueEndPosition));
            if (localName == "url"sv)
                url = value;
            TRACE("ATTRIBUTE", "prefix", prefix, "qname", qName, "localName", localName, "value", value);
            content.remove_prefix(valueEndPosition + "\""sv.size());
            content.remove_prefix(content.find_first_not_of(WHITESPACE));
            if (content[0] == '>') {
                content.remove_prefix(">"sv.size());
                inTag = false;
                ++depth;
            } else if (content[0] == '/' && content[1] == '>') {
                content.remove_prefix("/>"sv.size());
                TRACE("END TAG", "prefix", inTagPrefix, "qName", inTagQName, "localName", inTagLocalName);
                inTag = false;
            }
        } else if (inXMLComment || (content[1] == '!' && content[0] == '<' && content[2] == '-' && content[3] == '-')) {
            // parse XML comment
            if (content.empty()) {
                std::cerr << "parser error : Unterminated XML comment\n";
                return 1;
            }
            if (!inXMLComment) {
                content.remove_prefix("<!=="sv.size());
            }
            std::size_t tagEndPosition = content.find("-->"sv);
            inXMLComment = tagEndPosition == content.size();
            const std::string_view comment(content.substr(0, tagEndPosition));
            TRACE("COMMENT", "comment", comment);
            content.remove_prefix(tagEndPosition);
            if (!inXMLComment) {
                content.remove_prefix(1 + "-->"sv.size());
            }
        } else if (inCDATA || (content[1] == '!' && content[0] == '<' && content[2] == '[' && content[3] == 'C' && content[4] == 'D' &&
                               content[5] == 'A' && content[6] == 'T' && content[7] == 'A' && content[8] == '[')) {
            // parse CDATA
            if (!inCDATA) {
                content.remove_prefix("<![CDATA["sv.size());
            }
            std::size_t tagEndPosition = content.find("]]>"sv);
            inCDATA = tagEndPosition == content.size();
            const std::string_view characters(content.substr(0, tagEndPosition));
            TRACE("CDATA", "characters", characters);
            textsize += static_cast<int>(characters.size());
            loc += static_cast<int>(std::count(characters.cbegin(), characters.cend(), '\n'));
            content.remove_prefix(tagEndPosition);
            if (!inCDATA) {
                content.remove_prefix("]]>"sv.size() + 1);
            }

        } else if (content[1] == '?' && content[0] == '<' && content[2] == 'x' && content[3] == 'm' && content[4] == 'l' && content[5] == ' ') {
            // parse XML declaration
            std::size_t tagEndPosition = content.find('>');
            if (tagEndPosition == content.npos) {
                int bytesRead = refillBuffer(content);
                if (bytesRead < 0) {
                    std::cerr << "parser error : File input error\n";
                    return 1;
                }
                totalBytes += bytesRead;
                if ((tagEndPosition = content.find('>')) == content.npos) {
                    std::cerr << "parser error: Incomplete XML declaration\n";
                    return 1;
                }
            }
            content.remove_prefix("<?xml"sv.size());
            content.remove_prefix(content.find_first_not_of(WHITESPACE));
            // parse required version
            // if (cursor == tagEnd) {
            //     std::cerr << "parser error: Missing space after before version in XML declaration\n";
            //     return 1;
            // }
            // std::string::const_iterator nameEnd = std::find(cursor, tagEnd, '=');
            std::size_t nameEndPosition = content.find('=');
            const std::string_view attr(content.substr(0, nameEndPosition));
            content.remove_prefix(nameEndPosition + "="sv.size());
            const char delimiter = content.front();
            if (delimiter != '"' && delimiter != '\'') {
                std::cerr << "parser error: Invalid start delimiter for version in XML declaration\n";
                return 1;
            }
            content.remove_prefix("\""sv.size());
            std::size_t valueEndPosition = content.find(delimiter);
            if (valueEndPosition == content.npos) {
                std::cerr << "parser error: Invalid end delimiter for version in XML declaration\n";
                return 1;
            }
            if (attr != "version"sv) {
                std::cerr << "parser error: Missing required first attribute version in XML declaration\n";
                return 1;
            }
            const std::string_view version(content.substr(0, valueEndPosition));
            content.remove_prefix(valueEndPosition + "\""sv.size());
            content.remove_prefix(content.find_first_not_of(WHITESPACE));
            // parse optional encoding and standalone attributes
            std::optional<std::string_view> encoding;
            std::optional<std::string_view> standalone;
            // FIX
            if (true) { //cursor != (tagEnd - 1)) {
                // nameEnd = std::find(cursor, tagEnd, '=');
                std::size_t nameEndPosition = content.find('=');
                if (nameEndPosition == content.npos) {
                    std::cerr << "parser error: Incomplete attribute in XML declaration\n";
                    return 1;
                }
                const std::string_view attr2(content.substr(0, nameEndPosition));
                content.remove_prefix(nameEndPosition + "="sv.size());
                char delimiter2 = content.front();
                if (delimiter2 != '"' && delimiter2 != '\'') {
                    std::cerr << "parser error: Invalid end delimiter for attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                content.remove_prefix("\""sv.size());
                std::size_t valueEndPosition = content.find(delimiter2);
                if (valueEndPosition == content.npos) {
                    std::cerr << "parser error: Incomplete attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                if (attr2 == "encoding"sv) {
                    encoding = std::string_view(content.substr(0, valueEndPosition));
                } else if (attr2 == "standalone"sv) {
                    standalone = std::string_view(content.substr(0, valueEndPosition));
                } else {
                    std::cerr << "parser error: Invalid attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                content.remove_prefix(valueEndPosition + 1);
                content.remove_prefix(content.find_first_not_of(WHITESPACE));
            }
            // FIX
            if (true) { // cursor != (tagEnd - endXMLDecl.size() + 1)) {
                // nameEnd = std::find(cursor, tagEnd, '=');
                std::size_t nameEndPosition = content.substr(0, tagEndPosition).find('=');
                if (nameEndPosition == content.npos) {
                    std::cerr << "parser error: Incomplete attribute in XML declaration\n";
                    return 1;
                }
                const std::string_view attr2(content.substr(0, nameEndPosition));
                content.remove_prefix(nameEndPosition + "="sv.size());
                const char delimiter2 = content.front();
                if (delimiter2 != '"' && delimiter2 != '\'') {
                    std::cerr << "parser error: Invalid end delimiter for attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                content.remove_prefix("\""sv.size());
                // auto valueEnd = std::find(cursor, tagEnd, delimiter2);
                std::size_t valueEndPosition = content.substr(0, tagEndPosition).find(delimiter2);
                if (valueEndPosition == content.npos) {
                    std::cerr << "parser error: Incomplete attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                if (!standalone && attr2 == "standalone"sv) {
                    standalone = std::string_view(content.substr(0, valueEndPosition));
                } else {
                    std::cerr << "parser error: Invalid attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                content.remove_prefix(valueEndPosition + 1);
                content.remove_prefix(content.substr(0, tagEndPosition).find_first_not_of(WHITESPACE));
            }
            TRACE("XML DECLARATION", "version", version, "encoding", (encoding ? *encoding : ""), "standalone", (standalone ? *standalone : ""));
            content.remove_prefix("?>"sv.size());
            content.remove_prefix(content.find_first_not_of(WHITESPACE));

        } else if (content[1] == '?' && content[0] == '<') {
            // parse processing instruction
            std::size_t tagEndPosition = content.find("?>"sv);
            if (tagEndPosition == content.npos) {
                int bytesRead = refillBuffer(content);
                if (bytesRead < 0) {
                    std::cerr << "parser error : File input error\n";
                    return 1;
                }
                totalBytes += bytesRead;
                tagEndPosition = content.find("?>"sv);
                if (tagEndPosition == content.npos) {
                    std::cerr << "parser error: Incomplete XML declaration\n";
                    return 1;
                }
            }
            content.remove_prefix("<?"sv.size());
            std::size_t nameEndPosition = std::distance(content.cbegin(), std::find_if_not(content.cbegin(), content.cend(), [] (char c) { return tagNameMask[c]; }));
            // FIX
            if (nameEndPosition == 0) {
                std::cerr << "parser error : Unterminated processing instruction '" << content.substr(0, nameEndPosition) << "'\n";
                return 1;
            }
            const std::string_view target(content.substr(0, nameEndPosition));
            content.remove_prefix(nameEndPosition);
            const std::string_view data(content.substr(0, tagEndPosition));
            TRACE("PI", "target", target, "data", data);
            content.remove_prefix(tagEndPosition);
            content.remove_prefix("?>"sv.size());
        } else if (content[1] == '/' && content[0] == '<') {
            // parse end tag
            content.remove_prefix("</"sv.size());
            if (content.size() < 100) {
                if (content.find('>') == content.npos) {
                    int bytesRead = refillBuffer(content);
                    if (bytesRead < 0) {
                        std::cerr << "parser error : File input error\n";
                        return 1;
                    }
                    totalBytes += bytesRead;
                    // @TODO start search after initial sed
                    if (content.substr(0, bytesRead).find('>') == content.npos) {
                        std::cerr << "parser error: Incomplete element end tag\n";
                        return 1;
                    }
                }
            }
            if (content.front() == ':') {
                std::cerr << "parser error : Invalid end tag name\n";
                return 1;
            }
            std::size_t nameEndPosition = std::distance(content.cbegin(), std::find_if_not(content.cbegin(), content.cend(), [] (char c) { return tagNameMask[c]; }));
            if (nameEndPosition == content.size()) {
                std::cerr << "parser error : Unterminated end tag '" << content.substr(0, nameEndPosition) << "'\n";
                return 1;
            }
            const std::string_view qName(content.substr(0, nameEndPosition));
            if (qName.empty()) {
                std::cerr << "parser error: EndTag: invalid element name\n";
                return 1;
            }
            size_t colonPosition = qName.find(':');
            if (colonPosition == qName.npos) {
                colonPosition = 0;
            }
            const std::string_view prefix(qName.substr(0, colonPosition));
            const std::string_view localName(qName.substr(colonPosition ? colonPosition + 1 : 0));
            content.remove_prefix(nameEndPosition + ">"sv.size());
            --depth;
            TRACE("END TAG", "prefix", prefix, "qName", qName, "localName", localName);
        } else if (content.front() == '<') {
            // parse start tag
            content.remove_prefix("<"sv.size());
            if (content.size() < 200) {
                if (std::none_of(content.cbegin(), content.cend(), [] (char c) { return c =='>'; })) {
                    int bytesRead = refillBuffer(content);
                    if (bytesRead < 0) {
                        std::cerr << "parser error : File input error\n";
                        return 1;
                    }
                    totalBytes += bytesRead;
                    // @TODO start search after initial sed
                    if (std::none_of(content.cbegin(), content.cend(), [] (char c) { return c =='>'; })) {
                        std::cerr << "parser error: Incomplete element start tag\n";
                        return 1;
                    }
                }
            }
            if (content.front() == ':') {
                std::cerr << "parser error : Invalid start tag name\n";
                return 1;
            }
            std::size_t nameEndPosition = std::distance(content.cbegin(), std::find_if_not(content.cbegin(), content.cend(), [] (char c) { return tagNameMask[c]; }));
            if (nameEndPosition == content.size()) {
                std::cerr << "parser error : Unterminated start tag '" << content.substr(0, nameEndPosition) << "'\n";
                return 1;
            }
            size_t colonPosition = 0;
            if (content[nameEndPosition] == ':') {
                colonPosition = nameEndPosition;
                nameEndPosition = std::distance(content.cbegin(), std::find_if_not(content.cbegin() + nameEndPosition + 1, content.cend(), [] (char c) { return tagNameMask[c]; }));
            }
            const std::string_view qName(content.substr(0, nameEndPosition));
            if (qName.empty()) {
                std::cerr << "parser error: StartTag: invalid element name\n";
                return 1;
            }
            const std::string_view prefix(qName.substr(0, colonPosition));
            const std::string_view localName(qName.substr(colonPosition ? colonPosition + 1 : 0, nameEndPosition));
            TRACE("START TAG", "prefix", prefix, "qName", qName, "localName", localName);
            if (localName == "expr"sv) {
                ++exprCount;
            } else if (localName == "decl"sv) {
                ++declCount;
            } else if (localName == "comment"sv) {
                ++commentCount;
            } else if (localName == "function"sv) {
                ++functionCount;
            } else if (localName == "unit"sv) {
                ++unitCount;
                if (depth == 1)
                    isArchive = true;
            } else if (localName == "class"sv) {
                ++classCount;
            }
            content.remove_prefix(nameEndPosition);
            content.remove_prefix(content.find_first_not_of(WHITESPACE));
            if (content.front() == '>') {
                content.remove_prefix(">"sv.size());
                ++depth;
            } else if (content[0] == '/' && content[1] == '>') {
                content.remove_prefix("/>"sv.size());
                TRACE("END TAG", "prefix", prefix, "qName", qName, "localName", localName);
            } else {
                inTagQName = qName;
                inTagPrefix = inTagQName.substr(0, prefix.size());
                inTagLocalName = inTagQName.substr(prefix.size() + 1);
                inTag = true;
            }
        } else if (depth == 0) {
            // ignore whitespace before or after XML
            content.remove_prefix(content.find_first_not_of(WHITESPACE));
        } else if (content.front() == '&') {
            // parse character entity references
            std::string_view reference;
            if (content[1] == 'l' && content[2] == 't' && content[3] == ';') {
                reference = "<";
                content.remove_prefix("&lt;"sv.size());
            } else if (content[1] == 'g' && content[2] == 't' && content[3] == ';') {
                reference = ">";
                content.remove_prefix("&gt;"sv.size());
            } else if (content[1] == 'a' && content[2] == 'm' && content[3] == 'p' && content[4] == ';') {
                reference = "&";
                content.remove_prefix("&amp;"sv.size());
            } else {
                reference = "&";
                content.remove_prefix("&"sv.size());
            }
            const std::string_view characters(reference);
            TRACE("ENTITYREF", "characters", characters);
            ++textsize;

        } else {
            // parse character non-entity references
            std::size_t tagEndPosition = content.find_first_of("<&");
            const std::string_view characters(content.substr(0, tagEndPosition));
            TRACE("CHARACTERS", "characters", characters);
            loc += static_cast<int>(std::count(characters.cbegin(), characters.cend(), '\n'));
            textsize += static_cast<int>(characters.size());
            content.remove_prefix(characters.size());
        }
    }
    TRACE("END DOCUMENT");
    const auto finish = std::chrono::steady_clock::now();
    const auto elapsed_seconds = std::chrono::duration_cast<std::chrono::duration<double> >(finish - start).count();
    const double mlocPerSec = loc / elapsed_seconds / 1000000;
    int files = unitCount;
    if (isArchive)
        --files;
    std::cout.imbue(std::locale{""});
    int valueWidth = std::max(5, static_cast<int>(log10(totalBytes) * 1.3 + 1));
    std::cout << "# srcFacts: " << url << '\n';
    std::cout << "| Measure      | " << std::setw(valueWidth + 3) << "Value |\n";
    std::cout << "|:-------------|-" << std::setw(valueWidth + 3) << std::setfill('-') << ":|\n" << std::setfill(' ');
    std::cout << "| Characters   | " << std::setw(valueWidth) << textsize      << " |\n";
    std::cout << "| LOC          | " << std::setw(valueWidth) << loc           << " |\n";
    std::cout << "| Files        | " << std::setw(valueWidth) << files         << " |\n";
    std::cout << "| Classes      | " << std::setw(valueWidth) << classCount    << " |\n";
    std::cout << "| Functions    | " << std::setw(valueWidth) << functionCount << " |\n";
    std::cout << "| Declarations | " << std::setw(valueWidth) << declCount     << " |\n";
    std::cout << "| Expressions  | " << std::setw(valueWidth) << exprCount     << " |\n";
    std::cout << "| Comments     | " << std::setw(valueWidth) << commentCount  << " |\n";
    std::clog.imbue(std::locale{""});
    std::clog.precision(3);
    std::clog << '\n';
    std::clog << totalBytes  << " bytes\n";
    std::clog << elapsed_seconds << " sec\n";
    std::clog << mlocPerSec << " MLOC/sec\n";
    return 0;
}
