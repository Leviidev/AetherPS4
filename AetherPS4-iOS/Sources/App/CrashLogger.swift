import Foundation
import UIKit

class CrashLogger {
    static let shared = CrashLogger()

    var logFilePath: String = ""
    private var logFileDescriptor: Int32 = -1

    func setup() {
        let documentsPath = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].path
        logFilePath = documentsPath + "/aether_crash.log"

        // Remove old log if it's too big or just append. Let's recreate it each launch.
        try? FileManager.default.removeItem(atPath: logFilePath)

        // Redirect stdout and stderr to the log file
        freopen(logFilePath.cString(using: .utf8), "a+", stdout)
        freopen(logFilePath.cString(using: .utf8), "a+", stderr)

        // Fully unbuffered: every print()/fprintf() call hits the OS write() syscall
        // immediately instead of sitting in libc's buffer. A hard crash (SIGSEGV from
        // JIT'd guest code, etc.) never runs normal cleanup, so anything still buffered
        // at that point would otherwise just vanish -- this is the difference between a
        // log that cuts off right before the actual failure and one that has it.
        setvbuf(stdout, nil, _IONBF, 0)
        setvbuf(stderr, nil, _IONBF, 0)

        // Keep a raw fd open for the signal handler below -- it must not go through
        // Swift's buffered stdio (print/fputs) since that's not safe to call from
        // inside a signal handler (can allocate/lock). Raw write(2) is.
        logFileDescriptor = fileno(stdout)

        print("--- AetherPS4 iOS App Launched at \(Date()) ---")

        // Backstop in case something upstream re-buffers stdout (e.g. a library that
        // calls setvbuf itself) or the crash happens in native code that wrote via a
        // different FILE*/fd than Swift's stdout: force a real flush on a fixed
        // cadence regardless of what was just logged.
        startPeriodicFlush()

        // Setup NSException handler. This runs on the throwing thread before any
        // signal is raised, so ordinary Swift/Foundation calls are still safe here.
        NSSetUncaughtExceptionHandler { exception in
            print("--- UNCAUGHT EXCEPTION ---")
            print("Name: \(exception.name)")
            print("Reason: \(exception.reason ?? "nil")")
            print("Symbols:\n\(exception.callStackSymbols.joined(separator: "\n"))")
            fflush(stdout)
            fflush(stderr)
        }

        // Setup signal handlers. Everything from here down runs inside a raw POSIX
        // signal handler -- only async-signal-safe calls are allowed (write(2),
        // backtrace_symbols_fd(2), _exit(2)). No print(), no Thread.callStackSymbols,
        // no String interpolation/malloc: those can deadlock or silently produce
        // nothing if the crash happened mid-allocation or while holding a related
        // lock, which is exactly why the old handler's output was unreliable.
        let signals = [SIGILL, SIGTRAP, SIGABRT, SIGFPE, SIGBUS, SIGSEGV, SIGQUIT]
        for sig in signals {
            signal(sig) { sig in
                let fd = CrashLogger.shared.logFileDescriptor
                "\n--- CAUGHT SIGNAL ".withCString { write(fd, $0, strlen($0)) }
                CrashLogger.writeDecimal(Int(sig), to: fd)
                " ---\n".withCString { write(fd, $0, strlen($0)) }

                var frames = [UnsafeMutableRawPointer?](repeating: nil, count: 128)
                let frameCount = frames.withUnsafeMutableBufferPointer { ptr in
                    backtrace(ptr.baseAddress, 128)
                }
                backtrace_symbols_fd(&frames, frameCount, fd)

                fsync(fd)

                // Reset signal handler to default and raise so the app actually crashes
                signal(sig, SIG_DFL)
                raise(sig)
            }
        }
    }

    /// Writes a non-negative integer as decimal digits directly to `fd`, without
    /// going through String interpolation/printf -- kept trivial on purpose so it
    /// stays safe to call from the signal handler above.
    fileprivate static func writeDecimal(_ value: Int, to fd: Int32) {
        var n = value
        if n == 0 {
            var zero: Int8 = 48
            write(fd, &zero, 1)
            return
        }
        var digits = [Int8]()
        while n > 0 {
            digits.append(Int8(48 + (n % 10)))
            n /= 10
        }
        digits.reverse()
        write(fd, digits, digits.count)
    }

    private func startPeriodicFlush() {
        let queue = DispatchQueue(label: "com.aether.ps4ios.crashlogflush", qos: .utility)
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + .milliseconds(100), repeating: .milliseconds(100))
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            fflush(stdout)
            fflush(stderr)
            if self.logFileDescriptor >= 0 {
                fsync(self.logFileDescriptor)
            }
        }
        timer.resume()
        periodicFlushTimer = timer
    }

    private var periodicFlushTimer: DispatchSourceTimer?
}
