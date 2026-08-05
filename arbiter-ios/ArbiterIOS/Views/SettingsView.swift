import SwiftUI

struct SettingsView: View {
    @Environment(\.dismiss) private var dismiss
    @Binding var hasToken: Bool

    @State private var token: String = ""
    @State private var baseURL: String = AppConfig.baseURL.absoluteString
    @State private var agentID: String = AppConfig.defaultAgentID
    @State private var healthStatus: String?
    @State private var isCheckingHealth = false
    @State private var saveError: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    SecureField("atr_… tenant token", text: $token)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    if KeychainStore.loadToken() != nil {
                        Button("Clear saved token", role: .destructive) {
                            KeychainStore.deleteToken()
                            token = ""
                            hasToken = false
                        }
                    }
                } header: {
                    Text("API token")
                } footer: {
                    Text("Stored in the Keychain on this device. Create one with `arbiter --add-tenant` on the server.")
                }

                Section("Server") {
                    TextField("Base URL", text: $baseURL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .keyboardType(.URL)
                    TextField("Default agent id", text: $agentID)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                }

                Section {
                    Button {
                        Task { await checkHealth() }
                    } label: {
                        if isCheckingHealth {
                            ProgressView()
                        } else {
                            Text("Check /v1/health")
                        }
                    }
                    .disabled(isCheckingHealth)

                    if let healthStatus {
                        Text(healthStatus)
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                }

                if let saveError {
                    Section {
                        Text(saveError)
                            .foregroundStyle(.red)
                    }
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { save() }
                }
            }
            .onAppear {
                // Don't echo the full token back into a SecureField by default.
                token = ""
                baseURL = AppConfig.baseURL.absoluteString
                agentID = AppConfig.defaultAgentID
            }
        }
    }

    private func save() {
        saveError = nil
        AppConfig.setBaseURL(baseURL)
        AppConfig.setDefaultAgentID(agentID)

        let trimmed = token.trimmingCharacters(in: .whitespacesAndNewlines)
        if !trimmed.isEmpty {
            do {
                try KeychainStore.saveToken(trimmed)
                hasToken = true
            } catch {
                saveError = error.localizedDescription
                return
            }
        } else {
            hasToken = KeychainStore.loadToken() != nil
        }
        dismiss()
    }

    private func checkHealth() async {
        isCheckingHealth = true
        defer { isCheckingHealth = false }
        AppConfig.setBaseURL(baseURL)
        do {
            let ok = try await ArbiterClient().health()
            healthStatus = ok ? "Healthy — \(AppConfig.baseURL.absoluteString)" : "Unexpected health payload"
        } catch {
            healthStatus = error.localizedDescription
        }
    }
}
