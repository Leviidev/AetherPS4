import SwiftUI
import UIKit

/// The emulated PS4 profile's display name and this app's own profile picture.
///
/// The username is a thin wrapper over bachata_get/set_primary_username (see
/// src/core/user_profile_bridge/) -- it's real emulator state (the same name
/// sceUserServiceGetUserName reports to games), stored in users.json, not something this
/// class invents. The profile picture has no equivalent on real PS4 hardware or in shadPS4's
/// own emulation surface -- it's purely cosmetic, shown only inside this app's own UI, so it's
/// stored as a plain JPEG in Documents rather than going through the C bridge at all.
@MainActor
final class ProfileStore: ObservableObject {
    static let shared = ProfileStore()

    @Published private(set) var username: String = "shadPS4"
    @Published private(set) var profileImage: UIImage?

    private let imageFileURL: URL

    private init() {
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        imageFileURL = documents.appendingPathComponent("profile_picture.jpg")
        reload()
    }

    func reload() {
        var buffer = [CChar](repeating: 0, count: 256)
        if bachata_get_primary_username(&buffer, Int32(buffer.count)) == 0 {
            let name = String(cString: buffer)
            if !name.isEmpty {
                username = name
            }
        }
        if let data = try? Data(contentsOf: imageFileURL), let image = UIImage(data: data) {
            profileImage = image
        } else {
            profileImage = nil
        }
    }

    @discardableResult
    func setUsername(_ newName: String) -> Bool {
        let trimmed = newName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return false }
        let ok = trimmed.withCString { bachata_set_primary_username($0) == 0 }
        if ok {
            username = trimmed
        }
        return ok
    }

    func setProfileImage(_ image: UIImage) {
        guard let data = image.jpegData(compressionQuality: 0.85) else { return }
        try? data.write(to: imageFileURL, options: .atomic)
        profileImage = image
    }

    func clearProfileImage() {
        try? FileManager.default.removeItem(at: imageFileURL)
        profileImage = nil
    }
}
