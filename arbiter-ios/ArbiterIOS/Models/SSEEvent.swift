import Foundation

struct SSEEvent: Equatable {
    let event: String
    let data: String
}

struct TextDelta: Decodable {
    let delta: String?
    let depth: Int?
    let agent: String?
    let streamID: Int?

    enum CodingKeys: String, CodingKey {
        case delta, depth, agent
        case streamID = "stream_id"
    }
}

struct ToolCallEvent: Decodable {
    let tool: String?
    let ok: Bool?
    let depth: Int?
    let agent: String?

    enum CodingKeys: String, CodingKey {
        case tool, ok, depth, agent
    }
}

struct RequestReceivedEvent: Decodable {
    let requestID: String?
    let agent: String?
    let tenant: String?

    enum CodingKeys: String, CodingKey {
        case requestID = "request_id"
        case agent, tenant
    }
}

struct DoneEvent: Decodable {
    let ok: Bool?
    let content: String?
    let error: String?
    let requestID: String?
    let conversationID: Int?
    let durationMS: Int?
    let inputTokens: Int?
    let outputTokens: Int?

    enum CodingKeys: String, CodingKey {
        case ok, content, error
        case requestID = "request_id"
        case conversationID = "conversation_id"
        case durationMS = "duration_ms"
        case inputTokens = "input_tokens"
        case outputTokens = "output_tokens"
    }
}
