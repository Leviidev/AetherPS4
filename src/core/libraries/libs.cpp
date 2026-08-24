// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "core/libraries/ajm/ajm.h"
#include "core/libraries/app_content/app_content.h"
#include "core/libraries/audio/audioin.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio3d/audio3d.h"
#include "core/libraries/audio3d/audio3d_openal.h"
#include "core/libraries/avplayer/avplayer.h"
#include "core/libraries/camera/camera.h"
#include "core/libraries/companion/companion_httpd.h"
#include "core/libraries/companion/companion_util.h"
#include "core/libraries/content_export/content_export.h"
#include "core/libraries/disc_map/disc_map.h"
#include "core/libraries/fiber/fiber.h"
#include "core/libraries/font/font.h"
#include "core/libraries/font/fontft.h"
#include "core/libraries/game_live_streaming/gamelivestreaming.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/hmd/hmd_setup_dialog.h"
#include "core/libraries/ime/error_dialog.h"
#include "core/libraries/ime/ime.h"
#include "core/libraries/ime/ime_dialog.h"
#include "core/libraries/jpeg/jpegenc.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/libc_internal/libc_internal.h"
#include "core/libraries/libpng/pngdec.h"
#include "core/libraries/libpng/pngenc.h"
#include "core/libraries/libs.h"
#include "core/libraries/mouse/mouse.h"
#include "core/libraries/move/move.h"
#include "core/libraries/network/http.h"
#include "core/libraries/network/http2.h"
#include "core/libraries/network/net.h"
#include "core/libraries/network/netctl.h"
#include "core/libraries/network/ssl.h"
#include "core/libraries/network/ssl2.h"
#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/np/np_auth.h"
#include "core/libraries/np/np_commerce.h"
#include "core/libraries/np/np_common.h"
#include "core/libraries/np/np_manager.h"
#include "core/libraries/np/np_matching2.h"
#include "core/libraries/np/np_misc_apis.h"
#include "core/libraries/np/np_misc_dialogs.h"
#include "core/libraries/np/np_partner.h"
#include "core/libraries/np/np_party.h"
#include "core/libraries/np/np_profile_dialog/np_profile_dialog.h"
#include "core/libraries/np/np_score/np_score.h"
#include "core/libraries/np/np_signaling/np_signaling.h"
#include "core/libraries/np/np_sns_facebook_dialog.h"
#include "core/libraries/np/np_trophy.h"
#include "core/libraries/np/np_tus.h"
#include "core/libraries/np/np_web_api/np_web_api.h"
#include "core/libraries/np/np_web_api2.h"
#include "core/libraries/pad/pad.h"
#include "core/libraries/playgo/playgo.h"
#include "core/libraries/playgo/playgo_dialog.h"
#include "core/libraries/random/random.h"
#include "core/libraries/razor_cpu/razor_cpu.h"
#include "core/libraries/remote_play/remoteplay.h"
#include "core/libraries/rtc/rtc.h"
#include "core/libraries/rudp/rudp.h"
#include "core/libraries/save_data/dialog/savedatadialog.h"
#include "core/libraries/save_data/savedata.h"
#include "core/libraries/screenshot/screenshot.h"
#include "core/libraries/share_play/shareplay.h"
#include "core/libraries/signin_dialog/signindialog.h"
#include "core/libraries/system_gesture/system_gesture.h"
#include "core/libraries/sysmodule/sysmodule.h"
#include "core/libraries/system/commondialog.h"
#include "core/libraries/system/msgdialog.h"
#include "core/libraries/system/posix.h"
#include "core/libraries/system/systemservice.h"
#include "core/libraries/system/userservice.h"
#include "core/libraries/ulobjmgr/ulobjmgr.h"
#ifndef SHADPS4_DISABLE_USBD
#include "core/libraries/usbd/usbd.h"
#endif
#include "core/libraries/video_recording/video_recording.h"
#include "core/libraries/videodec/videodec.h"
#include "core/libraries/videodec/videodec2.h"
#include "core/libraries/videoout/video_out.h"
#include "core/libraries/voice/voice.h"
#include "core/libraries/vr_tracker/vr_tracker.h"
#include "core/libraries/web_browser_dialog/webbrowserdialog.h"
#include "core/libraries/zlib/zlib_sce.h"

namespace Libraries {

void InitHLELibs(Core::Loader::SymbolsResolver* sym) {
    LOG_INFO(Lib_Kernel, "Initializing HLE libraries");
    Libraries::Kernel::RegisterLib(sym);
    Libraries::LibcInternal::ForceRegisterLib(sym);
    // The same gap as libSceRtc/libSceNgs2 below, but far higher impact: RegisterLib (as
    // opposed to ForceRegisterLib just above, which only covers the FEX-interception "libc"
    // tag and one forced Io override) registers every math/string/memory/threads/CRT
    // function under the actual "libSceLibcInternal" tag PS4 games import against -- and
    // was ONLY ever reachable via the sysmodule-load-gated table in
    // sysmodule_internal.cpp. Since libSceLibcInternal is a core system library nearly
    // every commercial title links against, any game that doesn't happen to call
    // sceSysmoduleLoadModule("libSceLibcInternal.sprx") explicitly got ENOSYS for all of
    // it -- every function implemented under this tag this session was silently
    // unreachable until now.
    Libraries::LibcInternal::RegisterLib(sym);
    Libraries::GnmDriver::RegisterLib(sym);
    Libraries::VideoOut::RegisterLib(sym);
    Libraries::UserService::RegisterLib(sym);
    Libraries::SystemService::RegisterLib(sym);
    Libraries::CommonDialog::RegisterLib(sym);
    Libraries::MsgDialog::RegisterLib(sym);
    Libraries::AudioOut::RegisterLib(sym);
    // Same class of gap as libSceRtc above: fully implemented but only ever registered
    // via the sysmodule-load-gated table, so any game using NGS2 audio without an
    // explicit sceSysmoduleLoadModule("libSceNgs2.sprx") call got ENOSYS for every
    // sceNgs2* function.
    Libraries::Ngs2::RegisterLib(sym);
    Libraries::Http::RegisterLib(sym);
    Libraries::Http2::RegisterLib(sym);
    Libraries::Net::RegisterLib(sym);
    Libraries::NetCtl::RegisterLib(sym);
    Libraries::SaveData::RegisterLib(sym);
    Libraries::SaveData::Dialog::RegisterLib(sym);
    Libraries::Ssl2::RegisterLib(sym);
    Libraries::SysModule::RegisterLib(sym);
    Libraries::Posix::RegisterLib(sym);
    Libraries::AudioIn::RegisterLib(sym);
    Libraries::Np::NpCommerce::RegisterLib(sym);
    Libraries::Np::NpCommon::RegisterLib(sym);
    Libraries::Np::NpManager::RegisterLib(sym);
    Libraries::Np::NpMatching2::RegisterLib(sym);
    Libraries::Np::NpSignaling::RegisterLib(sym);
    Libraries::Np::NpScore::RegisterLib(sym);
    Libraries::Np::NpTrophy::RegisterLib(sym);
    Libraries::Np::NpWebApi::RegisterLib(sym);
    Libraries::Np::NpWebApi2::RegisterLib(sym);
    Libraries::Np::NpProfileDialog::RegisterLib(sym);
    Libraries::Np::NpSnsFacebookDialog::RegisterLib(sym);
    Libraries::Np::NpAuth::RegisterLib(sym);
    Libraries::Np::NpParty::RegisterLib(sym);
    Libraries::Np::NpPartner::RegisterLib(sym);
    Libraries::Np::NpTus::RegisterLib(sym);
    Libraries::Np::MiscDialogs::RegisterLib(sym);
    Libraries::Np::MiscApis::RegisterLib(sym);
    Libraries::ScreenShot::RegisterLib(sym);
    Libraries::AppContent::RegisterLib(sym);
    Libraries::PngDec::RegisterLib(sym);
    Libraries::PlayGo::RegisterLib(sym);
    Libraries::PlayGo::Dialog::RegisterLib(sym);
    Libraries::Random::RegisterLib(sym);
#ifndef SHADPS4_DISABLE_USBD
    Libraries::Usbd::RegisterLib(sym);
#endif
    Libraries::Pad::RegisterLib(sym);
    Libraries::Ajm::RegisterLib(sym);
    Libraries::ErrorDialog::RegisterLib(sym);
    Libraries::ImeDialog::RegisterLib(sym);
    Libraries::AvPlayer::RegisterLib(sym);
    Libraries::Videodec::RegisterLib(sym);
    Libraries::Videodec2::RegisterLib(sym);
    if (EmulatorSettings.GetAudioBackend() == AudioBackend::OpenAL) {
        Libraries::Audio3dOpenAL::RegisterLib(sym);
    } else {
        Libraries::Audio3d::RegisterLib(sym);
    }
    Libraries::Ime::RegisterLib(sym);
    Libraries::GameLiveStreaming::RegisterLib(sym);
    Libraries::SharePlay::RegisterLib(sym);
    Libraries::Remoteplay::RegisterLib(sym);
    Libraries::RazorCpu::RegisterLib(sym);
    Libraries::Move::RegisterLib(sym);
#ifdef ARCH_X86_64
    Libraries::Fiber::RegisterLib(sym);
#endif
    Libraries::Mouse::RegisterLib(sym);
    Libraries::WebBrowserDialog::RegisterLib(sym);
    Libraries::Zlib::RegisterLib(sym);
    Libraries::Hmd::RegisterLib(sym);
    Libraries::HmdSetupDialog::RegisterLib(sym);
    Libraries::DiscMap::RegisterLib(sym);
    Libraries::Ulobjmgr::RegisterLib(sym);
    Libraries::SigninDialog::RegisterLib(sym);
    Libraries::Camera::RegisterLib(sym);
    Libraries::CompanionHttpd::RegisterLib(sym);
    Libraries::CompanionUtil::RegisterLib(sym);
    Libraries::Voice::RegisterLib(sym);
    Libraries::Rudp::RegisterLib(sym);
    Libraries::VrTracker::RegisterLib(sym);
    Libraries::ContentExport::RegisterLib(sym);
    Libraries::VideoRecording::RegisterLib(sym);
    // Real Time Clock is a basic utility library like the ones above -- most games call
    // it without ever explicitly calling sceSysmoduleLoadModule("libSceRtc.sprx") first
    // (confirmed on-device: Rocket League statically imports sceRtcGetCurrentClock etc.
    // but never loads the module, so the sysmodule-table-gated registration in
    // sysmodule_internal.cpp never fires and every RTC call falls through to ENOSYS).
    Libraries::Rtc::RegisterLib(sym);
    // Same gated-but-fully-implemented pattern as Rtc/Ngs2/LibcInternal above, found by
    // cross-referencing sysmodule_internal.cpp's gated table against this list: these four
    // were never registered anywhere except behind an explicit sceSysmoduleLoadModule call.
    Libraries::JpegEnc::RegisterLib(sym);
    Libraries::PngEnc::RegisterLib(sym);
    Libraries::Font::RegisterlibSceFont(sym);
    Libraries::FontFt::RegisterlibSceFontFt(sym);
    Libraries::SystemGesture::RegisterLib(sym);

    // Loading libSceSsl is locked behind a title workaround that currently applies to nothing.
    // Libraries::Ssl::RegisterLib(sym);
}

} // namespace Libraries
