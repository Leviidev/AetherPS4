// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Libraries::Mouse {

enum class MouseOpenBehaviour : u8 {
    Normal = 0,
    Merged = 1,
};

struct OrbisMouseOpenParam {
    MouseOpenBehaviour flag;
    u8 reserve[7];
};

// Official libSceMouse accepts a null param and treats it as Normal.
inline const OrbisMouseOpenParam* ResolveMouseOpenParam(const OrbisMouseOpenParam* pParam,
                                                        OrbisMouseOpenParam& storage) {
    if (pParam != nullptr) {
        return pParam;
    }
    storage = {};
    storage.flag = MouseOpenBehaviour::Normal;
    return &storage;
}

} // namespace Libraries::Mouse
