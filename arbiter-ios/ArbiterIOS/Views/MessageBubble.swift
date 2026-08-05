import SwiftUI

struct MessageBubble: View {
    let message: ChatMessage

    var body: some View {
        HStack {
            if message.role == .user { Spacer(minLength: 48) }

            VStack(alignment: message.role == .user ? .trailing : .leading, spacing: 6) {
                Text(message.text.isEmpty && message.isStreaming ? "Thinking…" : message.text)
                    .font(.body)
                    .foregroundStyle(foreground)
                    .textSelection(.enabled)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 10)
                    .background(background)
                    .clipShape(RoundedRectangle(cornerRadius: 16, style: .continuous))

                if message.isStreaming {
                    ProgressView()
                        .controlSize(.small)
                }
            }

            if message.role != .user { Spacer(minLength: 48) }
        }
    }

    private var background: Color {
        switch message.role {
        case .user:
            return Color.accentColor.opacity(0.9)
        case .assistant:
            return Color(.secondarySystemBackground)
        case .status:
            return Color.clear
        }
    }

    private var foreground: Color {
        switch message.role {
        case .user:
            return .white
        case .assistant, .status:
            return .primary
        }
    }
}
