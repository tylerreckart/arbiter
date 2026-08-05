import Foundation

struct Conversation: Identifiable, Decodable, Equatable {
    let id: Int
    var title: String?
    var agentID: String?
    var createdAt: Double?
    var updatedAt: Double?
    var messageCount: Int?
    var archived: Bool?

    enum CodingKeys: String, CodingKey {
        case id, title
        case agentID = "agent_id"
        case createdAt = "created_at"
        case updatedAt = "updated_at"
        case messageCount = "message_count"
        case archived
    }
}

struct ConversationCreateRequest: Encodable {
    let title: String
    let agentID: String

    enum CodingKeys: String, CodingKey {
        case title
        case agentID = "agent_id"
    }
}

struct MessageSendRequest: Encodable {
    let message: String
}
