import Foundation

/// Reads and writes the exact same `config.json` the C++ engine reads via
/// `EmulatorSettingsImpl::Load()` (see src/core/emulator_settings.cpp) -- it lives at
/// `<user_dir>/config.json`, where `user_dir` is the same Documents directory
/// EmulatorProcess.swift passes as `ShadPS4Options.user_dir`. Writing here directly, rather
/// than growing the ShadPS4Options C struct for every new knob, means a new setting only
/// needs a line in emulator_settings.h's Setting<T> struct on the C++ side (it's already
/// wired into Save()/Load() there) and a control here -- no new C bridge function per field.
///
/// Caveat: shadps4_init() only calls EmulatorSettings.Load() once per process lifetime
/// (std::call_once), at the first game launch. A setting changed here after that point in
/// the same app session won't be picked up until the app is relaunched -- there is no way to
/// push a live update into the already-running C++ singleton from here.
///
/// Values are kept as a loose [String: Any] tree (via JSONSerialization) rather than a
/// strict Codable model so this file only ever touches the keys it knows about and leaves
/// every other key -- including ones from a newer/older engine build -- untouched on save.
@MainActor
final class ConfigStore {
    static let shared = ConfigStore()

    private var root: [String: Any] = [:]
    private let fileURL: URL

    private init() {
        let documentsPath = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        fileURL = documentsPath.appendingPathComponent("config.json")
        reload()
    }

    /// Re-reads the file from disk, discarding any in-memory state. Call if the C++ side may
    /// have just written it (e.g. after a game session, since EmulatorSettingsImpl::Save()
    /// runs on destruction) and the Settings screen is about to be shown.
    func reload() {
        guard let data = try? Data(contentsOf: fileURL),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            root = [:]
            return
        }
        root = json
    }

    private func write() {
        guard let data = try? JSONSerialization.data(withJSONObject: root, options: [.prettyPrinted, .sortedKeys]) else {
            return
        }
        try? data.write(to: fileURL, options: .atomic)
    }

    private func section(_ name: String) -> [String: Any] {
        root[name] as? [String: Any] ?? [:]
    }

    private func setValue(_ value: Any, section name: String, key: String) {
        var sec = section(name)
        sec[key] = value
        root[name] = sec
        write()
    }

    func bool(_ section: String, _ key: String, default def: Bool) -> Bool {
        (self.section(section)[key] as? Bool) ?? def
    }

    func setBool(_ section: String, _ key: String, _ value: Bool) {
        setValue(value, section: section, key: key)
    }

    func int(_ section: String, _ key: String, default def: Int) -> Int {
        if let n = self.section(section)[key] as? NSNumber {
            return n.intValue
        }
        return def
    }

    func setInt(_ section: String, _ key: String, _ value: Int) {
        setValue(value, section: section, key: key)
    }

    func double(_ section: String, _ key: String, default def: Double) -> Double {
        if let n = self.section(section)[key] as? NSNumber {
            return n.doubleValue
        }
        return def
    }

    func setDouble(_ section: String, _ key: String, _ value: Double) {
        setValue(value, section: section, key: key)
    }

    func string(_ section: String, _ key: String, default def: String) -> String {
        (self.section(section)[key] as? String) ?? def
    }

    func setString(_ section: String, _ key: String, _ value: String) {
        setValue(value, section: section, key: key)
    }
}
