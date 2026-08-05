import Foundation

enum AppConfig {
    /// Public Arbiter API origin (no trailing slash).
    static var baseURL: URL {
        if let override = UserDefaults.standard.string(forKey: Keys.baseURL),
           let url = URL(string: override),
           !override.isEmpty {
            return url
        }
        return URL(string: "https://api.arbiter.run")!
    }

    static var defaultAgentID: String {
        let value = UserDefaults.standard.string(forKey: Keys.agentID) ?? "index"
        return value.isEmpty ? "index" : value
    }

    static func setBaseURL(_ string: String) {
        let trimmed = string.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            UserDefaults.standard.removeObject(forKey: Keys.baseURL)
        } else {
            UserDefaults.standard.set(trimmed, forKey: Keys.baseURL)
        }
    }

    static func setDefaultAgentID(_ string: String) {
        let trimmed = string.trimmingCharacters(in: .whitespacesAndNewlines)
        UserDefaults.standard.set(trimmed.isEmpty ? "index" : trimmed, forKey: Keys.agentID)
    }

    private enum Keys {
        static let baseURL = "arbiter.baseURL"
        static let agentID = "arbiter.agentID"
    }
}
