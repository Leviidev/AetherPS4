import PhotosUI
import SwiftUI
import UIKit

/// Profile (emulated PS4 username + this app's own profile picture) and app theme color.
struct SettingsPersonalizationView: View {
    @ObservedObject private var profile = ProfileStore.shared
    @ObservedObject private var theme = AppTheme.shared

    @State private var usernameDraft = ""
    @State private var usernameSaveFailed = false
    @State private var selectedPhotoItem: PhotosPickerItem?
    @State private var customColor: Color = .blue

    var body: some View {
        Form {
            Section("Profile") {
                HStack(spacing: 16) {
                    PhotosPicker(selection: $selectedPhotoItem, matching: .images) {
                        profileAvatar
                    }
                    .buttonStyle(.plain)

                    VStack(alignment: .leading, spacing: 4) {
                        Text("Tap the picture to change it.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        if profile.profileImage != nil {
                            Button("Remove Picture", role: .destructive) {
                                profile.clearProfileImage()
                            }
                            .font(.caption)
                        }
                    }
                }
                .padding(.vertical, 4)

                HStack {
                    TextField("Username", text: $usernameDraft)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .onSubmit(saveUsername)
                    if usernameDraft != profile.username {
                        Button("Save", action: saveUsername)
                            .buttonStyle(.borderedProminent)
                    }
                }
                if usernameSaveFailed {
                    Text("Couldn't save that name. Try something short, without leading/trailing spaces.")
                        .font(.caption)
                        .foregroundStyle(.red)
                } else {
                    Text("This is the name games see for your PS4 profile.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            Section("Theme Color") {
                LazyVGrid(columns: Array(repeating: GridItem(.flexible()), count: 5), spacing: 14) {
                    ForEach(AppTheme.presets) { preset in
                        Button {
                            theme.accentColor = preset.color
                        } label: {
                            ThemeSwatch(color: preset.color, isSelected: isSelected(preset.color))
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(preset.name)
                    }
                }
                .padding(.vertical, 6)

                ColorPicker("Custom Color", selection: $customColor, supportsOpacity: false)
                    .onChange(of: customColor) { _, newValue in
                        theme.accentColor = newValue
                    }

                Text("Tints buttons, toggles, and backgrounds throughout the app.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle("Profile")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear {
            usernameDraft = profile.username
            customColor = theme.accentColor
        }
        .onChange(of: selectedPhotoItem) { _, item in
            loadPickedPhoto(item)
        }
    }

    private var profileAvatar: some View {
        Group {
            if let image = profile.profileImage {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
            } else {
                Image(systemName: "person.crop.circle.fill")
                    .resizable()
                    .foregroundStyle(.secondary)
            }
        }
        .frame(width: 72, height: 72)
        .clipShape(Circle())
        .overlay(Circle().strokeBorder(Color.accentColor, lineWidth: 2))
    }

    private func isSelected(_ color: Color) -> Bool {
        color.toHex() == theme.accentColor.toHex()
    }

    private func saveUsername() {
        usernameSaveFailed = !profile.setUsername(usernameDraft)
        if !usernameSaveFailed {
            usernameDraft = profile.username
        }
    }

    private func loadPickedPhoto(_ item: PhotosPickerItem?) {
        guard let item else { return }
        Task {
            if let data = try? await item.loadTransferable(type: Data.self),
               let image = UIImage(data: data) {
                profile.setProfileImage(image)
            }
        }
    }
}

private struct ThemeSwatch: View {
    let color: Color
    let isSelected: Bool

    var body: some View {
        Circle()
            .fill(color)
            .frame(width: 32, height: 32)
            .overlay(
                Circle()
                    .strokeBorder(.primary, lineWidth: isSelected ? 2 : 0)
                    .padding(-3)
            )
            .overlay {
                if isSelected {
                    Image(systemName: "checkmark")
                        .font(.caption.bold())
                        .foregroundStyle(color.isLight ? .black : .white)
                }
            }
    }
}

private extension Color {
    var isLight: Bool {
        let ui = UIColor(self)
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        ui.getRed(&r, green: &g, blue: &b, alpha: &a)
        let luminance = 0.299 * r + 0.587 * g + 0.114 * b
        return luminance > 0.6
    }
}
