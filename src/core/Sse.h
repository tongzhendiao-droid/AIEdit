#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace aiedit::core {

// One server-sent event: its type ("message" unless an "event:" field said otherwise) and the
// data lines joined with '\n'.
struct SseEvent
{
    std::string type;
    std::string data;
};

// Incremental parser for text/event-stream (the streaming format of OpenAI-style chat APIs).
// Network reads end anywhere, even inside a line or a UTF-8 character, so feed() buffers
// partial lines and returns only the events completed so far.
class SseParser
{
public:
    std::vector<SseEvent> feed(std::string_view chunk);
    // Flushes an event left without its terminating blank line when the stream ends.
    std::vector<SseEvent> finish();

private:
    void processLine(std::string_view line, std::vector<SseEvent>& events);
    void dispatch(std::vector<SseEvent>& events);

    std::string m_buffer;    // a line not yet terminated
    std::string m_eventType;
    std::string m_data;
    bool m_hasData = false;
    bool m_skipLineFeed = false; // the previous chunk ended with '\r'
};

} // namespace aiedit::core
