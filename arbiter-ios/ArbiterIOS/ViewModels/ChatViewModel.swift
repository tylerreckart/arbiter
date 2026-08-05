import Foundation
import Observation

@MainActor
@Observable
final class ChatViewModel {
    var messages: [ChatMessage] = []
    var input: String = ""
    var isSending = false
    var errorMessage: String?
    var conversationID: Int?
    var conversationTitle: String = "New chat"
    var hasToken: Bool = false
    var statusLine: String?

    private let client = ArbiterClient()
    private var streamTask: Task<Void, Never>?
    private var activeRequestID: String?
    private var assistantMessageID: UUID?

    init() {
        refreshTokenState()
    }

    func refreshTokenState() {
        hasToken = KeychainStore.loadToken() != nil
    }

    func startNewConversation() {
        streamTask?.cancel()
        streamTask = nil
        messages = []
        conversationID = nil
        conversationTitle = "New chat"
        errorMessage = nil
        statusLine = nil
        activeRequestID = nil
        assistantMessageID = nil
        isSending = false
    }

    func send() {
        let text = input.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty, !isSending else { return }
        guard let token = KeychainStore.loadToken(), !token.isEmpty else {
            errorMessage = ArbiterClientError.missingToken.localizedDescription
            hasToken = false
            return
        }

        input = ""
        errorMessage = nil
        statusLine = nil
        isSending = true

        let userMessage = ChatMessage(role: .user, text: text)
        messages.append(userMessage)

        let assistantID = UUID()
        assistantMessageID = assistantID
        messages.append(ChatMessage(id: assistantID, role: .assistant, text: "", isStreaming: true))

        streamTask = Task { [weak self] in
            await self?.runTurn(message: text, token: token, assistantID: assistantID)
        }
    }

    func stop() {
        streamTask?.cancel()
        streamTask = nil
        if let requestID = activeRequestID, let token = KeychainStore.loadToken() {
            Task {
                try? await client.cancelRequest(requestID: requestID, token: token)
            }
        }
        finalizeAssistant(ok: false, error: "Canceled")
        isSending = false
        statusLine = nil
    }

    private func runTurn(message: String, token: String, assistantID: UUID) async {
        do {
            let conversationID = try await ensureConversation(token: token)
            let stream = client.sendMessage(
                conversationID: conversationID,
                message: message,
                token: token,
                idempotencyKey: UUID().uuidString
            )

            for try await event in stream {
                if Task.isCancelled { break }
                handle(event: event, assistantID: assistantID)
            }

            if Task.isCancelled {
                finalizeAssistant(ok: false, error: "Canceled")
            } else if let idx = messages.firstIndex(where: { $0.id == assistantID }),
                      messages[idx].isStreaming {
                // Stream ended without done — still close the bubble.
                messages[idx].isStreaming = false
            }
        } catch is CancellationError {
            finalizeAssistant(ok: false, error: "Canceled")
        } catch {
            errorMessage = error.localizedDescription
            finalizeAssistant(ok: false, error: error.localizedDescription)
        }

        isSending = false
        statusLine = nil
        activeRequestID = nil
        streamTask = nil
    }

    private func ensureConversation(token: String) async throws -> Int {
        if let conversationID {
            return conversationID
        }
        let title = conversationTitle == "New chat"
            ? String(messages.first(where: { $0.role == .user })?.text.prefix(48) ?? "Chat")
            : conversationTitle
        let created = try await client.createConversation(
            title: title,
            agentID: AppConfig.defaultAgentID,
            token: token
        )
        conversationID = created.id
        conversationTitle = created.title?.isEmpty == false ? (created.title ?? title) : title
        return created.id
    }

    private func handle(event: SSEEvent, assistantID: UUID) {
        let data = Data(event.data.utf8)

        switch event.event {
        case "request_received":
            if let parsed = try? JSONDecoder().decode(RequestReceivedEvent.self, from: data) {
                activeRequestID = parsed.requestID
                if let idx = messages.firstIndex(where: { $0.id == assistantID }) {
                    messages[idx].requestID = parsed.requestID
                }
            }

        case "text":
            guard let parsed = try? JSONDecoder().decode(TextDelta.self, from: data) else { return }
            // Main reply only — ignore sub-agent depths.
            guard (parsed.depth ?? 0) == 0, let delta = parsed.delta, !delta.isEmpty else { return }
            if let idx = messages.firstIndex(where: { $0.id == assistantID }) {
                messages[idx].text += delta
            }

        case "tool_call":
            if let parsed = try? JSONDecoder().decode(ToolCallEvent.self, from: data) {
                let tool = parsed.tool ?? "tool"
                let mark = (parsed.ok ?? false) ? "✓" : "✗"
                statusLine = "\(mark) \(tool)"
            }

        case "done":
            if let parsed = try? JSONDecoder().decode(DoneEvent.self, from: data) {
                if let cid = parsed.conversationID {
                    conversationID = cid
                }
                if parsed.ok == false {
                    let err = parsed.error ?? "Request failed"
                    errorMessage = err
                    finalizeAssistant(ok: false, error: err)
                } else {
                    finalizeAssistant(ok: true, error: nil)
                }
            } else {
                finalizeAssistant(ok: true, error: nil)
            }

        case "error":
            if let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
               let message = obj["message"] as? String {
                errorMessage = message
            }

        default:
            break
        }
    }

    private func finalizeAssistant(ok: Bool, error: String?) {
        guard let assistantMessageID,
              let idx = messages.firstIndex(where: { $0.id == assistantMessageID }) else {
            return
        }
        messages[idx].isStreaming = false
        if !ok {
            if messages[idx].text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                messages[idx].text = error ?? "Something went wrong."
            }
        }
        self.assistantMessageID = nil
    }
}
