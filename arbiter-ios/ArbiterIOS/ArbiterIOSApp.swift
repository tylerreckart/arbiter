import SwiftUI

@main
struct ArbiterIOSApp: App {
    @State private var model = ChatViewModel()

    var body: some Scene {
        WindowGroup {
            ChatView(model: model)
        }
    }
}
