// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Three small PS4 dialog APIs (friend list, game invitations, game-defined custom data)
// share the exact same Initialize/Open/UpdateStatus/GetResult/Terminate state machine as
// Libraries::CommonDialog and NpProfileDialog (see np_profile_dialog.cpp for the fuller
// reference implementation with a real UI). None of these three have any UI implemented
// here -- Open() completes the dialog immediately as USER_CANCELED (matching how a real
// player backing out of any of these dialogs looks to the calling game), rather than
// leaving it RUNNING forever waiting for an interaction that will never come. Result
// structs are only ever zero-filled, never read from an unverified field offset, since
// none of these three dialogs' param/result struct layouts are confirmed against a real
// SDK reference.

#include <cstring>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/system/commondialog.h"

namespace Libraries::Np::MiscDialogs {

using Libraries::CommonDialog::Error;
using Libraries::CommonDialog::Status;

struct DialogState {
    Status status = Status::NONE;
};

static DialogState g_friend_list;
static DialogState g_invitation;
static DialogState g_game_custom_data;

Error Initialize(DialogState& state, const char* name) {
    if (state.status != Status::NONE) {
        LOG_ERROR(Lib_NpCommon, "{}Initialize: already initialized", name);
        return Error::ALREADY_INITIALIZED;
    }
    state.status = Status::INITIALIZED;
    LOG_INFO(Lib_NpCommon, "{}Initialize: initialized", name);
    return Error::OK;
}

Error Open(DialogState& state, const char* name) {
    if (state.status != Status::INITIALIZED && state.status != Status::FINISHED) {
        LOG_ERROR(Lib_NpCommon, "{}Open: called without initialize", name);
        return Error::INVALID_STATE;
    }
    // No real UI to show -- complete immediately rather than staying RUNNING forever.
    state.status = Status::FINISHED;
    LOG_INFO(Lib_NpCommon, "{}Open: no UI implemented, completing as user-canceled", name);
    return Error::OK;
}

Status UpdateStatus(DialogState& state) {
    return state.status;
}

Error GetResult(DialogState& state, void* result, u64 result_size, const char* name) {
    if (state.status == Status::NONE) {
        return Error::NOT_INITIALIZED;
    }
    if (state.status != Status::FINISHED) {
        return Error::NOT_FINISHED;
    }
    if (result != nullptr && result_size > 0) {
        // Zero-fill only: result struct field layouts for these three dialogs are not
        // confirmed, so nothing is written beyond an all-zero (result=OK-shaped but
        // inert) buffer. Games reading a non-zero "user canceled" enum from a specific
        // field would need that field's real offset, which isn't guessed here.
        std::memset(result, 0, result_size);
    }
    LOG_INFO(Lib_NpCommon, "{}GetResult: returning zeroed result", name);
    return Error::OK;
}

Error Terminate(DialogState& state, const char* name) {
    if (state.status == Status::NONE) {
        return Error::NOT_INITIALIZED;
    }
    state.status = Status::NONE;
    LOG_INFO(Lib_NpCommon, "{}Terminate: terminated", name);
    return Error::OK;
}

Error PS4_SYSV_ABI sceNpFriendListDialogInitialize() {
    return Initialize(g_friend_list, "sceNpFriendListDialog");
}
Error PS4_SYSV_ABI sceNpFriendListDialogOpen(void* param) {
    return Open(g_friend_list, "sceNpFriendListDialog");
}
Status PS4_SYSV_ABI sceNpFriendListDialogUpdateStatus() {
    return UpdateStatus(g_friend_list);
}
Error PS4_SYSV_ABI sceNpFriendListDialogGetResult(void* result, u64 result_size) {
    return GetResult(g_friend_list, result, result_size, "sceNpFriendListDialog");
}
Error PS4_SYSV_ABI sceNpFriendListDialogTerminate() {
    return Terminate(g_friend_list, "sceNpFriendListDialog");
}

Error PS4_SYSV_ABI sceInvitationDialogInitialize() {
    return Initialize(g_invitation, "sceInvitationDialog");
}
Error PS4_SYSV_ABI sceInvitationDialogOpen(void* param) {
    return Open(g_invitation, "sceInvitationDialog");
}
Status PS4_SYSV_ABI sceInvitationDialogUpdateStatus() {
    return UpdateStatus(g_invitation);
}
Error PS4_SYSV_ABI sceInvitationDialogGetResult(void* result, u64 result_size) {
    return GetResult(g_invitation, result, result_size, "sceInvitationDialog");
}
Error PS4_SYSV_ABI sceInvitationDialogTerminate() {
    return Terminate(g_invitation, "sceInvitationDialog");
}

Error PS4_SYSV_ABI sceGameCustomDataDialogInitialize() {
    return Initialize(g_game_custom_data, "sceGameCustomDataDialog");
}
Error PS4_SYSV_ABI sceGameCustomDataDialogOpen(void* param) {
    return Open(g_game_custom_data, "sceGameCustomDataDialog");
}
Status PS4_SYSV_ABI sceGameCustomDataDialogUpdateStatus() {
    return UpdateStatus(g_game_custom_data);
}
Error PS4_SYSV_ABI sceGameCustomDataDialogGetResult(void* result, u64 result_size) {
    return GetResult(g_game_custom_data, result, result_size, "sceGameCustomDataDialog");
}
Error PS4_SYSV_ABI sceGameCustomDataDialogTerminate() {
    return Terminate(g_game_custom_data, "sceGameCustomDataDialog");
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("2L-W-ZYn2Qo", "libSceNpFriendListDialog", 1, "libSceNpFriendListDialog",
                 sceNpFriendListDialogInitialize);
    LIB_FUNCTION("zUM-RG5Hmyc", "libSceNpFriendListDialog", 1, "libSceNpFriendListDialog",
                 sceNpFriendListDialogOpen);
    LIB_FUNCTION("frwz3eyuA6w", "libSceNpFriendListDialog", 1, "libSceNpFriendListDialog",
                 sceNpFriendListDialogUpdateStatus);
    LIB_FUNCTION("Z4JJhXCnIvY", "libSceNpFriendListDialog", 1, "libSceNpFriendListDialog",
                 sceNpFriendListDialogGetResult);
    LIB_FUNCTION("ECEzk+K9L2k", "libSceNpFriendListDialog", 1, "libSceNpFriendListDialog",
                 sceNpFriendListDialogTerminate);

    LIB_FUNCTION("XvA5KS56wcs", "libSceInvitationDialog", 1, "libSceInvitationDialog",
                 sceInvitationDialogInitialize);
    LIB_FUNCTION("0zU0G+wiVLA", "libSceInvitationDialog", 1, "libSceInvitationDialog",
                 sceInvitationDialogOpen);
    LIB_FUNCTION("9+g9iOq+7kg", "libSceInvitationDialog", 1, "libSceInvitationDialog",
                 sceInvitationDialogUpdateStatus);
    LIB_FUNCTION("8XKR6wa64iQ", "libSceInvitationDialog", 1, "libSceInvitationDialog",
                 sceInvitationDialogGetResult);
    LIB_FUNCTION("B6HVJtDYxEE", "libSceInvitationDialog", 1, "libSceInvitationDialog",
                 sceInvitationDialogTerminate);

    LIB_FUNCTION("xtbb-2f703A", "libSceGameCustomDataDialog", 1, "libSceGameCustomDataDialog",
                 sceGameCustomDataDialogInitialize);
    LIB_FUNCTION("5TvttyRuU84", "libSceGameCustomDataDialog", 1, "libSceGameCustomDataDialog",
                 sceGameCustomDataDialogOpen);
    LIB_FUNCTION("PkdLsRA4ON0", "libSceGameCustomDataDialog", 1, "libSceGameCustomDataDialog",
                 sceGameCustomDataDialogUpdateStatus);
    LIB_FUNCTION("sJptZwvs1is", "libSceGameCustomDataDialog", 1, "libSceGameCustomDataDialog",
                 sceGameCustomDataDialogGetResult);
    LIB_FUNCTION("HwEtHFCpU5U", "libSceGameCustomDataDialog", 1, "libSceGameCustomDataDialog",
                 sceGameCustomDataDialogTerminate);
}

} // namespace Libraries::Np::MiscDialogs
