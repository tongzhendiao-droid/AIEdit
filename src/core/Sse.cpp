#include "core/Sse.h"

namespace aiedit::core {

std::vector<SseEvent> SseParser::feed(std::string_view chunk)
{
    std::vector<SseEvent> events;
    for (const char c : chunk) {
        // Lines end with "\r\n", "\n" or "\r"; a "\r\n" may be split across two chunks.
        if (m_skipLineFeed) {
            m_skipLineFeed = false;
            if (c == '\n')
                continue;
        }
        if (c == '\r' || c == '\n') {
            processLine(m_buffer, events);
            m_buffer.clear();
            m_skipLineFeed = c == '\r';
        } else {
            m_buffer += c;
        }
    }
    return events;
}

std::vector<SseEvent> SseParser::finish()
{
    std::vector<SseEvent> events;
    if (!m_buffer.empty()) {
        processLine(m_buffer, events);
        m_buffer.clear();
    }
    dispatch(events);
    return events;
}

void SseParser::processLine(std::string_view line, std::vector<SseEvent>& events)
{
    if (line.empty()) {
        dispatch(events);
        return;
    }
    if (line.front() == ':') // a comment, e.g. ": keep-alive"
        return;

    const auto colon = line.find(':');
    const std::string_view field = line.substr(0, colon);
    std::string_view value = colon == std::string_view::npos ? std::string_view{} : line.substr(colon + 1);
    if (!value.empty() && value.front() == ' ')
        value.remove_prefix(1);

    if (field == "data") {
        if (m_hasData)
            m_data += '\n';
        m_data += value;
        m_hasData = true;
    } else if (field == "event") {
        m_eventType = value;
    }
    // "id" and "retry" do not matter for one-shot streams; unknown fields are ignored.
}

void SseParser::dispatch(std::vector<SseEvent>& events)
{
    if (m_hasData)
        events.push_back({m_eventType.empty() ? "message" : m_eventType, m_data});
    m_eventType.clear();
    m_data.clear();
    m_hasData = false;
}

} // namespace aiedit::core
