import Foundation
import CryptoKit

/// Parser for PlayStation Param.SFO (PSF format) files.
struct ParamSfo {
    var title: String?
    var titleId: String?
    var appVersion: String?
    var contentId: String?
    var entries: [String: Any] = [:]

    static func parse(from url: URL) -> ParamSfo? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        return parse(from: data)
    }

    static func parse(from data: Data) -> ParamSfo? {
        guard data.count >= 0x14 else { return nil }

        let magic = data.withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
        // \0PSF in little-endian = 0x46535000
        guard magic == 0x46535000 else { return nil }

        let keyTableOffset = Int(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0x08, as: UInt32.self) })
        let dataTableOffset = Int(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0x0C, as: UInt32.self) })
        let numEntries = Int(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0x10, as: UInt32.self) })

        var sfo = ParamSfo()

        for i in 0..<numEntries {
            let entryOffset = 0x14 + i * 16
            guard entryOffset + 16 <= data.count else { break }

            let keyOffset = Int(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: entryOffset, as: UInt16.self) })
            let paramFmt = data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: entryOffset + 2, as: UInt16.self) }
            let paramLen = Int(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: entryOffset + 4, as: UInt32.self) })
            let dataOffset = Int(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: entryOffset + 12, as: UInt32.self) })

            let absoluteKeyOffset = keyTableOffset + keyOffset
            guard absoluteKeyOffset < data.count else { continue }

            // Find null terminator for key
            var keyEnd = absoluteKeyOffset
            while keyEnd < data.count && data[keyEnd] != 0 {
                keyEnd += 1
            }
            guard let key = String(data: data[absoluteKeyOffset..<keyEnd], encoding: .utf8) else { continue }

            let absoluteDataOffset = dataTableOffset + dataOffset
            guard absoluteDataOffset + paramLen <= data.count else { continue }

            let rawValData = data[absoluteDataOffset..<(absoluteDataOffset + paramLen)]

            if paramFmt == 0x0204 { // UTF-8 String (null-terminated)
                if let str = String(data: rawValData, encoding: .utf8)?.trimmingCharacters(in: CharacterSet(charactersIn: "\0")) {
                    sfo.entries[key] = str
                    if key == "TITLE" { sfo.title = str }
                    else if key == "TITLE_ID" { sfo.titleId = str }
                    else if key == "APP_VER" { sfo.appVersion = str }
                    else if key == "CONTENT_ID" { sfo.contentId = str }
                }
            } else if paramFmt == 0x0404 { // Integer (32-bit uint)
                if paramLen >= 4 {
                    let val = rawValData.withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
                    sfo.entries[key] = val
                }
            }
        }

        return sfo
    }
}
