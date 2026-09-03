import SwiftUI

/// Root Settings screen, styled after the iOS Settings app: a profile card up top (like the
/// Apple ID card) that pushes into the profile/personalization screen, followed by a plain
/// list of rows that each push a dedicated full-screen destination -- not a Form of toggles
/// or a segmented switcher, an actual drill-down navigation hierarchy.
struct SettingsView: View {
    @ObservedObject private var profile = ProfileStore.shared

    var body: some View {
        List {
            Section {
                NavigationLink {
                    SettingsPersonalizationView()
                } label: {
                    profileCard
                }
            }

            Section {
                settingsRow(title: "Display & Performance", systemImage: "gauge.with.dots.needle.67percent", tint: .blue) {
                    SettingsDisplayPerformanceView()
                }
                settingsRow(title: "Console", systemImage: "gamecontroller.fill", tint: .indigo) {
                    SettingsConsoleView()
                }
                settingsRow(title: "Network", systemImage: "wifi", tint: .green) {
                    SettingsNetworkView()
                }
            }

            Section {
                settingsRow(title: "Graphics", systemImage: "cube.fill", tint: .purple) {
                    SettingsGraphicsView()
                }
                settingsRow(title: "Audio", systemImage: "speaker.wave.2.fill", tint: .pink) {
                    SettingsAudioView()
                }
                settingsRow(title: "Input & Touch Controls", systemImage: "hand.tap.fill", tint: .orange) {
                    SettingsInputView()
                }
            }

            Section {
                settingsRow(title: "System Modules", systemImage: "shippingbox.fill", tint: .brown) {
                    SettingsSystemModulesView()
                }
                settingsRow(title: "Advanced", systemImage: "wrench.and.screwdriver.fill", tint: .gray) {
                    SettingsAdvancedView()
                }
            }

            Section {
                settingsRow(title: "Socials", systemImage: "bubble.left.and.bubble.right.fill", tint: .cyan) {
                    SettingsSocialsView()
                }
            }

            Section {
                Text("Changes to Display & Performance, Console, Graphics, Audio, and Input save immediately but only take effect the next time you start a game.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle("Settings")
        .onAppear {
            profile.reload()
        }
    }

    private var profileCard: some View {
        HStack(spacing: 14) {
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
            .frame(width: 56, height: 56)
            .clipShape(Circle())

            VStack(alignment: .leading, spacing: 2) {
                Text(profile.username)
                    .font(.title3.weight(.semibold))
                Text("AetherPS4 Profile & Theme")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 6)
    }

    @ViewBuilder
    private func settingsRow<Destination: View>(
        title: String, systemImage: String, tint: Color, @ViewBuilder destination: () -> Destination
    ) -> some View {
        NavigationLink {
            destination()
        } label: {
            Label {
                Text(title)
            } icon: {
                RoundedRectangle(cornerRadius: 7)
                    .fill(tint.gradient)
                    .frame(width: 29, height: 29)
                    .overlay {
                        Image(systemName: systemImage)
                            .font(.system(size: 15, weight: .medium))
                            .foregroundStyle(.white)
                    }
            }
        }
    }
}
