import Foundation

/// Reads shadPS4's own `play_time.txt` (written by `Emulator::UpdatePlayTime` in
/// `src/emulator.cpp`) to show accumulated playtime per game in the library UI. That file is
/// plain text, one line per title: `<CUSA_SERIAL> <H:MM:SS>` (hours unbounded, minutes/seconds
/// zero-padded), living at `<user_dir>/play_time.txt` -- the same Documents directory passed
/// as `user_dir` in `EmulatorProcess.swift`'s `shadps4_init` call.
enum PlayTimeStore {
    /// Returns a short, friendly playtime string (e.g. "2h 15m", "45m", "Not played yet") for
    /// the given title ID, or nil if the title ID is nil. Re-reads the file fresh each call --
    /// it's a handful of short lines, not worth caching/invalidating.
    static func formatted(forTitleId titleId: String?) -> String? {
        guard let titleId, let seconds = seconds(forTitleId: titleId) else { return nil }
        guard seconds > 0 else { return "Not played yet" }

        let hours = seconds / 3600
        let minutes = (seconds % 3600) / 60
        if hours > 0 && minutes > 0 {
            return "\(hours)h \(minutes)m"
        } else if hours > 0 {
            return "\(hours)h"
        } else if minutes > 0 {
            return "\(minutes)m"
        } else {
            return "< 1m"
        }
    }

    private static func seconds(forTitleId titleId: String) -> Int? {
        guard
            let documentsURL = FileManager.default.urls(
                for: .documentDirectory, in: .userDomainMask
            ).first,
            let contents = try? String(
                contentsOf: documentsURL.appendingPathComponent("play_time.txt"), encoding: .utf8)
        else {
            return nil
        }

        for line in contents.split(separator: "\n") {
            let parts = line.split(separator: " ")
            guard parts.count == 2, parts[0] == titleId else { continue }
            let hms = parts[1].split(separator: ":")
            guard hms.count == 3, let h = Int(hms[0]), let m = Int(hms[1]), let s = Int(hms[2])
            else { continue }
            return h * 3600 + m * 60 + s
        }
        return nil
    }
}
