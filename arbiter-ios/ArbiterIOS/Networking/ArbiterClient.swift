import Foundation

enum ArbiterClientError: LocalizedError {
    case missingToken
    case invalidURL
    case httpStatus(Int, String)
    case decoding(Error)

    var errorDescription: String? {
        switch self {
        case .missingToken:
            return "Add a tenant API token in Settings."
        case .invalidURL:
            return "API base URL is invalid."
        case .httpStatus(let code, let body):
            return "HTTP \(code): \(body)"
        case .decoding(let error):
            return "Decode error: \(error.localizedDescription)"
        }
    }
}

actor ArbiterClient {
    private let session: URLSession
    private let encoder = JSONEncoder()
    private let decoder = JSONDecoder()

    init(session: URLSession = .shared) {
        self.session = session
    }

    func createConversation(title: String, agentID: String, token: String) async throws -> Conversation {
        let url = AppConfig.baseURL.appending(path: "/v1/conversations")
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try encoder.encode(ConversationCreateRequest(title: title, agentID: agentID))

        let (data, response) = try await session.data(for: request)
        try Self.throwIfNeeded(data: data, response: response, allowed: [200, 201])
        do {
            return try decoder.decode(Conversation.self, from: data)
        } catch {
            throw ArbiterClientError.decoding(error)
        }
    }

    func listConversations(token: String) async throws -> [Conversation] {
        let url = AppConfig.baseURL.appending(path: "/v1/conversations")
        var request = URLRequest(url: url)
        request.httpMethod = "GET"
        request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")

        let (data, response) = try await session.data(for: request)
        try Self.throwIfNeeded(data: data, response: response, allowed: [200])

        // API may return `{ "conversations": [...] }` or a bare array depending on version.
        if let wrapped = try? decoder.decode(ConversationListResponse.self, from: data) {
            return wrapped.conversations
        }
        do {
            return try decoder.decode([Conversation].self, from: data)
        } catch {
            throw ArbiterClientError.decoding(error)
        }
    }

    /// Streams SSE events for a conversation turn.
    nonisolated func sendMessage(
        conversationID: Int,
        message: String,
        token: String,
        idempotencyKey: String? = nil
    ) -> AsyncThrowingStream<SSEEvent, Error> {
        AsyncThrowingStream { continuation in
            let task = Task {
                do {
                    let url = AppConfig.baseURL.appending(path: "/v1/conversations/\(conversationID)/messages")
                    var request = URLRequest(url: url)
                    request.httpMethod = "POST"
                    request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
                    request.setValue("application/json", forHTTPHeaderField: "Content-Type")
                    request.setValue("text/event-stream", forHTTPHeaderField: "Accept")
                    request.timeoutInterval = 3600
                    if let idempotencyKey {
                        request.setValue(idempotencyKey, forHTTPHeaderField: "Idempotency-Key")
                    }
                    request.httpBody = try JSONEncoder().encode(MessageSendRequest(message: message))

                    let (bytes, response) = try await URLSession.shared.bytes(for: request)
                    if let http = response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
                        var lines: [String] = []
                        for try await line in bytes.lines {
                            lines.append(line)
                            if lines.joined(separator: "\n").count > 4_096 { break }
                        }
                        throw ArbiterClientError.httpStatus(http.statusCode, lines.joined(separator: "\n"))
                    }

                    // AsyncBytes.lines strips newlines; re-add them so the
                    // parser can detect the blank-line event delimiter.
                    var parser = SSEParser.Buffer()
                    for try await line in bytes.lines {
                        if Task.isCancelled { break }
                        for event in parser.push(line + "\n") {
                            continuation.yield(event)
                        }
                    }
                    for event in parser.finish() {
                        continuation.yield(event)
                    }
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: error)
                }
            }
            continuation.onTermination = { _ in
                task.cancel()
            }
        }
    }

    func cancelRequest(requestID: String, token: String) async throws {
        let url = AppConfig.baseURL.appending(path: "/v1/requests/\(requestID)/cancel")
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        let (data, response) = try await session.data(for: request)
        try Self.throwIfNeeded(data: data, response: response, allowed: [200, 204])
    }

    func health() async throws -> Bool {
        let url = AppConfig.baseURL.appending(path: "/v1/health")
        let (data, response) = try await session.data(from: url)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            return false
        }
        if let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let ok = obj["ok"] as? Bool {
            return ok
        }
        // Some builds return the bare string "ok".
        let text = String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines)
        return text == "ok" || text == "{\"ok\":true}"
    }

    private static func throwIfNeeded(data: Data, response: URLResponse, allowed: Set<Int>) throws {
        guard let http = response as? HTTPURLResponse else {
            throw ArbiterClientError.httpStatus(-1, "No HTTP response")
        }
        guard allowed.contains(http.statusCode) else {
            let body = String(data: data, encoding: .utf8) ?? ""
            throw ArbiterClientError.httpStatus(http.statusCode, body)
        }
    }
}

private struct ConversationListResponse: Decodable {
    let conversations: [Conversation]
}
