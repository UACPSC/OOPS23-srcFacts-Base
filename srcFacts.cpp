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
#include <ctype.h>
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

/*
    Refill the buffer preserving the unused data.
    Current content [cursor, cursorEnd) is shifted left and new data
    appended to the rest of the buffer.

    @param[in,out] cursor Iterator to current position in buffer
    @param[in, out] cursorEnd Iterator to end of buffer for this read
    @param[in, out] buffer Container for characters
    @return Number of bytes read
    @retval 0 EOF
    @retval -1 Read error
*/
int refillBuffer(std::string::const_iterator& cursor, std::string::const_iterator& cursorEnd, std::string& buffer) {

    // number of unprocessed characters [cursor, cursorEnd)
    size_t unprocessed = std::distance(cursor, cursorEnd);

    // move unprocessed characters, [cursor, cursorEnd), to start of the buffer
    std::copy(cursor, cursorEnd, buffer.begin());

    // reset cursors
    cursor = buffer.begin();
    cursorEnd = cursor + unprocessed;

    // read in whole blocks
    ssize_t readBytes = 0;
    while (((readBytes = READ(0, static_cast<void*>(buffer.data() + unprocessed),
        std::distance(cursorEnd, buffer.cend()))) == -1) && (errno == EINTR)) {
    }
    if (readBytes == -1)
        // error in read
        return -1;
    if (readBytes == 0) {
        // EOF
        cursor = buffer.cend();
        cursorEnd = buffer.cend();
        return 0;
    }

    // adjust the end of the cursor to the new bytes
    cursorEnd += readBytes;

    return readBytes;
}

// trace parsing
#ifdef TRACE
#undef TRACE
#define HEADER(m) std::clog << std::setw(10) << std::left << m <<"\t"
#define FIELD(l, n) l << ":|" << n << "| "
#define FIELD2(l1, n1, l2, n2) FIELD(l1,n1) << FIELD(l2,n2)
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
    std::string buffer(BUFFER_SIZE, ' ');
    std::string::const_iterator cursor = buffer.cend();
    std::string::const_iterator cursorEnd = buffer.cend();
    std::string_view contents;
    TRACE("START DOCUMENT");
    std::string lastContents;
    std::string lastCursor;
    while (true) {

        if (contents.size() != std::distance(cursor, cursorEnd)) {
            fprintf(stderr, "DEBUG:  %s %s %d std::distance(cursor, cursorEnd): %d\n", __FILE__,  __FUNCTION__, __LINE__,  (int) std::distance(cursor, cursorEnd));
            exit(1);
        }
        if (contents.substr(0, 100) != std::string_view(&cursor[0], std::min(100l, std::distance(cursor, cursorEnd)))) {
            fprintf(stderr, "DEBUG:  %s %s %d std::string_view(cursor, std::distance(cursor, cursorEnd)): %s\n", __FILE__,  __FUNCTION__, __LINE__,  std::string_view(&cursor[0], std::distance(cursor, cursorEnd)).data());
            fprintf(stderr, "DEBUG:  %s %s %d contents: %s\n", __FILE__,  __FUNCTION__, __LINE__,  contents.data());
            exit(1);
        }
        if (std::distance(cursor, cursorEnd) < 5) {
            // refill buffer and adjust iterator
            int bytesRead = refillBuffer(cursor, cursorEnd, buffer);
            if (bytesRead < 0) {
                std::cerr << "parser error : File input error\n";
                return 1;
            }
            totalBytes += bytesRead;
            if (!inXMLComment && !inCDATA && cursor == cursorEnd)
                break;
            contents = std::string_view(&cursor[0], std::distance(cursor, cursorEnd));
        } else if (inTag && contents[0] == 'x' && contents[1] == 'm' && contents[2] == 'l' && contents[3] == 'n' && contents[4] == 's' && (cursor[5] == ':' || contents[5] == '=')) {
            // parse XML namespace
            contents.remove_prefix(5);
            std::advance(cursor, 5);
            const std::string::const_iterator nameEnd = std::find(cursor, cursorEnd, '=');
            int nameEndPos = contents.find('=');
            if (nameEnd == cursorEnd) {
                std::cerr << "parser error : incomplete namespace\n";
                return 1;
            }
            int prefixSize = 0;
            if (*cursor == ':') {
                contents.remove_prefix(1);
                std::advance(cursor, 1);
                prefixSize = nameEndPos;
                // prefixSize = std::distance(cursor, nameEnd);
            }
            const std::string_view prefix(std::addressof(*cursor), prefixSize);
            contents.remove_prefix(std::distance(cursor, nameEnd) + 1);
            cursor = std::next(nameEnd);
            contents.remove_prefix(contents.find_first_not_of(" \n\t\r"sv));
            // contents.remove_prefix(std::distance(cursor, std::find_if_not(cursor, cursorEnd, isspace)));
            cursor = std::find_if_not(cursor, cursorEnd, isspace);
            if (cursor == cursorEnd) {
                std::cerr << "parser error : incomplete namespace\n";
                return 1;
            }
            const char delimiter = *cursor;
            if (delimiter != '"' && delimiter != '\'') {
                std::cerr << "parser error : incomplete namespace\n";
                return 1;
            }
            contents.remove_prefix(1);
            std::advance(cursor, 1);
            const std::string::const_iterator valueEnd = std::find(cursor, cursorEnd, delimiter);
            if (valueEnd == cursorEnd) {
                std::cerr << "parser error : incomplete namespace\n";
                return 1;
            }
            const std::string_view uri(std::addressof(*cursor), std::distance(cursor, valueEnd));
            TRACE("NAMESPACE", "prefix", prefix, "uri", uri);
            contents.remove_prefix(std::distance(cursor, valueEnd) + 1);
            cursor = std::next(valueEnd);
            contents.remove_prefix(contents.find_first_not_of(" \n\t\r"sv));
            // contents.remove_prefix(std::distance(cursor, std::find_if_not(cursor, cursorEnd, isspace)));
            cursor = std::find_if_not(cursor, cursorEnd, isspace);
            if (*cursor == '>') {
                contents.remove_prefix(1);
                std::advance(cursor, 1);
                inTag = false;
                ++depth;
            } else if (*cursor == '/' && contents[1] == '>') {
                contents.remove_prefix(2);
                std::advance(cursor, 2);
                TRACE("END TAG", "prefix", inTagPrefix, "qName", inTagQName, "localName", inTagLocalName);
                inTag = false;
            }
        } else if (inTag) {
            // parse attribute
            const std::string::const_iterator nameEnd = std::find_if_not(cursor, cursorEnd, [] (char c) { return tagNameMask[c]; });
            if (nameEnd == cursorEnd) {
                std::cerr << "parser error : Empty attribute name" << '\n';
                return 1;
            }
            const std::string_view qName(std::addressof(*cursor), std::distance(cursor, nameEnd));
            size_t colonPosition = qName.find(':');
            if (colonPosition == 0) {
                std::cerr << "parser error : Invalid attribute name " << qName << '\n';
                return 1;
            }
            if (colonPosition == std::string::npos)
                colonPosition = 0;
            const std::string_view prefix(std::addressof(*qName.cbegin()), colonPosition);
            if (colonPosition != 0)
                colonPosition += 1;
            const std::string_view localName(std::addressof(*qName.cbegin()) + colonPosition, qName.size() - colonPosition);
            contents.remove_prefix(std::distance(cursor, nameEnd));
            cursor = nameEnd;
            if (isspace(*cursor)) {
                contents.remove_prefix(contents.find_first_not_of(" \n\t\r"sv));
                // contents.remove_prefix(std::distance(cursor, std::find_if_not(cursor, cursorEnd, isspace)));
                cursor = std::find_if_not(cursor, cursorEnd, isspace);
            }
            if (cursor == cursorEnd) {
                std::cerr << "parser error : attribute " << qName << " incomplete attribute\n";
                return 1;
            }
            if (*cursor != '=') {
                std::cerr << "parser error : attribute " << qName << " missing =\n";
                return 1;
            }
            contents.remove_prefix(1);
            std::advance(cursor, 1);
            if (isspace(*cursor)) {
                contents.remove_prefix(contents.find_first_not_of(" \n\t\r"sv));
                // contents.remove_prefix(std::distance(cursor, std::find_if_not(cursor, cursorEnd, isspace)));
                cursor = std::find_if_not(cursor, cursorEnd, isspace);
            }
            const char delimiter = *cursor;
            if (delimiter != '"' && delimiter != '\'') {
                std::cerr << "parser error : attribute " << qName << " missing delimiter\n";
                return 1;
            }
            contents.remove_prefix(1);
            std::advance(cursor, 1);
            std::string::const_iterator valueEnd = std::find(cursor, cursorEnd, delimiter);
            if (valueEnd == cursorEnd) {
                std::cerr << "parser error : attribute " << qName << " missing delimiter\n";
                return 1;
            }
            const std::string_view value(std::addressof(*cursor), std::distance(cursor, valueEnd));
            if (localName == "url"sv)
                url = value;
            TRACE("ATTRIBUTE", "prefix", prefix, "qname", qName, "localName", localName, "value", value);
            contents.remove_prefix(std::distance(cursor, valueEnd) + 1);
            cursor = std::next(valueEnd);
            if (isspace(*cursor)) {
                contents.remove_prefix(contents.find_first_not_of(" \n\t\r"sv));
                // contents.remove_prefix(std::distance(cursor, std::find_if_not(cursor, cursorEnd, isspace)));
                cursor = std::find_if_not(cursor, cursorEnd, isspace);
            }
            if (*cursor == '>') {
                contents.remove_prefix(1);
                std::advance(cursor, 1);
                inTag = false;
                ++depth;
            } else if (*cursor == '/' && contents[1] == '>') {
                contents.remove_prefix(2);
                std::advance(cursor, 2);
                TRACE("END TAG", "prefix", inTagPrefix, "qName", inTagQName, "localName", inTagLocalName);
                inTag = false;
            }
        } else if (inXMLComment || (cursor[1] == '!' && contents[0] == '<' && contents[2] == '-' && contents[3] == '-')) {
            // parse XML comment
            if (cursor == cursorEnd) {
                std::cerr << "parser error : Unterminated XML comment\n";
                return 1;
            }
            if (!inXMLComment) {
                contents.remove_prefix(4);
                std::advance(cursor, 4);
            }
            constexpr std::string_view endComment = "-->"sv;
            int position = std::string_view(&*cursor, std::distance(cursor, cursorEnd)).find(endComment);
            std::string::const_iterator tagEnd = cursor + position;
            inXMLComment = tagEnd == cursorEnd;
            const std::string_view comment(std::addressof(*cursor), std::distance(cursor, tagEnd));
            TRACE("COMMENT", "comment", comment);
            if (!inXMLComment) {
                contents.remove_prefix(std::distance(cursor, tagEnd) + endComment.size());
                cursor = std::next(tagEnd, endComment.size());
            } else {
                contents.remove_prefix(std::distance(cursor, tagEnd));
                cursor = tagEnd;
            }
        } else if (inCDATA || (cursor[1] == '!' && contents[0] == '<' && contents[2] == '[' && contents[3] == 'C' && contents[4] == 'D'
            && contents[5] == 'A' && contents[6] == 'T' && contents[7] == 'A' && contents[8] == '[')) {
            // parse CDATA
            if (cursor == cursorEnd) {
                std::cerr << "parser error : Unterminated CDATA\n";
                return 1;
            }
            constexpr std::string_view endCDATA = "]]>"sv;
            if (!inCDATA) {
                contents.remove_prefix(9);
                std::advance(cursor, 9);
            }
            int position = std::string_view(&*cursor, std::distance(cursor, cursorEnd)).find(endCDATA);
            std::string::const_iterator tagEnd = cursor + position;
            inCDATA = tagEnd == cursorEnd;
            const std::string_view characters(std::addressof(*cursor), std::distance(cursor, tagEnd));
            TRACE("CDATA", "characters", characters);
            textsize += static_cast<int>(characters.size());
            loc += static_cast<int>(std::count(characters.begin(), characters.end(), '\n'));
            contents.remove_prefix(std::distance(cursor, tagEnd) + endCDATA.size());
            cursor = std::next(tagEnd, endCDATA.size());
            // if (!inCDATA)
            //     cursor = std::next(tagEnd, endCDATA.size());
            // else
            //     cursor = tagEnd;
        } else if (cursor[1] == '?' && contents[0] == '<' && contents[1] == '?' && contents[2] == 'x' && contents[3] == 'm' && contents[4] == 'l' && contents[5] == ' ') {
            // parse XML declaration
            constexpr std::string_view startXMLDecl = "<?xml";
            constexpr std::string_view endXMLDecl = "?>";
            std::string::const_iterator tagEnd = std::find(cursor, cursorEnd, '>');
            if (tagEnd == cursorEnd) {
                int bytesRead = refillBuffer(cursor, cursorEnd, buffer);
                if (bytesRead < 0) {
                    std::cerr << "parser error : File input error\n";
                    return 1;
                }
                totalBytes += bytesRead;
                if ((tagEnd = std::find(cursor, cursorEnd, '>')) == cursorEnd) {
                    std::cerr << "parser error: Incomplete XML declaration\n";
                    return 1;
                }
                contents = std::string_view(&cursor[0], std::distance(cursor, cursorEnd));
            }
            contents.remove_prefix(startXMLDecl.size());
            std::advance(cursor, startXMLDecl.size());
            if (isspace(*cursor)) {
                contents.remove_prefix(std::distance(cursor, std::find_if_not(cursor, tagEnd, isspace)));
                cursor = std::find_if_not(cursor, tagEnd, isspace);
            }
            // cursor = std::find_if_not(cursor, tagEnd, isspace);
            // parse required version
            if (cursor == tagEnd) {
                std::cerr << "parser error: Missing space after before version in XML declaration\n";
                return 1;
            }
            std::string::const_iterator nameEnd = std::find(cursor, tagEnd, '=');
            const std::string_view attr(std::addressof(*cursor), std::distance(cursor, nameEnd));
            contents.remove_prefix(std::distance(cursor, nameEnd) + 1);
            cursor = std::next(nameEnd);
            const char delimiter = *cursor;
            if (delimiter != '"' && delimiter != '\'') {
                std::cerr << "parser error: Invalid start delimiter for version in XML declaration\n";
                return 1;
            }
            contents.remove_prefix(1);
            std::advance(cursor, 1);
            std::string::const_iterator valueEnd = std::find(cursor, tagEnd, delimiter);
            if (valueEnd == tagEnd) {
                std::cerr << "parser error: Invalid end delimiter for version in XML declaration\n";
                return 1;
            }
            if (attr != "version"sv) {
                std::cerr << "parser error: Missing required first attribute version in XML declaration\n";
                return 1;
            }
            const std::string_view version(std::addressof(*cursor), std::distance(cursor, valueEnd));
            contents.remove_prefix(std::distance(cursor, valueEnd) + 1);
            cursor = std::next(valueEnd);
            if (isspace(*cursor)) {
                contents.remove_prefix(std::distance(cursor, std::find_if_not(cursor, tagEnd, isspace)));
                cursor = std::find_if_not(cursor, tagEnd, isspace);
            }
            // parse optional encoding and standalone attributes
            std::optional<std::string_view> encoding;
            std::optional<std::string_view> standalone;
            if (cursor != (tagEnd - 1)) {
                nameEnd = std::find(cursor, tagEnd, '=');
                if (nameEnd == tagEnd) {
                    std::cerr << "parser error: Incomplete attribute in XML declaration\n";
                    return 1;
                }
                const std::string_view attr2(std::addressof(*cursor), std::distance(cursor, nameEnd));
                contents.remove_prefix(std::distance(cursor, nameEnd) + 1);
                cursor = std::next(nameEnd);
                char delimiter2 = *cursor;
                if (delimiter2 != '"' && delimiter2 != '\'') {
                    std::cerr << "parser error: Invalid end delimiter for attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                contents.remove_prefix(1);
                std::advance(cursor, 1);
                valueEnd = std::find(cursor, tagEnd, delimiter2);
                if (valueEnd == tagEnd) {
                    std::cerr << "parser error: Incomplete attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                if (attr2 == "encoding"sv) {
                    encoding = std::string_view(std::addressof(*cursor), std::distance(cursor, valueEnd));
                } else if (attr2 == "standalone"sv) {
                    standalone = std::string_view(std::addressof(*cursor), std::distance(cursor, valueEnd));
                } else {
                    std::cerr << "parser error: Invalid attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                contents.remove_prefix(std::distance(cursor, valueEnd) + 1);
                cursor = std::next(valueEnd);
                contents.remove_prefix(std::distance(cursor, std::find_if_not(cursor, tagEnd, isspace)));
                cursor = std::find_if_not(cursor, tagEnd, isspace);
            }
            if (cursor != (tagEnd - endXMLDecl.size() + 1)) {
                nameEnd = std::find(cursor, tagEnd, '=');
                if (nameEnd == tagEnd) {
                    std::cerr << "parser error: Incomplete attribute in XML declaration\n";
                    return 1;
                }
                const std::string_view attr2(std::addressof(*cursor), std::distance(cursor, nameEnd));
                contents.remove_prefix(std::distance(cursor, nameEnd) + 1);
                cursor = std::next(nameEnd);
                const char delimiter2 = *cursor;
                if (delimiter2 != '"' && delimiter2 != '\'') {
                    std::cerr << "parser error: Invalid end delimiter for attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                contents.remove_prefix(1);
                std::advance(cursor, 1);
                valueEnd = std::find(cursor, tagEnd, delimiter2);
                if (valueEnd == tagEnd) {
                    std::cerr << "parser error: Incomplete attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                if (!standalone && attr2 == "standalone"sv) {
                    standalone = std::string_view(std::addressof(*cursor), std::distance(cursor, valueEnd));
                } else {
                    std::cerr << "parser error: Invalid attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                contents.remove_prefix(std::distance(cursor, valueEnd) + 1);
                cursor = std::next(valueEnd);
                if (isspace(*cursor)) {
                    contents.remove_prefix(std::distance(cursor, std::find_if_not(cursor, tagEnd, isspace)));
                    cursor = std::find_if_not(cursor, tagEnd, isspace);
                }
//                cursor = std::find_if_not(cursor, tagEnd, isspace);
            }
            TRACE("XML DECLARATION", "version", version, "encoding", (encoding ? *encoding : ""), "standalone", (standalone ? *standalone : ""));
            contents.remove_prefix(endXMLDecl.size());
            std::advance(cursor, endXMLDecl.size());
            contents.remove_prefix(contents.find_first_not_of(" \n\t\r"sv));
            // contents.remove_prefix(std::distance(cursor, std::find_if_not(cursor, cursorEnd, isspace)));
            cursor = std::find_if_not(cursor, cursorEnd, isspace);

        } else if (cursor[1] == '?' && contents[0] == '<') {
            // parse processing instruction
            int position = std::string_view(&*cursor, std::distance(cursor, cursorEnd)).find("?>"sv);
            if (position == std::string_view::npos) {
                int bytesRead = refillBuffer(cursor, cursorEnd, buffer);
                if (bytesRead < 0) {
                    std::cerr << "parser error : File input error\n";
                    return 1;
                }
                contents = std::string_view(&cursor[0], std::distance(cursor, cursorEnd));
                totalBytes += bytesRead;
                position = std::string_view(&*cursor, std::distance(cursor, cursorEnd)).find("?>"sv);
                if (position == std::string_view::npos) {
                    std::cerr << "parser error: Incomplete XML declaration\n";
                    return 1;
                }
            }
            std::string::const_iterator tagEnd = cursor + position;
            contents.remove_prefix(2);
            std::advance(cursor, 2);
            std::string::const_iterator nameEnd = std::find_if_not(cursor, tagEnd, [] (char c) { return tagNameMask[c]; });
            if (nameEnd == tagEnd) {
                std::cerr << "parser error : Unterminated processing instruction '" << std::string_view(std::addressof(*cursor), std::distance(cursor, nameEnd)) << "'\n";
                return 1;
            }
            const std::string_view target(std::addressof(*cursor), std::distance(cursor, nameEnd));
            contents.remove_prefix(std::distance(cursor, std::find_if_not(cursor, tagEnd, isspace)));
            cursor = std::find_if_not(nameEnd, tagEnd, isspace);
            const std::string_view data(std::addressof(*cursor), std::distance(cursor, tagEnd));
            TRACE("PI", "target", target, "data", data);
            contents.remove_prefix(std::distance(cursor, tagEnd));
            cursor = tagEnd;
            contents.remove_prefix(2);
            std::advance(cursor, 2);
        } else if (cursor[1] == '/' && contents[0] == '<') {
            // parse end tag
            if (contents.size() < 100) {
                if (std::none_of(cursor, cursorEnd, [] (char c) { return c =='>'; })) {
                    int bytesRead = refillBuffer(cursor, cursorEnd, buffer);
                    if (bytesRead < 0) {
                        std::cerr << "parser error : File input error\n";
                        return 1;
                    }
                    totalBytes += bytesRead;
                    // @TODO start search after initial sed
                    if (std::none_of(cursor, cursorEnd, [] (char c) { return c =='>'; })) {
                        std::cerr << "parser error: Incomplete element end tag\n";
                        return 1;
                    }
                    contents = std::string_view(&cursor[0], std::distance(cursor, cursorEnd));
                }
            }
            contents.remove_prefix(2);
            std::advance(cursor, 2);
            if (contents.front() == ':') {
                std::cerr << "parser error : Invalid end tag name\n";
                return 1;
            }
            int nameEndPos = std::distance(&contents[0], std::find_if_not(contents.cbegin(), contents.cend(), [] (char c) { return tagNameMask[c]; }));
            if (nameEndPos == contents.size()) {
                std::cerr << "parser error : Unterminated end tag '" << contents.substr(0, nameEndPos) << "'\n";
                return 1;
            }
            const std::string_view qName(contents.substr(0, nameEndPos));
            if (qName.empty()) {
                std::cerr << "parser error: EndTag: invalid element name\n";
                return 1;
            }
            size_t colonPosition = qName.find(':');
            if (colonPosition == std::string::npos) {
                colonPosition = 0;
            }
            const std::string_view prefix(qName.substr(0, colonPosition));
            const std::string_view localName(qName.substr(colonPosition + 1));
            contents.remove_prefix(nameEndPos + 1);
            cursor += nameEndPos + 1;
            --depth;
            TRACE("END TAG", "prefix", prefix, "qName", qName, "localName", localName);
        } else if (*cursor == '<') {
            // parse start tag
            if (contents.size() < 200) {
                if (std::none_of(cursor, cursorEnd, [] (char c) { return c =='>'; })) {
                    int bytesRead = refillBuffer(cursor, cursorEnd, buffer);
                    if (bytesRead < 0) {
                        std::cerr << "parser error : File input error\n";
                        return 1;
                    }
                    totalBytes += bytesRead;
                    // @TODO start search after initial sed
                    if (std::none_of(cursor, cursorEnd, [] (char c) { return c =='>'; })) {
                        std::cerr << "parser error: Incomplete element start tag\n";
                        return 1;
                    }
                    contents = std::string_view(&cursor[0], std::distance(cursor, cursorEnd));
                }
            }
            contents.remove_prefix(1);
            std::advance(cursor, 1);
            if (contents.front() == ':') {
                std::cerr << "parser error : Invalid start tag name\n";
                return 1;
            }
            int nameEndPos = std::distance(&contents[0], std::find_if_not(contents.cbegin(), contents.cend(), [] (char c) { return tagNameMask[c]; }));
            if (nameEndPos == contents.size()) {
                std::cerr << "parser error : Unterminated start tag '" << contents.substr(0, nameEndPos) << "'\n";
                return 1;
            }
            size_t colonPosition = 0;
            if (contents[nameEndPos] == ':') {
                colonPosition = nameEndPos;
                nameEndPos = std::distance(&contents[0], std::find_if_not(contents.cbegin() + nameEndPos + 1, contents.cend(), [] (char c) { return tagNameMask[c]; }));
            }
            const std::string_view qName(contents.substr(0, nameEndPos));
            if (qName.empty()) {
                std::cerr << "parser error: StartTag: invalid element name\n";
                return 1;
            }
            const std::string_view prefix(contents.substr(0, colonPosition));
            if (colonPosition)
                ++colonPosition;
            const std::string_view localName(contents.substr(colonPosition, nameEndPos));
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
            contents.remove_prefix(nameEndPos);
            cursor += nameEndPos;
            if (*cursor != '>') {
                contents.remove_prefix(contents.find_first_not_of(" \n\t\r"sv));
                // contents.remove_prefix(std::distance(cursor, std::find_if_not(cursor, cursorEnd, isspace)));
                cursor = std::find_if_not(cursor, cursorEnd, isspace);
            }
            if (*cursor == '>') {
                contents.remove_prefix(1);
                std::advance(cursor, 1);
                ++depth;
            } else if (*cursor == '/' && contents[1] == '>') {
                contents.remove_prefix(2);
                std::advance(cursor, 2);
                TRACE("END TAG", "prefix", prefix, "qName", qName, "localName", localName);
            } else {
                inTagQName = qName;
                inTagPrefix = std::string_view(inTagQName.data(), prefix.size());
                inTagLocalName = std::string_view(inTagQName.data() + prefix.size() + 1);
                inTag = true;
            }
        } else if (depth == 0) {
            // parse characters before or after XML
            contents.remove_prefix(contents.find_first_not_of(" \n\t\r"sv));
            // contents.remove_prefix(std::distance(cursor, std::find_if_not(cursor, cursorEnd, isspace)));
            cursor = std::find_if_not(cursor, cursorEnd, isspace);
        } else if (*cursor == '&') {
            // parse character entity references
            std::string_view characters;
            if (cursor[1] == 'l' && contents[2] == 't' && contents[3] == ';') {
                characters = "<";
                contents.remove_prefix(4);
                std::advance(cursor, 4);
            } else if (cursor[1] == 'g' && contents[2] == 't' && contents[3] == ';') {
                characters = ">";
                contents.remove_prefix(4);
                std::advance(cursor, 4);
            } else if (cursor[1] == 'a' && contents[2] == 'm' && contents[3] == 'p' && contents[4] == ';') {
                characters = "&";
                contents.remove_prefix(5);
                std::advance(cursor, 5);
            } else {
                characters = "&";
                contents.remove_prefix(1);
                std::advance(cursor, 1);
            }
            TRACE("ENTITYREF", "characters", characters);
            ++textsize;

        } else {
            // parse character non-entity references
            const std::string::const_iterator tagEnd = std::find_if(cursor, cursorEnd, [] (char c) { return c == '<' || c == '&'; });
            const std::string_view characters(std::addressof(*cursor), std::distance(cursor, tagEnd));
            TRACE("CHARACTERS", "characters", characters);
            loc += static_cast<int>(std::count(characters.cbegin(), characters.cend(), '\n'));
            textsize += static_cast<int>(characters.size());
            contents.remove_prefix(characters.size());
            std::advance(cursor, characters.size());
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
    std::cout << "| srcML bytes  | " << std::setw(valueWidth) << totalBytes          << " |\n";
    std::cout << "| Characters   | " << std::setw(valueWidth) << textsize       << " |\n";
    std::cout << "| Files        | " << std::setw(valueWidth) << files          << " |\n";
    std::cout << "| LOC          | " << std::setw(valueWidth) << loc            << " |\n";
    std::cout << "| Classes      | " << std::setw(valueWidth) << classCount    << " |\n";
    std::cout << "| Functions    | " << std::setw(valueWidth) << functionCount << " |\n";
    std::cout << "| Declarations | " << std::setw(valueWidth) << declCount     << " |\n";
    std::cout << "| Expressions  | " << std::setw(valueWidth) << exprCount     << " |\n";
    std::cout << "| Comments     | " << std::setw(valueWidth) << commentCount  << " |\n";
    std::clog << '\n';
    std::clog << std::setprecision(3) << elapsed_seconds << " sec\n";
    std::clog << std::setprecision(3) << mlocPerSec << " MLOC/sec\n";
    return 0;
}
