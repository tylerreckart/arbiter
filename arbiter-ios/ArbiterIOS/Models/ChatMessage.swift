import Foundation

struct ChatMessage: Identifiable, Equatable {
    enum Role: String, Equatable {
        case user
        case assistant
        case status
    }

    let id: UUID
    var role: Role
    var text: String
    var isStreaming: Bool
    var requestID: String?

    init(
        id: UUID = UUID(),
        role: Role,
        text: String,
        isStreaming: Bool = false,
        requestID: String? = nil
    ) {
        self.id = id
        self.role = role
        self.text = text
        self.isStreaming = isStreaming
        self.requestID = requestID
    }
}
