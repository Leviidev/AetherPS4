This tree is a vendored, permanently-forked copy of FEXCore-Darwin
(/Users/davi/Documents/Coding/AetherCore4/FEXCore-Darwin), brought into
Bachata-S4 to adapt its JIT memory model from macOS's MAP_JIT/
pthread_jit_write_protect_np scheme to the StikDebug dual-mapped RW/RX
protocol Bachata-S4's iOS build already uses (see src/core/ios/
ios_jit_allocator.h). It is not headed upstream and is not a contribution
to FEXCore-Darwin or FEX-Emu; the no-AI-contribution notice that applies
to those projects does not apply to this copy.
