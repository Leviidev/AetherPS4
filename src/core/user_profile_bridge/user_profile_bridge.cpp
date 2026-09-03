#include "user_profile_bridge.h"

#include <cstdio>
#include <cstring>

#include "core/user_manager.h"
#include "core/user_settings.h"

namespace {

// Matches UserManager::CreateDefaultUsers()'s first entry -- the account real PS4 hardware
// and every other user-facing path here (sceUserServiceGetUserName's own fallback, the
// default player-1 slot) all agree is "the" user for a single-profile setup like this app's.
constexpr int32_t kPrimaryUserId = 1000;

} // namespace

int bachata_get_primary_username(char* out_buf, int out_buf_size) {
    if (out_buf == nullptr || out_buf_size <= 0) {
        return 1;
    }
    out_buf[0] = '\0';

    UserSettings.Load();
    const User* user = UserManagement.GetUserByID(kPrimaryUserId);
    if (user == nullptr) {
        return 1;
    }

    std::snprintf(out_buf, static_cast<size_t>(out_buf_size), "%s", user->user_name.c_str());
    return 0;
}

int bachata_set_primary_username(const char* new_name) {
    if (new_name == nullptr || new_name[0] == '\0') {
        return 1;
    }

    UserSettings.Load();
    if (UserManagement.GetUserByID(kPrimaryUserId) == nullptr) {
        return 1;
    }

    return UserManagement.RenameUser(kPrimaryUserId, new_name) ? 0 : 1;
}
