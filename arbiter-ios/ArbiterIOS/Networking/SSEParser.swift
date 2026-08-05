import Foundation

/// Incremental parser for Arbiter's `text/event-stream` frames.
enum SSEParser {
    struct Buffer {
        private var pending = ""

        mutating func push(_ chunk: String) -> [SSEEvent] {
            pending += chunk
            var events: [SSEEvent] = []

            while let range = pending.range(of: "\n\n") {
                let block = String(pending[..<range.lowerBound])
                pending = String(pending[range.upperBound...])
                if let event = Self.parseBlock(block) {
                    events.append(event)
                }
            }
            return events
        }

        mutating func finish() -> [SSEEvent] {
            guard !pending.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
                return []
            }
            let leftover = pending
            pending = ""
            if let event = Self.parseBlock(leftover) {
                return [event]
            }
            return []
        }
    }

    static func parseBlock(_ block: String) -> SSEEvent? {
        var eventName = "message"
        var dataLines: [String] = []

        for rawLine in block.split(separator: "\n", omittingEmptySubsequences: false) {
            let line = String(rawLine)
            if line.hasPrefix(":") {
                // Heartbeat / comment — ignore.
                continue
            }
            if line.hasPrefix("event:") {
                eventName = line.dropFirst(6).trimmingCharacters(in: .whitespaces)
            } else if line.hasPrefix("data:") {
                dataLines.append(line.dropFirst(5).trimmingCharacters(in: .whitespaces))
            }
        }

        guard !dataLines.isEmpty else { return nil }
        return SSEEvent(event: eventName, data: dataLines.joined(separator: "\n"))
    }
}
