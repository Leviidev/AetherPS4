#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reads the primary (first, player-index-1) emulated PS4 user's display name -- the same
// name sceUserServiceGetUserName reports to games -- into out_buf. Loads the user database
// from disk (users.json under the app's UserDir) if it hasn't been loaded yet this process,
// creating the default "shadPS4" user on first run just like a real game boot would.
// Returns 0 on success, non-zero if out_buf/out_buf_size are invalid or no primary user exists.
int bachata_get_primary_username(char* out_buf, int out_buf_size);

// Renames the primary emulated PS4 user and persists it to users.json immediately, so the
// change is visible the next time any game (or this same function) reads it back -- no
// separate "save" step needed. Returns 0 on success, non-zero on failure (null/empty name,
// or no primary user exists yet).
int bachata_set_primary_username(const char* new_name);

#ifdef __cplusplus
}
#endif
