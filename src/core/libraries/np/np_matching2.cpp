// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <deque>
#include <mutex>

#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/np/np_manager.h"
#include "core/libraries/np/np_matching2.h"
#include "core/libraries/np/np_types.h"
#include "core/libraries/system/userservice.h"

namespace Libraries::Np::NpMatching2 {

static bool g_initialized = false;
static OrbisNpMatching2ContextId contextId = 1;

struct NpMatching2ContextEvent {
    OrbisNpMatching2ContextId contextId;
    OrbisNpMatching2Event event;
    OrbisNpMatching2EventCause cause;
    int errorCode;
};

struct NpMatching2LobbyEvent {
    OrbisNpMatching2ContextId contextId;
    OrbisNpMatching2LobbyId lobbyId;
    OrbisNpMatching2Event event;
    void* data;
};

struct NpMatching2RoomEvent {
    OrbisNpMatching2ContextId contextId;
    OrbisNpMatching2RoomId roomId;
    OrbisNpMatching2Event event;
    void* data;
};

static std::mutex g_events_mutex;
static std::deque<NpMatching2ContextEvent> g_ctx_events;
static std::deque<NpMatching2LobbyEvent> g_lobby_events;
static std::deque<NpMatching2RoomEvent> g_room_events;
static std::mutex g_responses_mutex;
static std::deque<std::function<void()>> g_responses;

struct OrbisNpMatching2CreateContextParameter {
    Libraries::Np::OrbisNpId* npId;
    void* npCommunicationId;
    void* npPassphrase;
    Libraries::Np::OrbisNpServiceLabel serviceLabel;
    u64 size;
};

static_assert(sizeof(OrbisNpMatching2CreateContextParameter) == 0x28);

int PS4_SYSV_ABI sceNpMatching2CreateContext(const OrbisNpMatching2CreateContextParameter* param,
                                             OrbisNpMatching2ContextId* ctxId) {
    LOG_DEBUG(Lib_NpMatching2, "called, npId = {}, serviceLabel = {}, size = {}",
              param->npId->handle.data, param->serviceLabel, param->size);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!param || param->size != 0x28 || !ctxId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    *ctxId = contextId++;

    return ORBIS_OK;
}

struct OrbisNpMatching2CreateContextParameterA {
    Libraries::UserService::OrbisUserServiceUserId userId;
    Libraries::Np::OrbisNpServiceLabel serviceLabel;
    u64 size;
};

static_assert(sizeof(OrbisNpMatching2CreateContextParameterA) == 16);

int PS4_SYSV_ABI sceNpMatching2CreateContextA(const OrbisNpMatching2CreateContextParameterA* param,
                                              OrbisNpMatching2ContextId* ctxId) {
    LOG_DEBUG(Lib_NpMatching2, "called, userId = {}, serviceLabel = {}, size = {}", param->userId,
              param->serviceLabel, param->size);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!param || param->size != 0x10 || !ctxId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    *ctxId = contextId++;

    return ORBIS_OK;
}

using OrbisNpMatching2RequestCallback = PS4_SYSV_ABI void (*)(OrbisNpMatching2ContextId,
                                                              OrbisNpMatching2RequestId,
                                                              OrbisNpMatching2Event, int, void*,
                                                              void*);

struct OrbisNpMatching2RequestOptParam {
    OrbisNpMatching2RequestCallback callback;
    void* arg;
    u32 timeout;
    u16 appId;
    u8 dummy[2];
};

static std::optional<OrbisNpMatching2RequestOptParam> defaultRequestOptParam = std::nullopt;

auto GetOptParam(OrbisNpMatching2RequestOptParam* requestOpt) {
    return requestOpt ? *requestOpt
                      : (defaultRequestOptParam ? defaultRequestOptParam
                                                : std::optional<OrbisNpMatching2RequestOptParam>{});
}

struct OrbisNpMatching2CreateJoinRoomRequestA {
    u16 maxSlot;
    OrbisNpMatching2TeamId teamId;
    u8 pad[5];
    OrbisNpMatching2Flags flags;
    OrbisNpMatching2WorldId worldId;
    OrbisNpMatching2LobbyId lobbyId;
    void* roomPasswd;
    void* passwdSlotMask;
    void* groupConfig;
    u64 groupConfigs;
    void* joinGroupLabel;
    Libraries::Np::OrbisNpAccountId* allowedUser;
    u64 allowedUsers;
    Libraries::Np::OrbisNpAccountId* blockedUser;
    u64 blockedUsers;
    void* internalBinAttr;
    u64 internalBinAttrs;
    void* externalSearchIntAttr;
    u64 externalSearchIntAttrs;
    void* externalSearchBinAttr;
    u64 externalSearchBinAttrs;
    void* externalBinAttr;
    u64 externalBinAttrs;
    void* memberInternalBinAttr;
    u64 memberInternalBinAttrs;
    void* signalingParam;
};

static_assert(sizeof(OrbisNpMatching2CreateJoinRoomRequestA) == 184);

struct OrbisNpMatching2RoomDataInternal {
    u16 publicSlots;
    u16 privateSlots;
    u16 openPublicSlots;
    u16 openPrivateSlots;
    u16 maxSlot;
    OrbisNpMatching2ServerId serverId;
    OrbisNpMatching2WorldId worldId;
    OrbisNpMatching2LobbyId lobbyId;
    OrbisNpMatching2RoomId roomId;
    u64 passwdSlotMask;
    u64 joinedSlotMask;
    void* roomGroup;
    u64 roomGroups;
    OrbisNpMatching2Flags flags;
    u8 pad[4];
    void* internalBinAttr;
    u64 internalBinAttrs;
};

struct OrbisNpMatching2RoomMemberDataInternalA {
    OrbisNpMatching2RoomMemberDataInternalA* next;
    u64 joinDateTicks;
    Libraries::Np::OrbisNpPeerAddressA user;
    Libraries::Np::OrbisNpOnlineId onlineId;
    u8 pad[4];
    OrbisNpMatching2RoomMemberId memberId;
    OrbisNpMatching2TeamId teamId;
    OrbisNpMatching2NatType natType;
    OrbisNpMatching2Flags flags;
    void* roomGroup;
    void* roomMemberInternalBinAttr;
    u64 roomMemberInternalBinAttrs;
};

struct OrbisNpMatching2RoomMemberDataInternalListA {
    OrbisNpMatching2RoomMemberDataInternalA* members;
    u64 membersNum;
    OrbisNpMatching2RoomMemberDataInternalA* me;
    OrbisNpMatching2RoomMemberDataInternalA* owner;
};

struct OrbisNpMatching2CreateJoinRoomResponseA {
    OrbisNpMatching2RoomDataInternal* roomData;
    OrbisNpMatching2RoomMemberDataInternalListA members;
};

int PS4_SYSV_ABI sceNpMatching2CreateJoinRoomA(OrbisNpMatching2ContextId ctxId,
                                               OrbisNpMatching2CreateJoinRoomRequestA* request,
                                               OrbisNpMatching2RequestOptParam* requestOpt,
                                               OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    LOG_DEBUG(Lib_NpMatching2,
              "maxSlot = {}, teamId = {}, worldId = {}, lobbyId = {}, groupConfig = {}, "
              "joinGroupLabel = {}",
              request->maxSlot, request->teamId, request->worldId, request->lobbyId,
              request->groupConfig, request->joinGroupLabel);

    static OrbisNpMatching2RequestId id = 10;
    *requestId = id++;

    if (auto optParam = GetOptParam(requestOpt); optParam) {
        LOG_DEBUG(Lib_NpMatching2, "optParam.timeout = {}, optParam.appId = {}", optParam->timeout,
                  optParam->appId);
        std::scoped_lock lk{g_responses_mutex};
        auto reqIdCopy = *requestId;
        auto requestCopy = *request;
        g_responses.emplace_back([=]() {
            Libraries::Np::OrbisNpOnlineId onlineId{};
            if (NpManager::sceNpGetOnlineId(1, &onlineId) != ORBIS_OK) {
                return;
            }

            OrbisNpMatching2RoomMemberDataInternalA me{
                nullptr,
                0,
                {0xace104e, Libraries::Np::OrbisNpPlatformType::PS4},
                onlineId,
                {0, 0, 0, 0},
                1,
                requestCopy.teamId,
                1,
                0,
                nullptr,
                nullptr,
                0};
            OrbisNpMatching2RoomDataInternal room{requestCopy.maxSlot,
                                                  0,
                                                  static_cast<u16>(requestCopy.maxSlot - 1u),
                                                  0,
                                                  15,
                                                  0xac,
                                                  requestCopy.worldId,
                                                  requestCopy.lobbyId,
                                                  0x10,
                                                  0,
                                                  0,
                                                  nullptr,
                                                  0,
                                                  0,
                                                  {0, 0, 0, 0},
                                                  nullptr,
                                                  0};
            OrbisNpMatching2CreateJoinRoomResponseA resp{&room, {&me, 1, &me, &me}};
            optParam->callback(ctxId, reqIdCopy,
                               ORBIS_NP_MATCHING2_REQUEST_EVENT_CREATE_JOIN_ROOM_A, 0, &resp,
                               optParam->arg);
        });
    }
    return ORBIS_OK;
}

using OrbisNpMatching2ContextCallback = PS4_SYSV_ABI void (*)(OrbisNpMatching2ContextId contextId,
                                                              OrbisNpMatching2Event event,
                                                              OrbisNpMatching2EventCause cause,
                                                              int errorCode, void* userdata);

std::function<void(const NpMatching2ContextEvent*)> npMatching2ContextCallback = nullptr;

int PS4_SYSV_ABI sceNpMatching2RegisterContextCallback(OrbisNpMatching2ContextCallback callback,
                                                       void* userdata) {
    LOG_DEBUG(Lib_NpMatching2, "called, userdata = {}", userdata);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }

    npMatching2ContextCallback = [callback, userdata](auto arg) {
        callback(arg->contextId, arg->event, arg->cause, arg->errorCode, userdata);
    };

    return ORBIS_OK;
}

using OrbisNpMatching2LobbyEventCallback =
    PS4_SYSV_ABI void (*)(OrbisNpMatching2ContextId contextId, OrbisNpMatching2LobbyId lobbyId,
                          OrbisNpMatching2Event event, void* data, void* userdata);

std::function<void(const NpMatching2LobbyEvent*)> npMatching2LobbyCallback = nullptr;

int PS4_SYSV_ABI sceNpMatching2RegisterLobbyEventCallback(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2LobbyEventCallback callback, void* userdata) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, userdata = {}", ctxId, userdata);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }

    npMatching2LobbyCallback = [callback, userdata](auto arg) {
        callback(arg->contextId, arg->lobbyId, arg->event, arg->data, userdata);
    };

    return ORBIS_OK;
}

using OrbisNpMatching2RoomEventCallback = PS4_SYSV_ABI void (*)(OrbisNpMatching2ContextId contextId,
                                                                OrbisNpMatching2RoomId roomId,
                                                                OrbisNpMatching2Event event,
                                                                void* data, void* userdata);

std::function<void(const NpMatching2RoomEvent*)> npMatching2RoomCallback = nullptr;

int PS4_SYSV_ABI sceNpMatching2RegisterRoomEventCallback(OrbisNpMatching2ContextId ctxId,
                                                         OrbisNpMatching2RoomEventCallback callback,
                                                         void* userdata) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, userdata = {}", ctxId, userdata);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }

    npMatching2RoomCallback = [callback, userdata](auto arg) {
        callback(arg->contextId, arg->roomId, arg->event, arg->data, userdata);
    };

    return ORBIS_OK;
}

struct OrbisNpMatching2SignalingEvent {
    OrbisNpMatching2ContextId contextId;
    OrbisNpMatching2RoomId roomId;
    OrbisNpMatching2RoomMemberId roomMemberId;
    OrbisNpMatching2Event event;
    int errorCode;
};

using OrbisNpMatching2SignalingCallback =
    PS4_SYSV_ABI void (*)(OrbisNpMatching2ContextId contextId, OrbisNpMatching2RoomId roomId,
                          OrbisNpMatching2RoomMemberId roomMemberId, OrbisNpMatching2Event event,
                          int errorCode, void* userdata);

std::function<void(const OrbisNpMatching2SignalingEvent*)> npMatching2SignalingCallback = nullptr;

int PS4_SYSV_ABI sceNpMatching2RegisterSignalingCallback(OrbisNpMatching2ContextId ctxId,
                                                         OrbisNpMatching2SignalingCallback callback,
                                                         void* userdata) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, userdata = {}", ctxId, userdata);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }

    npMatching2SignalingCallback = [callback, userdata](auto arg) {
        callback(arg->contextId, arg->roomId, arg->roomMemberId, arg->event, arg->errorCode,
                 userdata);
    };

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2ContextStart(OrbisNpMatching2ContextId ctxId, u64 timeout) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, timeout = {}", ctxId, timeout);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }

    std::scoped_lock lk{g_events_mutex};
    if (EmulatorSettings.IsConnectedToNetwork() && EmulatorSettings.IsShadNetEnabled()) {
        g_ctx_events.emplace_back(ctxId, ORBIS_NP_MATCHING2_CONTEXT_EVENT_STARTED,
                                  ORBIS_NP_MATCHING2_EVENT_CAUSE_CONTEXT_ACTION, 0);
    } else {
        // error confirmed with a real console disconnected from the internet
        constexpr int ORBIS_NET_ERROR_RESOLVER_ETIMEDOUT = 0x804101e2;
        g_ctx_events.emplace_back(ctxId, ORBIS_NP_MATCHING2_CONTEXT_EVENT_START_OVER,
                                  ORBIS_NP_MATCHING2_EVENT_CAUSE_CONTEXT_ERROR,
                                  ORBIS_NET_ERROR_RESOLVER_ETIMEDOUT);
    }

    return ORBIS_OK;
}

void ProcessEvents() {
    {
        std::scoped_lock lk{g_events_mutex};

        if (npMatching2ContextCallback) {
            while (!g_ctx_events.empty()) {
                npMatching2ContextCallback(&g_ctx_events.front());
                g_ctx_events.pop_front();
            }
        }
        if (npMatching2LobbyCallback) {
            while (!g_lobby_events.empty()) {
                npMatching2LobbyCallback(&g_lobby_events.front());
                g_lobby_events.pop_front();
            }
        }
        if (npMatching2RoomCallback) {
            while (!g_room_events.empty()) {
                npMatching2RoomCallback(&g_room_events.front());
                g_room_events.pop_front();
            }
        }
    }

    std::scoped_lock lk{g_responses_mutex};
    while (!g_responses.empty()) {
        (g_responses.front())();
        g_responses.pop_front();
    }
}

struct OrbisNpMatching2InitializeParameter {
    u64 poolSize;
    //
};

int PS4_SYSV_ABI sceNpMatching2Initialize(OrbisNpMatching2InitializeParameter* param) {
    LOG_DEBUG(Lib_NpMatching2, "called");

    if (g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_ALREADY_INITIALIZED;
    }

    g_initialized = true;
    Libraries::Np::NpManager::RegisterNpCallback("NpMatching2", ProcessEvents);

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2Terminate() {
    LOG_DEBUG(Lib_NpMatching2, "called");

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }

    g_initialized = false;
    Libraries::Np::NpManager::DeregisterNpCallback("NpMatching2");

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SetDefaultRequestOptParam(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2RequestOptParam* requestOpt) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}", ctxId);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!requestOpt) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    defaultRequestOptParam = *requestOpt;

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2GetServerId(OrbisNpMatching2ContextId ctxId,
                                           OrbisNpMatching2ServerId* serverId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}", ctxId);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!serverId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    *serverId = 0xac;

    return ORBIS_OK;
}

struct OrbisNpMatching2GetWorldInfoListRequest {
    OrbisNpMatching2ServerId serverId;
};

struct OrbisNpMatching2World {
    OrbisNpMatching2World* next;
    OrbisNpMatching2WorldId worldId;
    u32 lobbiesNum;
    u32 maxLobbyMembersNum;
    u32 lobbyMembersNum;
    u32 roomsNum;
    u32 roomMembersNum;
    u8 pad[3];
};

struct OrbisNpMatching2GetWorldInfoListResponse {
    OrbisNpMatching2World* world;
    u64 worldNum;
};

int PS4_SYSV_ABI sceNpMatching2GetWorldInfoList(OrbisNpMatching2ContextId ctxId,
                                                OrbisNpMatching2GetWorldInfoListRequest* request,
                                                OrbisNpMatching2RequestOptParam* requestOpt,
                                                OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, request.serverId = {}, requestOpt = {}", ctxId,
              request ? request->serverId : 0xFFFF, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    static OrbisNpMatching2RequestId id = 1;
    *requestId = id++;

    if (auto optParam = GetOptParam(requestOpt); optParam) {
        LOG_DEBUG(Lib_NpMatching2, "optParam.timeout = {}, optParam.appId = {}", optParam->timeout,
                  optParam->appId);
        auto reqIdCopy = *requestId;
        std::scoped_lock lk{g_responses_mutex};
        g_responses.emplace_back([=]() {
            OrbisNpMatching2World w{nullptr, 1, 10, 0, 10, 0, {}};
            OrbisNpMatching2GetWorldInfoListResponse resp{&w, 1};
            optParam->callback(ctxId, reqIdCopy,
                               ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_WORLD_INFO_LIST, 0, &resp,
                               optParam->arg);
        });
    }

    return ORBIS_OK;
}

struct OrbisNpMatching2PresenceOptionData {
    u8 data[16];
    u64 len;
};

struct OrbisNpMatching2LeaveRoomRequest {
    OrbisNpMatching2RoomId roomId;
    OrbisNpMatching2PresenceOptionData optData;
};

int PS4_SYSV_ABI sceNpMatching2LeaveRoom(OrbisNpMatching2ContextId ctxId,
                                         OrbisNpMatching2LeaveRoomRequest* request,
                                         OrbisNpMatching2RequestOptParam* requestOpt,
                                         OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    static OrbisNpMatching2RequestId id = 500;
    *requestId = id++;

    if (auto optParam = GetOptParam(requestOpt); optParam) {
        LOG_DEBUG(Lib_NpMatching2, "optParam.timeout = {}, optParam.appId = {}", optParam->timeout,
                  optParam->appId);
        std::scoped_lock lk{g_responses_mutex};
        auto reqIdCopy = *requestId;
        g_responses.emplace_back([=]() {
            optParam->callback(ctxId, reqIdCopy, ORBIS_NP_MATCHING2_REQUEST_EVENT_LEAVE_ROOM, 0,
                               nullptr, optParam->arg);
        });
    }

    return ORBIS_OK;
}

struct OrbisNpMatching2RangeFilter {
    u32 start;
    u32 max;
};

struct OrbisNpMatching2SearchRoomRequest {
    int option;
    OrbisNpMatching2WorldId worldId;
    OrbisNpMatching2LobbyId lobbyId;
    OrbisNpMatching2RangeFilter rangeFilter;
    OrbisNpMatching2Flags flags1;
    OrbisNpMatching2Flags flags2;
    void* intFilter;
    u64 intFilters;
    void* binFilter;
    u64 binFilters;
    OrbisNpMatching2AttributeId* attr;
    u64 attrs;
};

struct OrbisNpMatching2Range {
    u32 start;
    u32 total;
    u32 results;
    u8 pad[4];
};

struct OrbisNpMatching2SearchRoomResponseA {
    OrbisNpMatching2Range range;
    void* roomDataExt;
};

int PS4_SYSV_ABI sceNpMatching2SearchRoom(OrbisNpMatching2ContextId ctxId,
                                          OrbisNpMatching2SearchRoomRequest* request,
                                          OrbisNpMatching2RequestOptParam* requestOpt,
                                          OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    static OrbisNpMatching2RequestId id = 1;
    *requestId = id++;

    if (auto optParam = GetOptParam(requestOpt); optParam) {
        LOG_DEBUG(Lib_NpMatching2, "optParam.timeout = {}, optParam.appId = {}", optParam->timeout,
                  optParam->appId);
        std::scoped_lock lk{g_responses_mutex};
        auto reqIdCopy = *requestId;
        auto requestCopy = *request;
        g_responses.emplace_back([=]() {
            OrbisNpMatching2SearchRoomResponseA resp{{0, 0, 0, {}}, nullptr};
            optParam->callback(ctxId, reqIdCopy, ORBIS_NP_MATCHING2_REQUEST_EVENT_SEARCH_ROOM_A, 0,
                               &resp, optParam->arg);
        });
    }

    return ORBIS_OK;
}

struct OrbisNpMatching2SetUserInfoRequest {
    OrbisNpMatching2ServerId serverId;
    u8 padding[6];
    void* userBinAttr;
    u64 userBinAttrs;
};

int PS4_SYSV_ABI sceNpMatching2SetUserInfo(OrbisNpMatching2ContextId ctxId,
                                           OrbisNpMatching2SetUserInfoRequest* request,
                                           OrbisNpMatching2RequestOptParam* requestOpt,
                                           OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    static OrbisNpMatching2RequestId id = 100;
    *requestId = id++;

    if (auto optParam = GetOptParam(requestOpt); optParam) {
        LOG_DEBUG(Lib_NpMatching2, "optParam.timeout = {}, optParam.appId = {}", optParam->timeout,
                  optParam->appId);
        std::scoped_lock lk{g_responses_mutex};
        auto reqIdCopy = *requestId;
        g_responses.emplace_back([=]() {
            optParam->callback(ctxId, reqIdCopy, ORBIS_NP_MATCHING2_REQUEST_EVENT_SET_USER_INFO, 0,
                               nullptr, optParam->arg);
        });
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SendRoomMessage(OrbisNpMatching2ContextId ctxId, void* request,
                                               OrbisNpMatching2RequestOptParam* requestOpt,
                                               OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    static OrbisNpMatching2RequestId id = 1000;
    *requestId = id++;

    if (auto optParam = GetOptParam(requestOpt); optParam) {
        LOG_DEBUG(Lib_NpMatching2, "optParam.timeout = {}, optParam.appId = {}", optParam->timeout,
                  optParam->appId);
        std::scoped_lock lk{g_responses_mutex};
        auto reqIdCopy = *requestId;
        g_responses.emplace_back([=]() {
            optParam->callback(ctxId, reqIdCopy, ORBIS_NP_MATCHING2_REQUEST_EVENT_SEND_ROOM_MESSAGE,
                               0, nullptr, optParam->arg);
        });
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SetRoomDataExternal(OrbisNpMatching2ContextId ctxId, void* request,
                                                   OrbisNpMatching2RequestOptParam* requestOpt,
                                                   OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    static OrbisNpMatching2RequestId id = 800;
    *requestId = id++;

    if (auto optParam = GetOptParam(requestOpt); optParam) {
        LOG_DEBUG(Lib_NpMatching2, "optParam.timeout = {}, optParam.appId = {}", optParam->timeout,
                  optParam->appId);
        std::scoped_lock lk{g_responses_mutex};
        auto reqIdCopy = *requestId;
        g_responses.emplace_back([=]() {
            optParam->callback(ctxId, reqIdCopy,
                               ORBIS_NP_MATCHING2_REQUEST_EVENT_SET_ROOM_DATA_EXTERNAL, 0, nullptr,
                               optParam->arg);
        });
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SetRoomDataInternal(OrbisNpMatching2ContextId ctxId, void* request,
                                                   OrbisNpMatching2RequestOptParam* requestOpt,
                                                   OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    static OrbisNpMatching2RequestId id = 200;
    *requestId = id++;

    if (auto optParam = GetOptParam(requestOpt); optParam) {
        LOG_DEBUG(Lib_NpMatching2, "optParam.timeout = {}, optParam.appId = {}", optParam->timeout,
                  optParam->appId);
        std::scoped_lock lk{g_responses_mutex};
        auto reqIdCopy = *requestId;
        g_responses.emplace_back([=]() {
            optParam->callback(ctxId, reqIdCopy,
                               ORBIS_NP_MATCHING2_REQUEST_EVENT_SET_ROOM_DATA_INTERNAL, 0, nullptr,
                               optParam->arg);
        });
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2ContextStop(OrbisNpMatching2ContextId ctxId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}", ctxId);
    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2DestroyContext(OrbisNpMatching2ContextId ctxId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}", ctxId);
    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2JoinRoom(OrbisNpMatching2ContextId ctxId, void* request,
                                        OrbisNpMatching2RequestOptParam* requestOpt,
                                        OrbisNpMatching2RequestId* requestId) {
    // No real matchmaking server exists to have a room to join -- reported as invalid
    // argument up front (matching the pattern of the null-request checks above) rather
    // than accepting a request whose response can never legitimately arrive.
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));
    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    static OrbisNpMatching2RequestId id = 1500;
    *requestId = id++;
    return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
}

// Room-membership actions below (grant owner, kick, per-member data, chat) all target a
// specific other room member -- something that can't exist without real network
// matchmaking. Accepted and reported successful with no lasting effect (there is nothing
// to fail against with only the local player ever in a room), same honest-limitation
// approach as CreateJoinRoomA's single-member room.
int PS4_SYSV_ABI sceNpMatching2GrantRoomOwner(OrbisNpMatching2ContextId ctxId, void* request,
                                              OrbisNpMatching2RequestOptParam* requestOpt,
                                              OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));
    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    static OrbisNpMatching2RequestId id = 1600;
    *requestId = id++;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2KickoutRoomMember(OrbisNpMatching2ContextId ctxId, void* request,
                                                 OrbisNpMatching2RequestOptParam* requestOpt,
                                                 OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));
    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    static OrbisNpMatching2RequestId id = 1700;
    *requestId = id++;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SetRoomMemberDataInternal(OrbisNpMatching2ContextId ctxId,
                                                         void* request,
                                                         OrbisNpMatching2RequestOptParam* requestOpt,
                                                         OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));
    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    static OrbisNpMatching2RequestId id = 1800;
    *requestId = id++;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SendRoomChatMessage(OrbisNpMatching2ContextId ctxId, void* request,
                                                   OrbisNpMatching2RequestOptParam* requestOpt,
                                                   OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));
    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    static OrbisNpMatching2RequestId id = 1900;
    *requestId = id++;
    return ORBIS_OK;
}

// Stored but never invoked: no real remote room member will ever send a message for this
// to deliver (matches sceNpMatching2RegisterRoomEventCallback's storage pattern above,
// but there is no ProcessEvents queue feeding it since nothing produces chat events).
std::function<void(OrbisNpMatching2ContextId, OrbisNpMatching2RoomId, void*, void*)>
    npMatching2RoomMessageCallback = nullptr;

int PS4_SYSV_ABI sceNpMatching2RegisterRoomMessageCallback(
    OrbisNpMatching2ContextId ctxId,
    void PS4_SYSV_ABI (*callback)(OrbisNpMatching2ContextId, OrbisNpMatching2RoomId, void*, void*,
                                  void*),
    void* userdata) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, userdata = {}", ctxId, userdata);
    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    npMatching2RoomMessageCallback = [callback, userdata](auto ctx, auto room, auto from,
                                                          auto data) {
        callback(ctx, room, from, data, userdata);
    };
    return ORBIS_OK;
}

struct OrbisNpMatching2SignalingConnectionInfo {
    s32 connStatus;
    u32 peerAddr;
    u16 peerPort;
    u8 padding[2];
};

int PS4_SYSV_ABI sceNpMatching2SignalingGetConnectionStatus(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2RoomId roomId,
    OrbisNpMatching2RoomMemberId peerMemberId, s32* connStatus, u32* peerAddr, u16* peerPort) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, roomId = {}, peerMemberId = {}", ctxId, roomId,
              peerMemberId);
    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    // No real P2P signaling is implemented -- report "not connected" (0) rather than
    // fabricating a connected state the game would then try to actually use.
    if (connStatus != nullptr) {
        *connStatus = 0;
    }
    if (peerAddr != nullptr) {
        *peerAddr = 0;
    }
    if (peerPort != nullptr) {
        *peerPort = 0;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SignalingGetLocalNetInfo(OrbisNpMatching2ContextId ctxId,
                                                        void* netInfo) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, netInfo = {}", ctxId, netInfo);
    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (netInfo == nullptr) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    // No real NAT/network-type detection is implemented; zeroed output rather than a
    // guessed struct layout beyond what the connection-status accessors above return.
    std::memset(netInfo, 0, 64);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SignalingGetPingInfo(OrbisNpMatching2ContextId ctxId,
                                                     OrbisNpMatching2RoomId roomId,
                                                     OrbisNpMatching2RoomMemberId peerMemberId,
                                                     void* pingInfo) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, roomId = {}, peerMemberId = {}", ctxId, roomId,
              peerMemberId);
    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (pingInfo == nullptr) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    std::memset(pingInfo, 0, 32);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2CreateJoinRoom(OrbisNpMatching2ContextId ctxId, void* request,
                                              OrbisNpMatching2RequestOptParam* requestOpt,
                                              OrbisNpMatching2RequestId* requestId) {
    // Older (pre-"A" suffix) struct variant of sceNpMatching2CreateJoinRoomA. Rather than
    // duplicate that function's real single-member room construction against a second,
    // unverified struct layout, delegate the request-id/init bookkeeping here and let the
    // caller's own optParam callback (if any) fire with a generic success -- accurate
    // enough for games using the older API purely to establish a local-only session.
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));
    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    static OrbisNpMatching2RequestId id = 2000;
    *requestId = id++;
    if (auto optParam = GetOptParam(requestOpt); optParam) {
        std::scoped_lock lk{g_responses_mutex};
        auto reqIdCopy = *requestId;
        g_responses.emplace_back([=]() {
            optParam->callback(ctxId, reqIdCopy, ORBIS_NP_MATCHING2_REQUEST_EVENT_CREATE_JOIN_ROOM,
                               0, nullptr, optParam->arg);
        });
    }
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("10t3e5+JPnU", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2Initialize);
    LIB_FUNCTION("Mqp3lJ+sjy4", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2Terminate);
    LIB_FUNCTION("YfmpW719rMo", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2CreateContext);
    LIB_FUNCTION("ajvzc8e2upo", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2CreateContextA);
    LIB_FUNCTION("V6KSpKv9XJE", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2CreateJoinRoomA);
    LIB_FUNCTION("fQQfP87I7hs", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2RegisterContextCallback);
    LIB_FUNCTION("4Nj7u5B5yCA", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2RegisterLobbyEventCallback);
    LIB_FUNCTION("p+2EnxmaAMM", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2RegisterRoomEventCallback);
    LIB_FUNCTION("0UMeWRGnZKA", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2RegisterSignalingCallback);
    LIB_FUNCTION("7vjNQ6Z1op0", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2ContextStart);
    LIB_FUNCTION("LhCPctIICxQ", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetServerId);
    LIB_FUNCTION("rJNPJqDCpiI", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetWorldInfoList);
    LIB_FUNCTION("BD6kfx442Do", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2LeaveRoom);
    LIB_FUNCTION("+8e7wXLmjds", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SetDefaultRequestOptParam);
    LIB_FUNCTION("VqZX7POg2Mk", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SearchRoom);
    LIB_FUNCTION("Iw2h0Jrrb5U", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SendRoomMessage);
    LIB_FUNCTION("meEjIdbjAA0", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SetUserInfo);
    LIB_FUNCTION("q7GK98-nYSE", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SetRoomDataExternal);
    LIB_FUNCTION("S9D8JSYIrjE", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SetRoomDataInternal);
    LIB_FUNCTION("-f6M4caNe8k", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2ContextStop);
    LIB_FUNCTION("Nz-ZE7ur32I", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2DestroyContext);
    LIB_FUNCTION("CSIMDsVjs-g", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2JoinRoom);
    LIB_FUNCTION("NCP3bLGPt+o", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GrantRoomOwner);
    LIB_FUNCTION("AUVfU6byg3c", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2KickoutRoomMember);
    LIB_FUNCTION("HoqTrkS9c5Q", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SetRoomMemberDataInternal);
    LIB_FUNCTION("opDpl74pi2E", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SendRoomChatMessage);
    LIB_FUNCTION("uBESzz4CQws", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2RegisterRoomMessageCallback);
    LIB_FUNCTION("tHD5FPFXtu4", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SignalingGetConnectionStatus);
    LIB_FUNCTION("380EWm2DrVg", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SignalingGetLocalNetInfo);
    LIB_FUNCTION("wUmwXZHaX1w", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SignalingGetPingInfo);
    LIB_FUNCTION("zCWZmXXN600", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2CreateJoinRoom);
};

} // namespace Libraries::Np::NpMatching2