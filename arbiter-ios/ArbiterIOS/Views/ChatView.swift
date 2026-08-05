import SwiftUI

struct ChatView: View {
    @Bindable var model: ChatViewModel
    @State private var showSettings = false
    @FocusState private var inputFocused: Bool

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                messageList
                Divider()
                composer
            }
            .background(Color(.systemBackground))
            .navigationTitle(model.conversationTitle)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button {
                        model.startNewConversation()
                    } label: {
                        Image(systemName: "square.and.pencil")
                    }
                    .accessibilityLabel("New chat")
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        showSettings = true
                    } label: {
                        Image(systemName: "gearshape")
                    }
                    .accessibilityLabel("Settings")
                }
            }
            .sheet(isPresented: $showSettings, onDismiss: {
                model.refreshTokenState()
            }) {
                SettingsView(hasToken: Binding(
                    get: { model.hasToken },
                    set: { model.hasToken = $0 }
                ))
            }
            .safeAreaInset(edge: .top, spacing: 0) {
                if !model.hasToken {
                    banner(
                        text: "Add your atr_… token in Settings to chat with the API.",
                        color: .orange
                    )
                } else if let error = model.errorMessage {
                    banner(text: error, color: .red)
                } else if let status = model.statusLine {
                    banner(text: status, color: .secondary)
                }
            }
        }
    }

    private var messageList: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 12) {
                    if model.messages.isEmpty {
                        emptyState
                    }
                    ForEach(model.messages) { message in
                        MessageBubble(message: message)
                            .id(message.id)
                    }
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 12)
            }
            .onChange(of: model.messages.last?.text) { _, _ in
                if let id = model.messages.last?.id {
                    withAnimation(.easeOut(duration: 0.15)) {
                        proxy.scrollTo(id, anchor: .bottom)
                    }
                }
            }
        }
    }

    private var emptyState: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Arbiter")
                .font(.largeTitle.weight(.semibold))
            Text("Chat with your deployed API at \(AppConfig.baseURL.host ?? AppConfig.baseURL.absoluteString).")
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.top, 24)
    }

    private var composer: some View {
        HStack(alignment: .bottom, spacing: 10) {
            TextField("Message", text: $model.input, axis: .vertical)
                .textFieldStyle(.plain)
                .lineLimit(1...6)
                .focused($inputFocused)
                .padding(.horizontal, 12)
                .padding(.vertical, 10)
                .background(Color(.secondarySystemBackground))
                .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
                .disabled(model.isSending)

            if model.isSending {
                Button {
                    model.stop()
                } label: {
                    Image(systemName: "stop.circle.fill")
                        .font(.system(size: 32))
                        .symbolRenderingMode(.hierarchical)
                        .foregroundStyle(.red)
                }
                .accessibilityLabel("Stop")
            } else {
                Button {
                    model.send()
                    inputFocused = true
                } label: {
                    Image(systemName: "arrow.up.circle.fill")
                        .font(.system(size: 32))
                        .symbolRenderingMode(.hierarchical)
                }
                .disabled(
                    model.input.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
                        || !model.hasToken
                )
                .accessibilityLabel("Send")
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }

    private func banner(text: String, color: Color) -> some View {
        Text(text)
            .font(.footnote)
            .foregroundStyle(color == .secondary ? Color.secondary : color)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 16)
            .padding(.vertical, 8)
            .background(Color(.secondarySystemBackground).opacity(0.95))
    }
}
