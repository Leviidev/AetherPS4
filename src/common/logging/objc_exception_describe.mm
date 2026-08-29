// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#import <Foundation/Foundation.h>

#include <sstream>

#include "common/logging/objc_exception_describe.h"

bool DescribeCurrentObjCException(std::string& out) {
    try {
        throw;
    } catch (NSException* exception) {
        std::ostringstream oss;
        oss << (exception.name ? exception.name.UTF8String : "(no name)") << " -- "
            << (exception.reason ? exception.reason.UTF8String : "(no reason)");
        for (NSString* line in exception.callStackSymbols) {
            oss << "\n  " << line.UTF8String;
        }
        out = oss.str();
        return true;
    } catch (...) {
        return false;
    }
}
