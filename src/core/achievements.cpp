// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

// TODO: Don't poll when booting the game, e.g. Crash Warped freaks out.
// TODO: rc_client_begin_change_media

#define IMGUI_DEFINE_MATH_OPERATORS

#include "achievements.h"
#include "bios.h"
#include "bus.h"
#include "cpu_core.h"
#include "fullscreen_ui.h"
#include "host.h"
#include "system.h"

#include "scmversion/scmversion.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/http_downloader.h"
#include "common/log.h"
#include "common/md5_digest.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include "util/cd_image.h"
#include "util/imgui_fullscreen.h"
#include "util/imgui_manager.h"
#include "util/platform_misc.h"
#include "util/state_wrapper.h"

#include "IconsFontAwesome5.h"
#include "fmt/format.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "rc_client.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

Log_SetChannel(Achievements);

#ifdef ENABLE_RAINTEGRATION
// RA_Interface ends up including windows.h, with its silly macros.
#ifdef _WIN32
#include "common/windows_headers.h"
#endif
#include "RA_Interface.h"
#endif
namespace Achievements {

static constexpr const char* INFO_SOUND_NAME = "sounds/achievements/message.wav";
static constexpr const char* UNLOCK_SOUND_NAME = "sounds/achievements/unlock.wav";
static constexpr const char* LBSUBMIT_SOUND_NAME = "sounds/achievements/lbsubmit.wav";

static constexpr u32 LEADERBOARD_NEARBY_ENTRIES_TO_FETCH = 10;
static constexpr u32 LEADERBOARD_ALL_FETCH_SIZE = 20;

static constexpr float LOGIN_NOTIFICATION_TIME = 5.0f;
static constexpr float ACHIEVEMENT_SUMMARY_NOTIFICATION_TIME = 5.0f;
static constexpr float GAME_COMPLETE_NOTIFICATION_TIME = 20.0f;
static constexpr float LEADERBOARD_STARTED_NOTIFICATION_TIME = 3.0f;
static constexpr float LEADERBOARD_FAILED_NOTIFICATION_TIME = 3.0f;

static constexpr float INDICATOR_FADE_IN_TIME = 0.1f;
static constexpr float INDICATOR_FADE_OUT_TIME = 0.5f;

namespace {
struct LoginWithPasswordParameters
{
  const char* username;
  Error* error;
  rc_client_async_handle_t* request;
  bool result;
};
struct LeaderboardTrackerIndicator
{
  u32 tracker_id;
  ImVec2 size;
  std::string text;
  Common::Timer show_hide_time;
  bool active;
};

struct AchievementChallengeIndicator
{
  const rc_client_achievement_t* achievement;
  std::string badge_path;
  Common::Timer show_hide_time;
  bool active;
};

struct AchievementProgressIndicator
{
  const rc_client_achievement_t* achievement;
  std::string badge_path;
  Common::Timer show_hide_time;
  bool active;
};
} // namespace

static void ReportError(const std::string_view& sv);
template<typename... T>
static void ReportFmtError(fmt::format_string<T...> fmt, T&&... args);
template<typename... T>
static void ReportRCError(int err, fmt::format_string<T...> fmt, T&&... args);
static void EnsureCacheDirectoriesExist();
static void ClearGameInfo();
static void ClearGameHash();
static std::string GetUserAgent();
static std::string GetGameHash(CDImage* image);
static void SetHardcoreMode(bool enabled, bool force_display_message);
static bool IsLoggedInOrLoggingIn();
static void ShowLoginSuccess(const rc_client_t* client);
static void ShowLoginNotification();
static void IdentifyGame(const std::string& path, CDImage* image);
static void BeginLoadGame();
static void UpdateGameSummary();
static void DownloadImage(std::string url, std::string cache_filename);

static bool CreateClient(rc_client_t** client, std::unique_ptr<Common::HTTPDownloader>* http);
static void DestroyClient(rc_client_t** client, std::unique_ptr<Common::HTTPDownloader>* http);
static void ClientMessageCallback(const char* message, const rc_client_t* client);
static uint32_t ClientReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client);
static void ClientServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data,
                             rc_client_t* client);

static void ClientEventHandler(const rc_client_event_t* event, rc_client_t* client);
static void HandleResetEvent(const rc_client_event_t* event);
static void HandleUnlockEvent(const rc_client_event_t* event);
static void HandleGameCompleteEvent(const rc_client_event_t* event);
static void HandleLeaderboardStartedEvent(const rc_client_event_t* event);
static void HandleLeaderboardFailedEvent(const rc_client_event_t* event);
static void HandleLeaderboardSubmittedEvent(const rc_client_event_t* event);
static void HandleLeaderboardScoreboardEvent(const rc_client_event_t* event);
static void HandleLeaderboardTrackerShowEvent(const rc_client_event_t* event);
static void HandleLeaderboardTrackerHideEvent(const rc_client_event_t* event);
static void HandleLeaderboardTrackerUpdateEvent(const rc_client_event_t* event);
static void HandleAchievementChallengeIndicatorShowEvent(const rc_client_event_t* event);
static void HandleAchievementChallengeIndicatorHideEvent(const rc_client_event_t* event);
static void HandleAchievementProgressIndicatorShowEvent(const rc_client_event_t* event);
static void HandleAchievementProgressIndicatorHideEvent(const rc_client_event_t* event);
static void HandleAchievementProgressIndicatorUpdateEvent(const rc_client_event_t* event);
static void HandleServerErrorEvent(const rc_client_event_t* event);
static void HandleServerDisconnectedEvent(const rc_client_event_t* event);
static void HandleServerReconnectedEvent(const rc_client_event_t* event);

static void ClientLoginWithTokenCallback(int result, const char* error_message, rc_client_t* client, void* userdata);
static void ClientLoginWithPasswordCallback(int result, const char* error_message, rc_client_t* client, void* userdata);
static void ClientLoadGameCallback(int result, const char* error_message, rc_client_t* client, void* userdata);

static void DisplayHardcoreDeferredMessage();
static void DisplayAchievementSummary();
static void UpdateRichPresence(std::unique_lock<std::recursive_mutex>& lock);

static std::string GetAchievementBadgePath(const rc_client_achievement_t* achievement, int state);
static std::string GetUserBadgePath(const std::string_view& username);
static std::string GetLeaderboardUserBadgePath(const rc_client_leaderboard_entry_t* entry);

static void DrawAchievement(const rc_client_achievement_t* cheevo);
static void DrawLeaderboardListEntry(const rc_client_leaderboard_t* lboard);
static void DrawLeaderboardEntry(const rc_client_leaderboard_entry_t& entry, bool is_self, float rank_column_width,
                                 float name_column_width, float time_column_width, float column_spacing);
static void OpenLeaderboard(const rc_client_leaderboard_t* lboard);
static void LeaderboardFetchNearbyCallback(int result, const char* error_message,
                                           rc_client_leaderboard_entry_list_t* list, rc_client_t* client,
                                           void* callback_userdata);
static void LeaderboardFetchAllCallback(int result, const char* error_message, rc_client_leaderboard_entry_list_t* list,
                                        rc_client_t* client, void* callback_userdata);
static void FetchNextLeaderboardEntries();
static void CloseLeaderboard();

static bool s_hardcore_mode = false;

#ifdef ENABLE_RAINTEGRATION
static bool s_using_raintegration = false;
#endif

static std::recursive_mutex s_achievements_mutex;
static rc_client_t* s_client;
static std::string s_image_directory;
static std::unique_ptr<Common::HTTPDownloader> s_http_downloader;

static std::string s_game_path;
static std::string s_game_hash;
static std::string s_game_title;
static std::string s_game_icon;
static rc_client_user_game_summary_t s_game_summary;
static u32 s_game_id = 0;

static bool s_has_achievements = false;
static bool s_has_leaderboards = false;
static bool s_has_rich_presence = false;
static std::string s_rich_presence_string;
static Common::Timer s_rich_presence_poll_time;

static rc_client_async_handle_t* s_login_request;
static rc_client_async_handle_t* s_load_game_request;

static rc_client_achievement_list_t* s_achievement_list;
static rc_client_leaderboard_list_t* s_leaderboard_list;
static std::vector<std::pair<const void*, std::string>> s_achievement_badge_paths;
static const rc_client_leaderboard_t* s_open_leaderboard = nullptr;
static rc_client_async_handle_t* s_leaderboard_fetch_handle = nullptr;
static std::vector<rc_client_leaderboard_entry_list_t*> s_leaderboard_entry_lists;
static rc_client_leaderboard_entry_list_t* s_leaderboard_nearby_entries;
static std::vector<std::pair<const rc_client_leaderboard_entry_t*, std::string>> s_leaderboard_user_icon_paths;
static bool s_is_showing_all_leaderboard_entries = false;

static std::vector<LeaderboardTrackerIndicator> s_active_leaderboard_trackers;
static std::vector<AchievementChallengeIndicator> s_active_challenge_indicators;
static std::optional<AchievementProgressIndicator> s_active_progress_indicator;
} // namespace Achievements

std::unique_lock<std::recursive_mutex> Achievements::GetLock()
{
  return std::unique_lock(s_achievements_mutex);
}

std::string Achievements::GetUserAgent()
{
  return fmt::format("DuckStation for {} ({}) {}", TARGET_OS_STR, CPU_ARCH_STR, g_scm_tag_str);
}

void Achievements::ReportError(const std::string_view& sv)
{
  std::string error = fmt::format("Achievements error: {}", sv);
  Log_ErrorPrint(error.c_str());
  Host::AddOSDMessage(std::move(error), Host::OSD_CRITICAL_ERROR_DURATION);
}

template<typename... T>
void Achievements::ReportFmtError(fmt::format_string<T...> fmt, T&&... args)
{
  TinyString str;
  fmt::vformat_to(std::back_inserter(str), fmt, fmt::make_format_args(args...));
  ReportError(str);
}

template<typename... T>
void Achievements::ReportRCError(int err, fmt::format_string<T...> fmt, T&&... args)
{
  TinyString str;
  fmt::vformat_to(std::back_inserter(str), fmt, fmt::make_format_args(args...));
  str.append_fmt("{} ({})", rc_error_str(err), err);
  ReportError(str);
}

std::string Achievements::GetGameHash(CDImage* image)
{
  std::string executable_name;
  std::vector<u8> executable_data;
  if (!System::ReadExecutableFromImage(image, &executable_name, &executable_data))
    return {};

  BIOS::PSEXEHeader header;
  if (executable_data.size() >= sizeof(header))
    std::memcpy(&header, executable_data.data(), sizeof(header));
  if (!BIOS::IsValidPSExeHeader(header, static_cast<u32>(executable_data.size())))
  {
    Log_ErrorFmt("PS-EXE header is invalid in '{}' ({} bytes)", executable_name, executable_data.size());
    return {};
  }

  // See rcheevos hash.c - rc_hash_psx().
  const u32 MAX_HASH_SIZE = 64 * 1024 * 1024;
  const u32 hash_size = std::min<u32>(sizeof(header) + header.file_size, MAX_HASH_SIZE);
  Assert(hash_size <= executable_data.size());

  MD5Digest digest;
  digest.Update(executable_name.c_str(), static_cast<u32>(executable_name.size()));
  if (hash_size > 0)
    digest.Update(executable_data.data(), hash_size);

  u8 hash[16];
  digest.Final(hash);

  const std::string hash_str =
    fmt::format("{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7], hash[8], hash[9], hash[10],
                hash[11], hash[12], hash[13], hash[14], hash[15]);

  Log_InfoFmt("Hash for '{}' ({} bytes, {} bytes hashed): {}", executable_name, executable_data.size(), hash_size,
              hash_str);
  return hash_str;
}

void Achievements::DownloadImage(std::string url, std::string cache_filename)
{
  auto callback = [cache_filename](s32 status_code, std::string content_type,
                                   Common::HTTPDownloader::Request::Data data) {
    if (status_code != Common::HTTPDownloader::HTTP_OK)
      return;

    if (!FileSystem::WriteBinaryFile(cache_filename.c_str(), data.data(), data.size()))
    {
      Log_ErrorFmt("Failed to write badge image to '{}'", cache_filename);
      return;
    }

    ImGuiFullscreen::InvalidateCachedTexture(cache_filename);
  };

  s_http_downloader->CreateRequest(std::move(url), std::move(callback));
}

bool Achievements::IsActive()
{
#ifdef ENABLE_RAINTEGRATION
  return (s_client != nullptr) || s_using_raintegration;
#else
  return (s_client != nullptr);
#endif
}

bool Achievements::IsHardcoreModeActive()
{
#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
    return RA_HardcoreModeIsActive() != 0;
#endif

  return s_hardcore_mode;
}

bool Achievements::HasActiveGame()
{
  return s_game_id != 0;
}

u32 Achievements::GetGameID()
{
  return s_game_id;
}

bool Achievements::HasAchievementsOrLeaderboards()
{
  return s_has_achievements || s_has_leaderboards;
}

bool Achievements::HasAchievements()
{
  return s_has_achievements;
}

bool Achievements::HasLeaderboards()
{
  return s_has_leaderboards;
}

bool Achievements::HasRichPresence()
{
  return s_has_rich_presence;
}

const std::string& Achievements::GetGameTitle()
{
  return s_game_title;
}

const std::string& Achievements::GetRichPresenceString()
{
  return s_rich_presence_string;
}

bool Achievements::Initialize()
{
  if (IsUsingRAIntegration())
    return true;

  EnsureCacheDirectoriesExist();

  auto lock = GetLock();
  AssertMsg(g_settings.achievements_enabled, "Achievements are enabled");
  Assert(!s_client && !s_http_downloader);

  if (!CreateClient(&s_client, &s_http_downloader))
    return false;

  // Hardcore starts off. We enable it on first boot.
  s_hardcore_mode = false;

  rc_client_set_event_handler(s_client, ClientEventHandler);

  rc_client_set_hardcore_enabled(s_client, s_hardcore_mode);
  rc_client_set_encore_mode_enabled(s_client, g_settings.achievements_encore_mode);
  rc_client_set_unofficial_enabled(s_client, g_settings.achievements_unofficial_test_mode);
  rc_client_set_spectator_mode_enabled(s_client, g_settings.achievements_spectator_mode);

  // Begin disc identification early, before the login finishes.
  if (System::IsValid())
    IdentifyGame(System::GetDiscPath(), nullptr);

  std::string username = Host::GetBaseStringSettingValue("Cheevos", "Username");
  std::string api_token = Host::GetBaseStringSettingValue("Cheevos", "Token");
  if (!username.empty() && !api_token.empty())
  {
    Log_InfoPrintf("Attempting login with user '%s'...", username.c_str());
    s_login_request = rc_client_begin_login_with_token(s_client, username.c_str(), api_token.c_str(),
                                                       ClientLoginWithTokenCallback, nullptr);
  }

  // Hardcore mode isn't enabled when achievements first starts, if a game is already running.
  if (System::IsValid() && IsLoggedInOrLoggingIn() && g_settings.achievements_hardcore_mode)
    DisplayHardcoreDeferredMessage();

  return true;
}

bool Achievements::CreateClient(rc_client_t** client, std::unique_ptr<Common::HTTPDownloader>* http)
{
  *http = Common::HTTPDownloader::Create(GetUserAgent().c_str());
  if (!*http)
  {
    Host::ReportErrorAsync("Achievements Error", "Failed to create HTTPDownloader, cannot use achievements");
    return false;
  }

  rc_client_t* new_client = rc_client_create(ClientReadMemory, ClientServerCall);
  if (!new_client)
  {
    Host::ReportErrorAsync("Achievements Error", "rc_client_create() failed, cannot use achievements");
    http->reset();
    return false;
  }

#ifdef _DEBUG
  rc_client_enable_logging(new_client, RC_CLIENT_LOG_LEVEL_VERBOSE, ClientMessageCallback);
#else
  rc_client_enable_logging(new_client, RC_CLIENT_LOG_LEVEL_INFO, ClientMessageCallback);
#endif

  rc_client_set_userdata(new_client, http->get());

  *client = new_client;
  return true;
}

void Achievements::DestroyClient(rc_client_t** client, std::unique_ptr<Common::HTTPDownloader>* http)
{
  (*http)->WaitForAllRequests();

  rc_client_destroy(*client);
  *client = nullptr;

  http->reset();
}

void Achievements::UpdateSettings(const Settings& old_config)
{
  if (IsUsingRAIntegration())
    return;

  if (!g_settings.achievements_enabled)
  {
    // we're done here
    Shutdown(false);
    return;
  }

  if (!IsActive())
  {
    // we just got enabled
    Initialize();
    return;
  }

  if (g_settings.achievements_hardcore_mode != old_config.achievements_hardcore_mode)
  {
    // Hardcore mode can only be enabled through reset (ResetChallengeMode()).
    if (s_hardcore_mode && !g_settings.achievements_hardcore_mode)
    {
      ResetHardcoreMode();
    }
    else if (!s_hardcore_mode && g_settings.achievements_hardcore_mode)
    {
      if (HasActiveGame())
        DisplayHardcoreDeferredMessage();
    }
  }

  // These cannot be modified while a game is loaded, so just toss state and reload.
  if (HasActiveGame())
  {
    if (g_settings.achievements_encore_mode != old_config.achievements_encore_mode ||
        g_settings.achievements_spectator_mode != old_config.achievements_spectator_mode ||
        g_settings.achievements_unofficial_test_mode != old_config.achievements_unofficial_test_mode)
    {
      Shutdown(false);
      Initialize();
      return;
    }
  }
  else
  {
    if (g_settings.achievements_encore_mode != old_config.achievements_encore_mode)
      rc_client_set_encore_mode_enabled(s_client, g_settings.achievements_encore_mode);
    if (g_settings.achievements_spectator_mode != old_config.achievements_spectator_mode)
      rc_client_set_spectator_mode_enabled(s_client, g_settings.achievements_spectator_mode);
    if (g_settings.achievements_unofficial_test_mode != old_config.achievements_unofficial_test_mode)
      rc_client_set_unofficial_enabled(s_client, g_settings.achievements_unofficial_test_mode);
  }

  // in case cache directory changed
  EnsureCacheDirectoriesExist();
}

bool Achievements::Shutdown(bool allow_cancel)
{
#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    if (System::IsValid() && allow_cancel && !RA_ConfirmLoadNewRom(true))
      return false;

    RA_SetPaused(false);
    RA_ActivateGame(0);
    return true;
  }
#endif

  if (!IsActive())
    return true;

  auto lock = GetLock();
  Assert(s_client && s_http_downloader);

  ClearGameInfo();
  ClearGameHash();
  DisableHardcoreMode();

  if (s_load_game_request)
  {
    rc_client_abort_async(s_client, s_load_game_request);
    s_load_game_request = nullptr;
  }
  if (s_login_request)
  {
    rc_client_abort_async(s_client, s_login_request);
    s_login_request = nullptr;
  }

  s_hardcore_mode = false;
  DestroyClient(&s_client, &s_http_downloader);

  Host::OnAchievementsRefreshed();
  return true;
}

void Achievements::EnsureCacheDirectoriesExist()
{
  s_image_directory = Path::Combine(EmuFolders::Cache, "achievement_images");

  if (!FileSystem::DirectoryExists(s_image_directory.c_str()) &&
      !FileSystem::CreateDirectory(s_image_directory.c_str(), false))
  {
    ReportFmtError("Failed to create cache directory '{}'", s_image_directory);
  }
}

void Achievements::ClientMessageCallback(const char* message, const rc_client_t* client)
{
  Log_DevPrint(message);
}

uint32_t Achievements::ClientReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
  switch (num_bytes)
  {
    case 1:
    {
      return CPU::SafeReadMemoryByte(address, buffer) ? 1 : 0;
    }

    case 2:
    {
      return CPU::SafeReadMemoryHalfWord(address, reinterpret_cast<u16*>(buffer)) ? 2 : 0;
    }

    case 4:
    {
      return CPU::SafeReadMemoryWord(address, reinterpret_cast<u32*>(buffer)) ? 4 : 0;
    }

    default:
      return 0;
  }
}

void Achievements::ClientServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback,
                                    void* callback_data, rc_client_t* client)
{
  Common::HTTPDownloader::Request::Callback hd_callback =
    [callback, callback_data](s32 status_code, std::string content_type, Common::HTTPDownloader::Request::Data data) {
      rc_api_server_response_t rr;
      rr.http_status_code = status_code;
      rr.body_length = data.size();
      rr.body = reinterpret_cast<const char*>(data.data());

      callback(&rr, callback_data);
    };

  Common::HTTPDownloader* http = static_cast<Common::HTTPDownloader*>(rc_client_get_userdata(client));

  // TODO: Content-type for post
  if (request->post_data)
  {
    // const auto pd = std::string_view(request->post_data);
    // Log_DevFmt("Server POST: {}", pd.substr(0, std::min<size_t>(pd.length(), 10)));
    http->CreatePostRequest(request->url, request->post_data, std::move(hd_callback));
  }
  else
  {
    http->CreateRequest(request->url, std::move(hd_callback));
  }
}

void Achievements::IdleUpdate()
{
  if (!IsActive())
    return;

#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
    return;
#endif

  const auto lock = GetLock();

  s_http_downloader->PollRequests();
  rc_client_idle(s_client);
}

void Achievements::FrameUpdate()
{
  if (!IsActive())
    return;

#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    RA_DoAchievementsFrame();
    return;
  }
#endif

  auto lock = GetLock();

  s_http_downloader->PollRequests();
  rc_client_do_frame(s_client);

  UpdateRichPresence(lock);
}

void Achievements::ClientEventHandler(const rc_client_event_t* event, rc_client_t* client)
{
  switch (event->type)
  {
    case RC_CLIENT_EVENT_RESET:
      HandleResetEvent(event);
      break;

    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
      HandleUnlockEvent(event);
      break;

    case RC_CLIENT_EVENT_GAME_COMPLETED:
      HandleGameCompleteEvent(event);
      break;

    case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
      HandleLeaderboardStartedEvent(event);
      break;

    case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
      HandleLeaderboardFailedEvent(event);
      break;

    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
      HandleLeaderboardSubmittedEvent(event);
      break;

    case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD:
      HandleLeaderboardScoreboardEvent(event);
      break;

    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
      HandleLeaderboardTrackerShowEvent(event);
      break;

    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
      HandleLeaderboardTrackerHideEvent(event);
      break;

    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
      HandleLeaderboardTrackerUpdateEvent(event);
      break;

    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
      HandleAchievementChallengeIndicatorShowEvent(event);
      break;

    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
      HandleAchievementChallengeIndicatorHideEvent(event);
      break;

    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
      HandleAchievementProgressIndicatorShowEvent(event);
      break;

    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
      HandleAchievementProgressIndicatorHideEvent(event);
      break;

    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
      HandleAchievementProgressIndicatorUpdateEvent(event);
      break;

    case RC_CLIENT_EVENT_SERVER_ERROR:
      HandleServerErrorEvent(event);
      break;

    case RC_CLIENT_EVENT_DISCONNECTED:
      HandleServerDisconnectedEvent(event);
      break;

    case RC_CLIENT_EVENT_RECONNECTED:
      HandleServerReconnectedEvent(event);
      break;

    default:
      Log_ErrorPrintf("Unhandled event: %u", event->type);
      break;
  }
}

void Achievements::UpdateGameSummary()
{
  rc_client_get_user_game_summary(s_client, &s_game_summary);
}

void Achievements::UpdateRichPresence(std::unique_lock<std::recursive_mutex>& lock)
{
  // Limit rich presence updates to once per second, since it could change per frame.
  if (!s_has_rich_presence || !s_rich_presence_poll_time.ResetIfSecondsPassed(1.0))
    return;

  char buffer[512];
  const size_t res = rc_client_get_rich_presence_message(s_client, buffer, std::size(buffer));
  const std::string_view sv(buffer, res);
  if (s_rich_presence_string == sv)
    return;

  s_rich_presence_string.assign(sv);

  Log_InfoPrintf("Rich presence updated: %s", s_rich_presence_string.c_str());
  Host::OnAchievementsRefreshed();

#ifdef ENABLE_DISCORD_PRESENCE
  lock.unlock();
  System::UpdateDiscordPresence();
  lock.lock();
#endif
}

void Achievements::GameChanged(const std::string& path, CDImage* image)
{
  std::unique_lock lock(s_achievements_mutex);

  if (!IsActive())
    return;

  IdentifyGame(path, image);
}

void Achievements::IdentifyGame(const std::string& path, CDImage* image)
{
  if (s_game_path == path)
  {
    Log_WarningPrint("Game path is unchanged.");
    return;
  }

  std::unique_ptr<CDImage> temp_image;
  if (!path.empty() && (!image || (g_settings.achievements_use_first_disc_from_playlist && image->HasSubImages() &&
                                   image->GetCurrentSubImage() != 0)))
  {
    temp_image = CDImage::Open(path.c_str(), g_settings.cdrom_load_image_patches, nullptr);
    image = temp_image.get();
    if (!temp_image)
      Log_ErrorPrintf("Failed to open temporary CD image '%s'", path.c_str());
  }

  std::string game_hash;
  if (image)
    game_hash = GetGameHash(image);

  if (s_game_hash == game_hash)
  {
    // only the path has changed - different format/save state/etc.
    Log_InfoPrintf("Detected path change from '%s' to '%s'", s_game_path.c_str(), path.c_str());
    s_game_path = path;
    return;
  }

  ClearGameHash();
  s_game_path = path;
  s_game_hash = std::move(game_hash);

#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    RAIntegration::GameChanged();
    return;
  }
#endif

  // shouldn't have a load game request when we're not logged in.
  Assert(IsLoggedInOrLoggingIn() || !s_load_game_request);

  // bail out if we're not logged in, just save the hash
  if (!IsLoggedInOrLoggingIn())
  {
    Log_InfoPrintf("Skipping load game because we're not logged in.");
    DisableHardcoreMode();
    return;
  }

  BeginLoadGame();
}

void Achievements::BeginLoadGame()
{
  // cancel previous requests
  if (s_load_game_request)
  {
    rc_client_abort_async(s_client, s_load_game_request);
    s_load_game_request = nullptr;
  }

  ClearGameInfo();

  if (s_game_hash.empty())
  {
    // when we're booting the bios, this will fail
    if (!s_game_path.empty())
    {
      Host::AddKeyedOSDMessage("retroachievements_disc_read_failed",
                               "Failed to read executable from disc. Achievements disabled.", Host::OSD_ERROR_DURATION);
    }

    DisableHardcoreMode();
    return;
  }

  s_load_game_request = rc_client_begin_load_game(s_client, s_game_hash.c_str(), ClientLoadGameCallback, nullptr);
}

void Achievements::ClientLoadGameCallback(int result, const char* error_message, rc_client_t* client, void* userdata)
{
  s_load_game_request = nullptr;

  if (result == RC_NO_GAME_LOADED)
  {
    // Unknown game.
    Log_InfoPrintf("Unknown game '%s', disabling achievements.", s_game_hash.c_str());
    DisableHardcoreMode();
    return;
  }
  else if (result == RC_LOGIN_REQUIRED)
  {
    // We would've asked to re-authenticate, so leave HC on for now.
    // Once we've done so, we'll reload the game.
    return;
  }
  else if (result != RC_OK)
  {
    ReportFmtError("Loading game failed: {}", error_message);
    DisableHardcoreMode();
    return;
  }

  const rc_client_game_t* info = rc_client_get_game_info(s_client);
  if (!info)
  {
    ReportError("rc_client_get_game_info() returned NULL");
    DisableHardcoreMode();
    return;
  }

  // We should have matched hardcore mode state.
  Assert(s_hardcore_mode == (rc_client_get_hardcore_enabled(client) != 0));

  s_game_id = info->id;
  s_game_title = info->title;
  s_has_achievements = rc_client_has_achievements(client);
  s_has_leaderboards = rc_client_has_leaderboards(client);
  s_has_rich_presence = rc_client_has_rich_presence(client);
  s_game_icon = {};

  // ensure fullscreen UI is ready for notifications
  FullscreenUI::Initialize();

  if (const std::string_view badge_name = info->badge_name; !badge_name.empty())
  {
    s_game_icon = Path::Combine(s_image_directory, fmt::format("game_{}.png", info->id));
    if (!FileSystem::FileExists(s_game_icon.c_str()))
    {
      char buf[512];
      if (int err = rc_client_game_get_image_url(info, buf, std::size(buf)); err == RC_OK)
      {
        DownloadImage(buf, s_game_icon);
      }
      else
      {
        ReportRCError(err, "rc_client_game_get_image_url() failed: ");
      }
    }
  }

  UpdateGameSummary();
  DisplayAchievementSummary();

  Host::OnAchievementsRefreshed();
}

void Achievements::ClearGameInfo()
{
  ClearUIState();

  if (s_load_game_request)
  {
    rc_client_abort_async(s_client, s_load_game_request);
    s_load_game_request = nullptr;
  }
  rc_client_unload_game(s_client);

  s_active_leaderboard_trackers = {};
  s_active_challenge_indicators = {};
  s_active_progress_indicator.reset();
  s_game_id = 0;
  s_game_title = {};
  s_game_icon = {};
  s_has_achievements = false;
  s_has_leaderboards = false;
  s_has_rich_presence = false;
  s_rich_presence_string = {};
  s_game_summary = {};

  Host::OnAchievementsRefreshed();
}

void Achievements::ClearGameHash()
{
  s_game_path = {};
  std::string().swap(s_game_hash);
}

void Achievements::DisplayAchievementSummary()
{
  if (g_settings.achievements_notifications && FullscreenUI::Initialize())
  {
    std::string title;
    if (IsHardcoreModeActive())
      title = fmt::format(TRANSLATE_FS("Achievements", "{} (Hardcore Mode)"), s_game_title);
    else
      title = s_game_title;

    std::string summary;
    if (s_game_summary.num_core_achievements > 0)
    {
      summary = fmt::format(TRANSLATE_FS("Achievements", "You have unlocked {} of {} achievements, and earned {} of {} points."),
                            s_game_summary.num_unlocked_achievements, s_game_summary.num_core_achievements,
                            s_game_summary.points_unlocked, s_game_summary.points_core);
    }
    else
    {
      summary = TRANSLATE_STR("Achievements", "This game has no achievements.");
    }

    ImGuiFullscreen::AddNotification("achievement_summary", ACHIEVEMENT_SUMMARY_NOTIFICATION_TIME, std::move(title),
                                     std::move(summary), s_game_icon);
  }

  // Technically not going through the resource API, but since we're passing this to something else, we can't.
  if (g_settings.achievements_sound_effects)
    PlatformMisc::PlaySoundAsync(Path::Combine(EmuFolders::Resources, INFO_SOUND_NAME).c_str());
}

void Achievements::DisplayHardcoreDeferredMessage()
{
  if (g_settings.achievements_hardcore_mode && !s_hardcore_mode && System::IsValid() && FullscreenUI::Initialize())
  {
    ImGuiFullscreen::ShowToast(std::string(),
                               TRANSLATE_STR("Achievements", "Hardcore mode will be enabled on system reset."),
                               Host::OSD_WARNING_DURATION);
  }
}

void Achievements::HandleResetEvent(const rc_client_event_t* event)
{
  // We handle system resets ourselves, but still need to reset the client's state.
  Log_InfoPrintf("Resetting runtime due to reset event");
  rc_client_reset(s_client);

  if (HasActiveGame())
    UpdateGameSummary();
}

void Achievements::HandleUnlockEvent(const rc_client_event_t* event)
{
  const rc_client_achievement_t* cheevo = event->achievement;
  DebugAssert(cheevo);

  Log_InfoPrintf("Achievement %s (%u) for game %u unlocked", cheevo->title, cheevo->id, s_game_id);
  UpdateGameSummary();

  if (g_settings.achievements_notifications && FullscreenUI::Initialize())
  {
    std::string title;
    if (cheevo->category == RC_CLIENT_ACHIEVEMENT_CATEGORY_UNOFFICIAL)
      title = fmt::format(TRANSLATE_FS("Achievements", "{} (Unofficial)"), cheevo->title);
    else
      title = cheevo->title;

    std::string badge_path = GetAchievementBadgePath(cheevo, cheevo->state);

    ImGuiFullscreen::AddNotification(fmt::format("achievement_unlock_{}", cheevo->id),
                                     static_cast<float>(g_settings.achievements_notification_duration),
                                     std::move(title), cheevo->description, std::move(badge_path));
  }

  if (g_settings.achievements_sound_effects)
    PlatformMisc::PlaySoundAsync(Path::Combine(EmuFolders::Resources, UNLOCK_SOUND_NAME).c_str());
}

void Achievements::HandleGameCompleteEvent(const rc_client_event_t* event)
{
  Log_InfoPrintf("Game %u complete", s_game_id);
  UpdateGameSummary();

  if (g_settings.achievements_notifications && FullscreenUI::Initialize())
  {
    std::string title = fmt::format(TRANSLATE_FS("Achievements", "Mastered {}"), s_game_title);
    std::string message = fmt::format(TRANSLATE_FS("Achievements", "{} achievements, {} points"),
                                      s_game_summary.num_unlocked_achievements, s_game_summary.points_unlocked);

    ImGuiFullscreen::AddNotification("achievement_mastery", GAME_COMPLETE_NOTIFICATION_TIME, std::move(title),
                                     std::move(message), s_game_icon);
  }
}

void Achievements::HandleLeaderboardStartedEvent(const rc_client_event_t* event)
{
  Log_DevPrintf("Leaderboard %u (%s) started", event->leaderboard->id, event->leaderboard->title);

  if (g_settings.achievements_leaderboard_notifications && FullscreenUI::Initialize())
  {
    std::string title = event->leaderboard->title;
    std::string message = TRANSLATE_STR("Achievements", "Leaderboard attempt started.");

    ImGuiFullscreen::AddNotification(fmt::format("leaderboard_{}", event->leaderboard->id),
                                     LEADERBOARD_STARTED_NOTIFICATION_TIME, std::move(title), std::move(message),
                                     s_game_icon);
  }
}

void Achievements::HandleLeaderboardFailedEvent(const rc_client_event_t* event)
{
  Log_DevPrintf("Leaderboard %u (%s) failed", event->leaderboard->id, event->leaderboard->title);

  if (g_settings.achievements_leaderboard_notifications && FullscreenUI::Initialize())
  {
    std::string title = event->leaderboard->title;
    std::string message = TRANSLATE_STR("Achievements", "Leaderboard attempt failed.");

    ImGuiFullscreen::AddNotification(fmt::format("leaderboard_{}", event->leaderboard->id),
                                     LEADERBOARD_FAILED_NOTIFICATION_TIME, std::move(title), std::move(message),
                                     s_game_icon);
  }
}

void Achievements::HandleLeaderboardSubmittedEvent(const rc_client_event_t* event)
{
  Log_DevPrintf("Leaderboard %u (%s) submitted", event->leaderboard->id, event->leaderboard->title);

  if (g_settings.achievements_leaderboard_notifications && FullscreenUI::Initialize())
  {
    static const char* value_strings[NUM_RC_CLIENT_LEADERBOARD_FORMATS] = {
      TRANSLATE_NOOP("Achievements", "Your Time: {}{}"),
      TRANSLATE_NOOP("Achievements", "Your Score: {}{}"),
      TRANSLATE_NOOP("Achievements", "Your Value: {}{}"),
    };

    std::string title = event->leaderboard->title;
    std::string message = fmt::format(
      fmt::runtime(Host::TranslateToStringView(
        "Achievements",
        value_strings[std::min<u8>(event->leaderboard->format, NUM_RC_CLIENT_LEADERBOARD_FORMATS - 1)])),
      event->leaderboard->tracker_value ? event->leaderboard->tracker_value : "Unknown",
      g_settings.achievements_spectator_mode ? std::string_view() : TRANSLATE_SV("Achievements", " (Submitting)"));

    ImGuiFullscreen::AddNotification(fmt::format("leaderboard_{}", event->leaderboard->id),
                                     static_cast<float>(g_settings.achievements_leaderboard_duration), std::move(title),
                                     std::move(message), s_game_icon);
  }

  if (g_settings.achievements_sound_effects)
    PlatformMisc::PlaySoundAsync(Path::Combine(EmuFolders::Resources, LBSUBMIT_SOUND_NAME).c_str());
}

void Achievements::HandleLeaderboardScoreboardEvent(const rc_client_event_t* event)
{
  Log_DevPrintf("Leaderboard %u scoreboard rank %u of %u", event->leaderboard_scoreboard->leaderboard_id,
                event->leaderboard_scoreboard->new_rank, event->leaderboard_scoreboard->num_entries);

  if (g_settings.achievements_leaderboard_notifications && FullscreenUI::Initialize())
  {
    static const char* value_strings[NUM_RC_CLIENT_LEADERBOARD_FORMATS] = {
      TRANSLATE_NOOP("Achievements", "Your Time: {} (Best: {})"),
      TRANSLATE_NOOP("Achievements", "Your Score: {} (Best: {})"),
      TRANSLATE_NOOP("Achievements", "Your Value: {} (Best: {})"),
    };

    std::string title = event->leaderboard->title;
    std::string message = fmt::format(
      TRANSLATE_FS("Achievements", "{}\nLeaderboard Position: {} of {}"),
      fmt::format(fmt::runtime(Host::TranslateToStringView(
                    "Achievements",
                    value_strings[std::min<u8>(event->leaderboard->format, NUM_RC_CLIENT_LEADERBOARD_FORMATS - 1)])),
                  event->leaderboard_scoreboard->submitted_score, event->leaderboard_scoreboard->best_score),
      event->leaderboard_scoreboard->new_rank, event->leaderboard_scoreboard->num_entries);

    ImGuiFullscreen::AddNotification(fmt::format("leaderboard_{}", event->leaderboard->id),
                                     static_cast<float>(g_settings.achievements_leaderboard_duration), std::move(title),
                                     std::move(message), s_game_icon);
  }
}

void Achievements::HandleLeaderboardTrackerShowEvent(const rc_client_event_t* event)
{
  Log_DevPrintf("Showing leaderboard tracker: %u: %s", event->leaderboard_tracker->id,
                event->leaderboard_tracker->display);

  TinyString width_string;
  width_string.append(ICON_FA_STOPWATCH);
  const u32 display_len = static_cast<u32>(std::strlen(event->leaderboard_tracker->display));
  for (u32 i = 0; i < display_len; i++)
    width_string.append('0');

  LeaderboardTrackerIndicator indicator;
  indicator.tracker_id = event->leaderboard_tracker->id;
  indicator.size = ImGuiFullscreen::g_medium_font->CalcTextSizeA(ImGuiFullscreen::g_medium_font->FontSize, FLT_MAX,
                                                                 0.0f, width_string.c_str(), width_string.end_ptr());
  indicator.text = fmt::format(ICON_FA_STOPWATCH " {}", event->leaderboard_tracker->display);
  indicator.active = true;
  s_active_leaderboard_trackers.push_back(std::move(indicator));
}

void Achievements::HandleLeaderboardTrackerHideEvent(const rc_client_event_t* event)
{
  const u32 id = event->leaderboard_tracker->id;
  auto it = std::find_if(s_active_leaderboard_trackers.begin(), s_active_leaderboard_trackers.end(),
                         [id](const auto& it) { return it.tracker_id == id; });
  if (it == s_active_leaderboard_trackers.end())
    return;

  Log_DevPrintf("Hiding leaderboard tracker: %u", id);
  it->active = false;
  it->show_hide_time.Reset();
}

void Achievements::HandleLeaderboardTrackerUpdateEvent(const rc_client_event_t* event)
{
  const u32 id = event->leaderboard_tracker->id;
  auto it = std::find_if(s_active_leaderboard_trackers.begin(), s_active_leaderboard_trackers.end(),
                         [id](const auto& it) { return it.tracker_id == id; });
  if (it == s_active_leaderboard_trackers.end())
    return;

  Log_DevPrintf("Updating leaderboard tracker: %u: %s", event->leaderboard_tracker->id,
                event->leaderboard_tracker->display);

  it->text.clear();
  fmt::format_to(std::back_inserter(it->text), ICON_FA_STOPWATCH " {}", event->leaderboard_tracker->display);
  it->active = true;
}

void Achievements::HandleAchievementChallengeIndicatorShowEvent(const rc_client_event_t* event)
{
  if (auto it =
        std::find_if(s_active_challenge_indicators.begin(), s_active_challenge_indicators.end(),
                     [event](const AchievementChallengeIndicator& it) { return it.achievement == event->achievement; });
      it != s_active_challenge_indicators.end())
  {
    it->show_hide_time.Reset();
    it->active = true;
    return;
  }

  AchievementChallengeIndicator indicator;
  indicator.achievement = event->achievement;
  indicator.badge_path = GetAchievementBadgePath(event->achievement, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);
  indicator.active = true;
  s_active_challenge_indicators.push_back(std::move(indicator));

  Log_DevPrintf("Show challenge indicator for %u (%s)", event->achievement->id, event->achievement->title);
}

void Achievements::HandleAchievementChallengeIndicatorHideEvent(const rc_client_event_t* event)
{
  auto it =
    std::find_if(s_active_challenge_indicators.begin(), s_active_challenge_indicators.end(),
                 [event](const AchievementChallengeIndicator& it) { return it.achievement == event->achievement; });
  if (it == s_active_challenge_indicators.end())
    return;

  Log_DevPrintf("Hide challenge indicator for %u (%s)", event->achievement->id, event->achievement->title);
  it->show_hide_time.Reset();
  it->active = false;
}

void Achievements::HandleAchievementProgressIndicatorShowEvent(const rc_client_event_t* event)
{
  Log_DevPrintf("Showing progress indicator: %u (%s): %s", event->achievement->id, event->achievement->title,
                event->achievement->measured_progress);

  if (!s_active_progress_indicator.has_value())
    s_active_progress_indicator.emplace();
  else
    s_active_progress_indicator->show_hide_time.Reset();

  s_active_progress_indicator->achievement = event->achievement;
  s_active_progress_indicator->badge_path =
    GetAchievementBadgePath(event->achievement, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);
  s_active_progress_indicator->active = true;
}

void Achievements::HandleAchievementProgressIndicatorHideEvent(const rc_client_event_t* event)
{
  if (!s_active_progress_indicator.has_value())
    return;

  Log_DevPrintf("Hiding progress indicator");
  s_active_progress_indicator->show_hide_time.Reset();
  s_active_progress_indicator->active = false;
}

void Achievements::HandleAchievementProgressIndicatorUpdateEvent(const rc_client_event_t* event)
{
  Log_DevPrintf("Updating progress indicator: %u (%s): %s", event->achievement->id, event->achievement->title,
                event->achievement->measured_progress);
  s_active_progress_indicator->achievement = event->achievement;
  s_active_progress_indicator->active = true;
}

void Achievements::HandleServerErrorEvent(const rc_client_event_t* event)
{
  std::string message =
    fmt::format(TRANSLATE_FS("Achievements", "Server error in {}:\n{}"),
                event->server_error->api ? event->server_error->api : "UNKNOWN",
                event->server_error->error_message ? event->server_error->error_message : "UNKNOWN");
  Log_ErrorPrint(message.c_str());
  Host::AddOSDMessage(std::move(message), Host::OSD_ERROR_DURATION);
}

void Achievements::HandleServerDisconnectedEvent(const rc_client_event_t* event)
{
  Log_WarningPrintf("Server disconnected.");

  if (FullscreenUI::Initialize())
  {
    ImGuiFullscreen::ShowToast(
      TRANSLATE_STR("Achievements", "Achievements Disconnected"),
      TRANSLATE_STR("Achievements",
                    "An unlock request could not be completed. We will keep retrying to submit this request."),
      Host::OSD_ERROR_DURATION);
  }
}

void Achievements::HandleServerReconnectedEvent(const rc_client_event_t* event)
{
  Log_WarningPrintf("Server reconnected.");

  if (FullscreenUI::Initialize())
  {
    ImGuiFullscreen::ShowToast(TRANSLATE_STR("Achievements", "Achievements Reconnected"),
                               TRANSLATE_STR("Achievements", "All pending unlock requests have completed."),
                               Host::OSD_INFO_DURATION);
  }
}

void Achievements::ResetClient()
{
#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    RA_OnReset();
    return;
  }
#endif

  if (!IsActive())
    return;

  Log_DevPrint("Reset client");
  rc_client_reset(s_client);
}

void Achievements::OnSystemPaused(bool paused)
{
#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
    RA_SetPaused(paused);
#endif
}

void Achievements::DisableHardcoreMode()
{
  if (!IsActive())
    return;

#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
  {
    if (RA_HardcoreModeIsActive())
      RA_DisableHardcore();

    return;
  }
#endif

  if (!s_hardcore_mode)
    return;

  SetHardcoreMode(false, true);
}

bool Achievements::ResetHardcoreMode()
{
  if (!IsActive())
    return false;

  const auto lock = GetLock();

  // If we're not logged in, don't apply hardcore mode restrictions.
  // If we later log in, we'll start with it off anyway.
  const bool wanted_hardcore_mode =
    (IsLoggedInOrLoggingIn() || s_load_game_request) && g_settings.achievements_hardcore_mode;
  if (s_hardcore_mode == wanted_hardcore_mode)
    return false;

  SetHardcoreMode(wanted_hardcore_mode, false);
  return true;
}

void Achievements::SetHardcoreMode(bool enabled, bool force_display_message)
{
  if (enabled == s_hardcore_mode)
    return;

  // new mode
  s_hardcore_mode = enabled;

  if (System::IsValid() && (HasActiveGame() || force_display_message) && FullscreenUI::Initialize())
  {
    ImGuiFullscreen::ShowToast(std::string(),
                               enabled ? TRANSLATE_STR("Achievements", "Hardcore mode is now enabled.") :
                                         TRANSLATE_STR("Achievements", "Hardcore mode is now disabled."),
                               Host::OSD_INFO_DURATION);
  }

  rc_client_set_hardcore_enabled(s_client, enabled);
  DebugAssert((rc_client_get_hardcore_enabled(s_client) != 0) == enabled);
  if (HasActiveGame())
  {
    UpdateGameSummary();
    DisplayAchievementSummary();
  }

  // Toss away UI state, because it's invalid now
  ClearUIState();

  Host::OnAchievementsHardcoreModeChanged(enabled);
}

bool Achievements::DoState(StateWrapper& sw)
{
  // if we're inactive, we still need to skip the data (if any)
  if (!IsActive())
  {
    u32 data_size = 0;
    sw.Do(&data_size);
    if (data_size > 0)
      sw.SkipBytes(data_size);

    return !sw.HasError();
  }

  std::unique_lock lock(s_achievements_mutex);

  if (sw.IsReading())
  {
    // if we're active, make sure we've downloaded and activated all the achievements
    // before deserializing, otherwise that state's going to get lost.
    if (!IsUsingRAIntegration() && s_load_game_request)
    {
      Host::DisplayLoadingScreen("Downloading achievements data...");
      s_http_downloader->WaitForAllRequests();
    }

    u32 data_size = 0;
    sw.Do(&data_size);
    if (data_size == 0)
    {
      // reset runtime, no data (state might've been created without cheevos)
      Log_DevPrintf("State is missing cheevos data, resetting runtime");
#ifdef ENABLE_RAINTEGRATION
      if (IsUsingRAIntegration())
        RA_OnReset();
      else
        rc_client_reset(s_client);
#else
      rc_client_reset(s_client);
#endif

      return !sw.HasError();
    }

    const std::unique_ptr<u8[]> data(new u8[data_size]);
    sw.DoBytes(data.get(), data_size);
    if (sw.HasError())
      return false;

#ifdef ENABLE_RAINTEGRATION
    if (IsUsingRAIntegration())
    {
      RA_RestoreState(reinterpret_cast<const char*>(data.get()));
    }
    else
    {
      const int result = rc_client_deserialize_progress(s_client, data.get());
      if (result != RC_OK)
      {
        Log_WarningPrintf("Failed to deserialize cheevos state (%d), resetting", result);
        rc_client_reset(s_client);
      }
    }
#endif

    return true;
  }
  else
  {
    u32 data_size;
    std::unique_ptr<u8[]> data;

#ifdef ENABLE_RAINTEGRATION
    if (IsUsingRAIntegration())
    {
      const int size = RA_CaptureState(nullptr, 0);

      data_size = (size >= 0) ? static_cast<u32>(size) : 0;
      data = std::unique_ptr<u8[]>(new u8[data_size]);

      const int result = RA_CaptureState(reinterpret_cast<char*>(data.get()), static_cast<int>(data_size));
      if (result != static_cast<int>(data_size))
      {
        Log_WarningPrint("Failed to serialize cheevos state from RAIntegration.");
        data_size = 0;
      }
    }
    else
#endif
    {
      // internally this happens twice.. not great.
      const u32 size = static_cast<u32>(rc_client_progress_size(s_client));

      data_size = (size >= 0) ? static_cast<u32>(size) : 0;
      data = std::unique_ptr<u8[]>(new u8[data_size]);

      const int result = rc_client_serialize_progress(s_client, data.get());
      if (result != RC_OK)
      {
        // set data to zero, effectively serializing nothing
        Log_WarningPrintf("Failed to serialize cheevos state (%d)", result);
        data_size = 0;
      }
    }

    sw.Do(&data_size);
    if (data_size > 0)
      sw.DoBytes(data.get(), data_size);

    return !sw.HasError();
  }
}

std::string Achievements::GetAchievementBadgePath(const rc_client_achievement_t* achievement, int state)
{
  static constexpr std::array<const char*, NUM_RC_CLIENT_ACHIEVEMENT_STATES> s_achievement_state_strings = {
    {"inactive", "active", "unlocked", "disabled"}};

  std::string path;

  if (achievement->badge_name[0] == 0)
    return path;

  path = Path::Combine(s_image_directory, TinyString::from_fmt("achievement_{}_{}_{}.png", s_game_id, achievement->id,
                                                               s_achievement_state_strings[state]));

  if (!FileSystem::FileExists(path.c_str()))
  {
    char buf[512];
    const int res = rc_client_achievement_get_image_url(achievement, state, buf, std::size(buf));
    if (res == RC_OK)
      DownloadImage(buf, path);
    else
      ReportRCError(res, "rc_client_achievement_get_image_url() for {} failed", achievement->title);
  }

  return path;
}

std::string Achievements::GetUserBadgePath(const std::string_view& username)
{
  // definitely want to sanitize usernames... :)
  std::string path;
  const std::string clean_username = Path::SanitizeFileName(username);
  if (!clean_username.empty())
    path = Path::Combine(s_image_directory, TinyString::from_fmt("user_{}.png", clean_username));
  return path;
}

std::string Achievements::GetLeaderboardUserBadgePath(const rc_client_leaderboard_entry_t* entry)
{
  // TODO: maybe we should just cache these in memory...
  std::string path = GetUserBadgePath(entry->user);

  if (!FileSystem::FileExists(path.c_str()))
  {
    char buf[512];
    const int res = rc_client_leaderboard_entry_get_user_image_url(entry, buf, std::size(buf));
    if (res == RC_OK)
      DownloadImage(buf, path);
    else
      ReportRCError(res, "rc_client_leaderboard_entry_get_user_image_url() for {} failed", entry->user);
  }

  return path;
}

bool Achievements::IsLoggedInOrLoggingIn()
{
  return (rc_client_get_user_info(s_client) != nullptr || s_login_request);
}

bool Achievements::Login(const char* username, const char* password, Error* error)
{
  auto lock = GetLock();

  // We need to use a temporary client if achievements aren't currently active.
  rc_client_t* client = s_client;
  Common::HTTPDownloader* http = s_http_downloader.get();
  const bool is_temporary_client = (client == nullptr);
  std::unique_ptr<Common::HTTPDownloader> temporary_downloader;
  ScopedGuard temporary_client_guard = [&client, is_temporary_client, &temporary_downloader]() {
    if (is_temporary_client)
      DestroyClient(&client, &temporary_downloader);
  };
  if (is_temporary_client)
  {
    if (!CreateClient(&client, &temporary_downloader))
    {
      Error::SetString(error, "Failed to create client.");
      return false;
    }
    http = temporary_downloader.get();
  }

  LoginWithPasswordParameters params = {username, error, nullptr, false};

  params.request =
    rc_client_begin_login_with_password(client, username, password, ClientLoginWithPasswordCallback, &params);
  if (!params.request)
  {
    Error::SetString(error, "Failed to create login request.");
    return false;
  }

  // Wait until the login request completes.
  http->WaitForAllRequests();
  Assert(!params.request);

  // Success? Assume the callback set the error message.
  if (!params.result)
    return false;

  // If we were't a temporary client, get the game loaded.
  if (System::IsValid() && !is_temporary_client)
    BeginLoadGame();

  return true;
}

void Achievements::ClientLoginWithPasswordCallback(int result, const char* error_message, rc_client_t* client,
                                                   void* userdata)
{
  Assert(userdata);

  LoginWithPasswordParameters* params = static_cast<LoginWithPasswordParameters*>(userdata);
  params->request = nullptr;

  if (result != RC_OK)
  {
    Log_ErrorPrintf("Login failed: %s: %s", rc_error_str(result), error_message ? error_message : "Unknown");
    Error::SetString(params->error,
                     fmt::format("{}: {}", rc_error_str(result), error_message ? error_message : "Unknown"));
    params->result = false;
    return;
  }

  // Grab the token from the client, and save it to the config.
  const rc_client_user_t* user = rc_client_get_user_info(client);
  if (!user || !user->token)
  {
    Log_ErrorPrint("rc_client_get_user_info() returned NULL");
    Error::SetString(params->error, "rc_client_get_user_info() returned NULL");
    params->result = false;
    return;
  }

  params->result = true;

  // Store configuration.
  Host::SetBaseStringSettingValue("Cheevos", "Username", params->username);
  Host::SetBaseStringSettingValue("Cheevos", "Token", user->token);
  Host::SetBaseStringSettingValue("Cheevos", "LoginTimestamp", fmt::format("{}", std::time(nullptr)).c_str());
  Host::CommitBaseSettingChanges();

  ShowLoginSuccess(client);
}

void Achievements::ClientLoginWithTokenCallback(int result, const char* error_message, rc_client_t* client,
                                                void* userdata)
{
  s_login_request = nullptr;

  if (result != RC_OK)
  {
    ReportFmtError("Login failed: {}", error_message);
    Host::OnAchievementsLoginRequested(LoginRequestReason::TokenInvalid);
    return;
  }

  ShowLoginSuccess(client);

  if (System::IsValid())
    BeginLoadGame();
}

void Achievements::ShowLoginSuccess(const rc_client_t* client)
{
  const rc_client_user_t* user = rc_client_get_user_info(client);
  if (!user)
    return;

  Host::OnAchievementsLoginSuccess(user->username, user->score, user->score_softcore, user->num_unread_messages);

  if (System::IsValid())
  {
    const auto lock = GetLock();
    if (s_client == client)
      Host::RunOnCPUThread(ShowLoginNotification);
  }
}

void Achievements::ShowLoginNotification()
{
  const rc_client_user_t* user = rc_client_get_user_info(s_client);
  if (!user)
    return;

  if (g_settings.achievements_notifications && FullscreenUI::Initialize())
  {
    std::string badge_path = GetUserBadgePath(user->username);
    if (!FileSystem::FileExists(badge_path.c_str()))
    {
      char url[512];
      const int res = rc_client_user_get_image_url(user, url, std::size(url));
      if (res == RC_OK)
        DownloadImage(url, badge_path);
      else
        ReportRCError(res, "rc_client_user_get_image_url() failed: ");
    }

    //: Summary for login notification.
    std::string title = user->display_name;
    std::string summary = fmt::format(TRANSLATE_FS("Achievements", "Score: {} ({} softcore)\nUnread messages: {}"),
                                      user->score, user->score_softcore, user->num_unread_messages);

    ImGuiFullscreen::AddNotification("achievements_login", LOGIN_NOTIFICATION_TIME, std::move(title),
                                     std::move(summary), std::move(badge_path));
  }
}

void Achievements::Logout()
{
  if (IsActive())
  {
    const auto lock = GetLock();

    if (HasActiveGame())
      ClearGameInfo();

    Log_InfoPrint("Logging out...");
    rc_client_logout(s_client);
  }

  Log_InfoPrint("Clearing credentials...");
  Host::DeleteBaseSettingValue("Cheevos", "Username");
  Host::DeleteBaseSettingValue("Cheevos", "Token");
  Host::DeleteBaseSettingValue("Cheevos", "LoginTimestamp");
  Host::CommitBaseSettingChanges();
}

bool Achievements::ConfirmSystemReset()
{
#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
    return RA_ConfirmLoadNewRom(false);
#endif

  return true;
}

bool Achievements::ConfirmHardcoreModeDisable(const char* trigger)
{
#ifdef ENABLE_RAINTEGRATION
  if (IsUsingRAIntegration())
    return (RA_WarnDisableHardcore(trigger) != 0);
#endif

  // I really hope this doesn't deadlock :/
  const bool confirmed = Host::ConfirmMessage(
    TRANSLATE("Achievements", "Confirm Hardcore Mode"),
    fmt::format(TRANSLATE_FS("Achievements", "{0} cannot be performed while hardcore mode is active. Do you "
                                             "want to disable hardcore mode? {0} will be cancelled if you select No."),
                trigger));
  if (!confirmed)
    return false;

  DisableHardcoreMode();
  return true;
}

void Achievements::ClearUIState()
{
  if (FullscreenUI::IsAchievementsWindowOpen() || FullscreenUI::IsLeaderboardsWindowOpen())
    FullscreenUI::ReturnToPreviousWindow();

  s_achievement_badge_paths = {};

  CloseLeaderboard();
  s_leaderboard_user_icon_paths = {};
  s_leaderboard_entry_lists = {};
  if (s_leaderboard_list)
  {
    rc_client_destroy_leaderboard_list(s_leaderboard_list);
    s_leaderboard_list = nullptr;
  }

  if (s_achievement_list)
  {
    rc_client_destroy_achievement_list(s_achievement_list);
    s_achievement_list = nullptr;
  }
}

template<typename T>
static float IndicatorOpacity(const T& i)
{
  const float elapsed = static_cast<float>(i.show_hide_time.GetTimeSeconds());
  const float time = i.active ? Achievements::INDICATOR_FADE_IN_TIME : Achievements::INDICATOR_FADE_OUT_TIME;
  const float opacity = (elapsed >= time) ? 1.0f : (elapsed / time);
  return (i.active) ? opacity : (1.0f - opacity);
}

void Achievements::DrawGameOverlays()
{
  using ImGuiFullscreen::g_medium_font;
  using ImGuiFullscreen::LayoutScale;

  if (!HasActiveGame() || !g_settings.achievements_overlays)
    return;

  const auto lock = GetLock();

  const float spacing = LayoutScale(10.0f);
  const float padding = LayoutScale(10.0f);
  const ImVec2 image_size =
    LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT);
  const ImGuiIO& io = ImGui::GetIO();
  ImVec2 position = ImVec2(io.DisplaySize.x - padding, io.DisplaySize.y - padding);
  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  if (!s_active_challenge_indicators.empty())
  {
    const float x_advance = image_size.x + spacing;
    ImVec2 current_position = ImVec2(position.x - image_size.x, position.y - image_size.y);

    for (auto it = s_active_challenge_indicators.begin(); it != s_active_challenge_indicators.end();)
    {
      const AchievementChallengeIndicator& indicator = *it;
      const float opacity = IndicatorOpacity(indicator);
      const u32 col = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, opacity));

      GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(indicator.badge_path);
      if (badge)
      {
        dl->AddImage(badge, current_position, current_position + image_size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                     col);
        current_position.x -= x_advance;
      }

      if (!indicator.active && opacity <= 0.01f)
      {
        Log_DevPrintf("Remove challenge indicator");
        it = s_active_challenge_indicators.erase(it);
      }
      else
      {
        ++it;
      }
    }

    position.y -= image_size.y + padding;
  }

  if (s_active_progress_indicator.has_value())
  {
    const AchievementProgressIndicator& indicator = s_active_progress_indicator.value();
    const float opacity = IndicatorOpacity(indicator);
    const u32 col = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, opacity));

    const char* text_start = s_active_progress_indicator->achievement->measured_progress;
    const char* text_end = text_start + std::strlen(text_start);
    const ImVec2 text_size = g_medium_font->CalcTextSizeA(g_medium_font->FontSize, FLT_MAX, 0.0f, text_start, text_end);

    const ImVec2 box_min = ImVec2(position.x - image_size.x - text_size.x - spacing - padding * 2.0f,
                                  position.y - image_size.y - padding * 2.0f);
    const ImVec2 box_max = position;
    const float box_rounding = LayoutScale(1.0f);

    dl->AddRectFilled(box_min, box_max, ImGui::GetColorU32(ImVec4(0.13f, 0.13f, 0.13f, opacity * 0.5f)), box_rounding);
    dl->AddRect(box_min, box_max, ImGui::GetColorU32(ImVec4(0.8f, 0.8f, 0.8f, opacity)), box_rounding);

    GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(indicator.badge_path);
    if (badge)
    {
      const ImVec2 badge_pos = box_min + ImVec2(padding, padding);
      dl->AddImage(badge, badge_pos, badge_pos + image_size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), col);
    }

    const ImVec2 text_pos =
      box_min + ImVec2(padding + image_size.x + spacing, (box_max.y - box_min.y - text_size.y) * 0.5f);
    const ImVec4 text_clip_rect(text_pos.x, text_pos.y, box_max.x, box_max.y);
    dl->AddText(g_medium_font, g_medium_font->FontSize, text_pos, col, text_start, text_end, 0.0f, &text_clip_rect);

    if (!indicator.active && opacity <= 0.01f)
    {
      Log_DevPrintf("Remove progress indicator");
      s_active_progress_indicator.reset();
    }
  }

  if (!s_active_leaderboard_trackers.empty())
  {
    for (auto it = s_active_leaderboard_trackers.begin(); it != s_active_leaderboard_trackers.end();)
    {
      const LeaderboardTrackerIndicator& indicator = *it;
      const float opacity = IndicatorOpacity(indicator);

      const ImVec2 box_min =
        ImVec2(position.x - indicator.size.x - padding * 2.0f, position.y - indicator.size.y - padding * 2.0f);
      const ImVec2 box_max = position;
      const float box_rounding = LayoutScale(1.0f);
      dl->AddRectFilled(box_min, box_max, ImGui::GetColorU32(ImVec4(0.13f, 0.13f, 0.13f, opacity * 0.5f)),
                        box_rounding);
      dl->AddRect(box_min, box_max, ImGui::GetColorU32(ImVec4(0.8f, 0.8f, 0.8f, opacity)), box_rounding);

      const u32 text_col = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, opacity));
      const ImVec2 text_pos = box_min + ImVec2(padding, padding);
      const ImVec4 text_clip_rect(text_pos.x, text_pos.y, box_max.x, box_max.y);
      dl->AddText(g_medium_font, g_medium_font->FontSize, text_pos, text_col, indicator.text.c_str(),
                  indicator.text.c_str() + indicator.text.length(), 0.0f, &text_clip_rect);

      if (!indicator.active && opacity <= 0.01f)
      {
        Log_DevPrintf("Remove tracker indicator");
        it = s_active_leaderboard_trackers.erase(it);
      }
      else
      {
        ++it;
      }
    }

    position.y -= image_size.y + padding;
  }
}

void Achievements::DrawPauseMenuOverlays()
{
  using ImGuiFullscreen::g_large_font;
  using ImGuiFullscreen::g_medium_font;
  using ImGuiFullscreen::LayoutScale;

  if (!HasActiveGame())
    return;

  const auto lock = GetLock();

  if (s_active_challenge_indicators.empty() && !s_active_progress_indicator.has_value())
    return;

  const ImGuiIO& io = ImGui::GetIO();
  ImFont* font = g_medium_font;

  const ImVec2 image_size(LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY,
                                      ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY));
  const float start_y = LayoutScale(10.0f + 4.0f + 4.0f) + g_large_font->FontSize + (g_medium_font->FontSize * 2.0f);
  const float margin = LayoutScale(10.0f);
  const float spacing = LayoutScale(10.0f);
  const float padding = LayoutScale(10.0f);

  const float max_text_width = ImGuiFullscreen::LayoutScale(300.0f);
  const float row_width = max_text_width + padding + padding + image_size.x + spacing;
  const float title_height = padding + font->FontSize + padding;

  if (!s_active_challenge_indicators.empty())
  {
    const ImVec2 box_min(io.DisplaySize.x - row_width - margin, start_y + margin);
    const ImVec2 box_max(box_min.x + row_width,
                         box_min.y + title_height +
                           (static_cast<float>(s_active_challenge_indicators.size()) * (image_size.y + padding)));

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(box_min, box_max, IM_COL32(0x21, 0x21, 0x21, 200), LayoutScale(10.0f));
    dl->AddText(font, font->FontSize, ImVec2(box_min.x + padding, box_min.y + padding), IM_COL32(255, 255, 255, 255),
                TRANSLATE("Achievements", "Active Challenge Achievements"));

    const float y_advance = image_size.y + spacing;
    const float acheivement_name_offset = (image_size.y - font->FontSize) / 2.0f;
    const float max_non_ellipised_text_width = max_text_width - LayoutScale(10.0f);
    ImVec2 position(box_min.x + padding, box_min.y + title_height);

    for (const AchievementChallengeIndicator& indicator : s_active_challenge_indicators)
    {
      GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(indicator.badge_path);
      if (!badge)
        continue;

      dl->AddImage(badge, position, position + image_size);

      const char* achievement_title = indicator.achievement->title;
      const char* achievement_title_end = achievement_title + std::strlen(indicator.achievement->title);
      const char* remaining_text = nullptr;
      const ImVec2 text_width(font->CalcTextSizeA(font->FontSize, max_non_ellipised_text_width, 0.0f, achievement_title,
                                                  achievement_title_end, &remaining_text));
      const ImVec2 text_position(position.x + image_size.x + spacing, position.y + acheivement_name_offset);
      const ImVec4 text_bbox(text_position.x, text_position.y, text_position.x + max_text_width,
                             text_position.y + image_size.y);
      const u32 text_color = IM_COL32(255, 255, 255, 255);

      if (remaining_text < achievement_title_end)
      {
        dl->AddText(font, font->FontSize, text_position, text_color, achievement_title, remaining_text, 0.0f,
                    &text_bbox);
        dl->AddText(font, font->FontSize, ImVec2(text_position.x + text_width.x, text_position.y), text_color, "...",
                    nullptr, 0.0f, &text_bbox);
      }
      else
      {
        dl->AddText(font, font->FontSize, text_position, text_color, achievement_title, achievement_title_end, 0.0f,
                    &text_bbox);
      }

      position.y += y_advance;
    }
  }
}

bool Achievements::PrepareAchievementsWindow()
{
  auto lock = Achievements::GetLock();

  s_achievement_badge_paths = {};

  if (s_achievement_list)
    rc_client_destroy_achievement_list(s_achievement_list);
  s_achievement_list = rc_client_create_achievement_list(
    s_client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
    RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS /*RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE*/);
  if (!s_achievement_list)
  {
    Log_ErrorPrint("rc_client_create_achievement_list() returned null");
    return false;
  }

  return true;
}

void Achievements::DrawAchievementsWindow()
{
  using ImGuiFullscreen::g_large_font;
  using ImGuiFullscreen::g_medium_font;
  using ImGuiFullscreen::LayoutScale;

  if (!s_achievement_list)
    return;

  auto lock = Achievements::GetLock();

  static constexpr float alpha = 0.8f;
  static constexpr float heading_alpha = 0.95f;
  static constexpr float heading_height_unscaled = 110.0f;

  const ImVec4 background = ImGuiFullscreen::ModAlpha(ImGuiFullscreen::UIBackgroundColor, alpha);
  const ImVec4 heading_background = ImGuiFullscreen::ModAlpha(ImGuiFullscreen::UIBackgroundColor, heading_alpha);
  const ImVec2 display_size = ImGui::GetIO().DisplaySize;
  const float heading_height = ImGuiFullscreen::LayoutScale(heading_height_unscaled);

  if (ImGuiFullscreen::BeginFullscreenWindow(
        ImVec2(0.0f, 0.0f), ImVec2(display_size.x, heading_height), "achievements_heading", heading_background, 0.0f,
        0.0f, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse))
  {
    ImRect bb;
    bool visible, hovered;
    ImGuiFullscreen::MenuButtonFrame("achievements_heading", false, heading_height_unscaled, &visible, &hovered,
                                     &bb.Min, &bb.Max, 0, heading_alpha);
    if (visible)
    {
      const float padding = ImGuiFullscreen::LayoutScale(10.0f);
      const float spacing = ImGuiFullscreen::LayoutScale(10.0f);
      const float image_height = ImGuiFullscreen::LayoutScale(85.0f);

      const ImVec2 icon_min(bb.Min + ImVec2(padding, padding));
      const ImVec2 icon_max(icon_min + ImVec2(image_height, image_height));

      if (!s_game_icon.empty())
      {
        GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(s_game_icon.c_str());
        if (badge)
        {
          ImGui::GetWindowDrawList()->AddImage(badge, icon_min, icon_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                               IM_COL32(255, 255, 255, 255));
        }
      }

      float left = bb.Min.x + padding + image_height + spacing;
      float right = bb.Max.x - padding;
      float top = bb.Min.y + padding;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      SmallString text;
      ImVec2 text_size;

      if (ImGuiFullscreen::FloatingButton(ICON_FA_WINDOW_CLOSE, 10.0f, 10.0f, -1.0f, -1.0f, 1.0f, 0.0f, true,
                                          g_large_font) ||
          ImGuiFullscreen::WantsToCloseMenu())
      {
        FullscreenUI::ReturnToPreviousWindow();
      }

      const ImRect title_bb(ImVec2(left, top), ImVec2(right, top + g_large_font->FontSize));
      text.assign(s_game_title);

      if (s_hardcore_mode)
        text.append(TRANSLATE_SV("Achievements", " (Hardcore Mode)"));

      top += g_large_font->FontSize + spacing;

      ImGui::PushFont(g_large_font);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, text.c_str(), text.end_ptr(), nullptr, ImVec2(0.0f, 0.0f),
                               &title_bb);
      ImGui::PopFont();

      const ImRect summary_bb(ImVec2(left, top), ImVec2(right, top + g_medium_font->FontSize));
      if (s_game_summary.num_unlocked_achievements == s_game_summary.num_core_achievements)
      {
        text.fmt(TRANSLATE_FS("Achievements", "You have unlocked all achievements and earned {} points!"),
                 s_game_summary.points_unlocked);
      }
      else
      {
        text.fmt(
          TRANSLATE_FS("Achievements", "You have unlocked {} of {} achievements, earning {} of {} possible points."),
          s_game_summary.num_unlocked_achievements, s_game_summary.num_core_achievements,
          s_game_summary.points_unlocked, s_game_summary.points_core);
      }

      top += g_medium_font->FontSize + spacing;

      ImGui::PushFont(g_medium_font);
      ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, text.c_str(), text.end_ptr(), nullptr,
                               ImVec2(0.0f, 0.0f), &summary_bb);
      ImGui::PopFont();

      const float progress_height = ImGuiFullscreen::LayoutScale(20.0f);
      const ImRect progress_bb(ImVec2(left, top), ImVec2(right, top + progress_height));
      const float fraction = static_cast<float>(s_game_summary.num_unlocked_achievements) /
                             static_cast<float>(s_game_summary.num_core_achievements);
      dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryDarkColor));
      dl->AddRectFilled(progress_bb.Min,
                        ImVec2(progress_bb.Min.x + fraction * progress_bb.GetWidth(), progress_bb.Max.y),
                        ImGui::GetColorU32(ImGuiFullscreen::UISecondaryColor));

      text.fmt("{}%", static_cast<int>(std::round(fraction * 100.0f)));
      text_size = ImGui::CalcTextSize(text.c_str(), text.end_ptr());
      const ImVec2 text_pos(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                            progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) -
                              (text_size.y / 2.0f));
      dl->AddText(g_medium_font, g_medium_font->FontSize, text_pos,
                  ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryTextColor), text.c_str(), text.end_ptr());
      top += progress_height + spacing;
    }
  }
  ImGuiFullscreen::EndFullscreenWindow();

  ImGui::SetNextWindowBgAlpha(alpha);

  if (ImGuiFullscreen::BeginFullscreenWindow(ImVec2(0.0f, heading_height),
                                             ImVec2(display_size.x, display_size.y - heading_height), "achievements",
                                             background, 0.0f, 0.0f, 0))
  {
    static bool buckets_collapsed[NUM_RC_CLIENT_ACHIEVEMENT_BUCKETS] = {};
    static const char* bucket_names[NUM_RC_CLIENT_ACHIEVEMENT_BUCKETS] = {
      TRANSLATE_NOOP("Achievements", "Unknown"),           TRANSLATE_NOOP("Achievements", "Locked"),
      TRANSLATE_NOOP("Achievements", "Unlocked"),          TRANSLATE_NOOP("Achievements", "Unsupported"),
      TRANSLATE_NOOP("Achievements", "Unofficial"),        TRANSLATE_NOOP("Achievements", "Recently Unlocked"),
      TRANSLATE_NOOP("Achievements", "Active Challenges"), TRANSLATE_NOOP("Achievements", "Almost There"),
    };

    ImGuiFullscreen::BeginMenuButtons();

    for (u32 bucket_type : {RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE,
                            RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED, RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED,
                            RC_CLIENT_ACHIEVEMENT_BUCKET_ALMOST_THERE, RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED,
                            RC_CLIENT_ACHIEVEMENT_BUCKET_UNOFFICIAL, RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED})
    {
      for (u32 bucket_idx = 0; bucket_idx < s_achievement_list->num_buckets; bucket_idx++)
      {
        const rc_client_achievement_bucket_t& bucket = s_achievement_list->buckets[bucket_idx];
        if (bucket.bucket_type != bucket_type)
          continue;

        DebugAssert(bucket.bucket_type < NUM_RC_CLIENT_ACHIEVEMENT_BUCKETS);

        // TODO: Once subsets are supported, this will need to change.
        bool& bucket_collapsed = buckets_collapsed[bucket.bucket_type];
        bucket_collapsed ^=
          ImGuiFullscreen::MenuHeadingButton(Host::TranslateToCString("Achievements", bucket_names[bucket.bucket_type]),
                                             bucket_collapsed ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_UP);
        if (!bucket_collapsed)
        {
          for (u32 i = 0; i < bucket.num_achievements; i++)
            DrawAchievement(bucket.achievements[i]);
        }
      }
    }

    ImGuiFullscreen::EndMenuButtons();
  }
  ImGuiFullscreen::EndFullscreenWindow();
}

void Achievements::DrawAchievement(const rc_client_achievement_t* cheevo)
{
  using ImGuiFullscreen::g_large_font;
  using ImGuiFullscreen::g_medium_font;
  using ImGuiFullscreen::LayoutScale;

  static constexpr float alpha = 0.8f;
  static constexpr float progress_height_unscaled = 20.0f;
  static constexpr float progress_spacing_unscaled = 5.0f;

  const float spacing = ImGuiFullscreen::LayoutScale(4.0f);

  const bool is_unlocked = (cheevo->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);
  const std::string_view measured_progress(cheevo->measured_progress);
  const bool is_measured = !is_unlocked && !measured_progress.empty();
  const float unlock_size = is_unlocked ? (spacing + ImGuiFullscreen::LAYOUT_MEDIUM_FONT_SIZE) : 0.0f;

  ImRect bb;
  bool visible, hovered;
  ImGuiFullscreen::MenuButtonFrame(TinyString::from_fmt("chv_{}", cheevo->id), true,
                                   !is_measured ? ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT + unlock_size :
                                                  ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT +
                                                    progress_height_unscaled + progress_spacing_unscaled,
                                   &visible, &hovered, &bb.Min, &bb.Max, 0, alpha);
  if (!visible)
    return;

  std::string* badge_path;
  if (const auto badge_it = std::find_if(s_achievement_badge_paths.begin(), s_achievement_badge_paths.end(),
                                         [cheevo](const auto& it) { return (it.first == cheevo); });
      badge_it != s_achievement_badge_paths.end())
  {
    badge_path = &badge_it->second;
  }
  else
  {
    std::string new_badge_path = Achievements::GetAchievementBadgePath(cheevo, cheevo->state);
    badge_path = &s_achievement_badge_paths.emplace_back(cheevo, std::move(new_badge_path)).second;
  }

  const ImVec2 image_size(
    LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT));
  if (!badge_path->empty())
  {
    GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(*badge_path);
    if (badge)
    {
      ImGui::GetWindowDrawList()->AddImage(badge, bb.Min, bb.Min + image_size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                           IM_COL32(255, 255, 255, 255));
    }
  }

  SmallString text;

  const float midpoint = bb.Min.y + g_large_font->FontSize + spacing;
  text.fmt((cheevo->points != 1) ? TRANSLATE_FS("Achievements", "{} points") : TRANSLATE_FS("Achievements", "{} point"),
           cheevo->points);
  const ImVec2 points_template_size(
    g_medium_font->CalcTextSizeA(g_medium_font->FontSize, FLT_MAX, 0.0f, TRANSLATE("Achievements", "XXX points")));
  const ImVec2 points_size(
    g_medium_font->CalcTextSizeA(g_medium_font->FontSize, FLT_MAX, 0.0f, text.c_str(), text.end_ptr()));
  const float points_template_start = bb.Max.x - points_template_size.x;
  const float points_start = points_template_start + ((points_template_size.x - points_size.x) * 0.5f);
  const char* lock_text = is_unlocked ? ICON_FA_LOCK_OPEN : ICON_FA_LOCK;
  const ImVec2 lock_size(g_large_font->CalcTextSizeA(g_large_font->FontSize, FLT_MAX, 0.0f, lock_text));

  const float text_start_x = bb.Min.x + image_size.x + LayoutScale(15.0f);
  const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(points_start, midpoint));
  const ImRect summary_bb(ImVec2(text_start_x, midpoint), ImVec2(points_start, midpoint + g_medium_font->FontSize));
  const ImRect points_bb(ImVec2(points_start, midpoint), bb.Max);
  const ImRect lock_bb(ImVec2(points_template_start + ((points_template_size.x - lock_size.x) * 0.5f), bb.Min.y),
                       ImVec2(bb.Max.x, midpoint));

  ImGui::PushFont(g_large_font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, cheevo->title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::RenderTextClipped(lock_bb.Min, lock_bb.Max, lock_text, nullptr, &lock_size, ImVec2(0.0f, 0.0f), &lock_bb);
  ImGui::PopFont();

  ImGui::PushFont(g_medium_font);
  if (cheevo->description && std::strlen(cheevo->description) > 0)
  {
    ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, cheevo->description, nullptr, nullptr, ImVec2(0.0f, 0.0f),
                             &summary_bb);
  }
  ImGui::RenderTextClipped(points_bb.Min, points_bb.Max, text.c_str(), text.end_ptr(), &points_size, ImVec2(0.0f, 0.0f),
                           &points_bb);

  if (is_unlocked)
  {
    TinyString date;
    FullscreenUI::TimeToPrintableString(&date, cheevo->unlock_time);
    text.fmt(TRANSLATE_FS("Achievements", "Unlocked: {}"), date);

    const ImRect unlock_bb(summary_bb.Min.x, summary_bb.Max.y + spacing, summary_bb.Max.x, bb.Max.y);
    ImGui::RenderTextClipped(unlock_bb.Min, unlock_bb.Max, text.c_str(), text.end_ptr(), nullptr, ImVec2(0.0f, 0.0f),
                             &unlock_bb);
  }
  else if (is_measured)
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float progress_height = LayoutScale(progress_height_unscaled);
    const float progress_spacing = LayoutScale(progress_spacing_unscaled);
    const float top = midpoint + g_medium_font->FontSize + progress_spacing;
    const ImRect progress_bb(ImVec2(text_start_x, top), ImVec2(bb.Max.x, top + progress_height));
    const float fraction = cheevo->measured_percent * 0.01f;
    dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryDarkColor));
    dl->AddRectFilled(progress_bb.Min, ImVec2(progress_bb.Min.x + fraction * progress_bb.GetWidth(), progress_bb.Max.y),
                      ImGui::GetColorU32(ImGuiFullscreen::UISecondaryColor));

    const ImVec2 text_size =
      ImGui::CalcTextSize(measured_progress.data(), measured_progress.data() + measured_progress.size());
    const ImVec2 text_pos(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                          progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) - (text_size.y / 2.0f));
    dl->AddText(g_medium_font, g_medium_font->FontSize, text_pos,
                ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryTextColor), measured_progress.data(),
                measured_progress.data() + measured_progress.size());
  }

  ImGui::PopFont();
}

bool Achievements::PrepareLeaderboardsWindow()
{
  auto lock = Achievements::GetLock();
  rc_client_t* const client = s_client;

  s_achievement_badge_paths = {};
  CloseLeaderboard();
  if (s_leaderboard_list)
    rc_client_destroy_leaderboard_list(s_leaderboard_list);
  s_leaderboard_list = rc_client_create_leaderboard_list(client, RC_CLIENT_LEADERBOARD_LIST_GROUPING_NONE);
  if (!s_leaderboard_list)
  {
    Log_ErrorPrint("rc_client_create_leaderboard_list() returned null");
    return false;
  }

  return true;
}

void Achievements::DrawLeaderboardsWindow()
{
  using ImGuiFullscreen::g_large_font;
  using ImGuiFullscreen::g_medium_font;
  using ImGuiFullscreen::LayoutScale;

  static constexpr float alpha = 0.8f;
  static constexpr float heading_alpha = 0.95f;
  static constexpr float heading_height_unscaled = 110.0f;
  static constexpr float tab_height_unscaled = 50.0f;

  auto lock = Achievements::GetLock();

  const bool is_leaderboard_open = (s_open_leaderboard != nullptr);
  bool close_leaderboard_on_exit = false;

  ImRect bb;

  const ImVec4 background = ImGuiFullscreen::ModAlpha(ImGuiFullscreen::UIBackgroundColor, alpha);
  const ImVec4 heading_background = ImGuiFullscreen::ModAlpha(ImGuiFullscreen::UIBackgroundColor, heading_alpha);
  const ImVec2 display_size = ImGui::GetIO().DisplaySize;
  const float padding = LayoutScale(10.0f);
  const float spacing = LayoutScale(10.0f);
  const float spacing_small = spacing / 2.0f;
  float heading_height = LayoutScale(heading_height_unscaled);
  if (is_leaderboard_open)
  {
    // tabs
    heading_height += spacing_small + LayoutScale(tab_height_unscaled) + spacing;

    // Add space for a legend - spacing + 1 line of text + spacing + line
    heading_height += LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) + spacing;
  }

  const float rank_column_width =
    g_large_font->CalcTextSizeA(g_large_font->FontSize, std::numeric_limits<float>::max(), -1.0f, "99999").x;
  const float name_column_width =
    g_large_font
      ->CalcTextSizeA(g_large_font->FontSize, std::numeric_limits<float>::max(), -1.0f, "WWWWWWWWWWWWWWWWWWWWWW")
      .x;
  const float time_column_width =
    g_large_font->CalcTextSizeA(g_large_font->FontSize, std::numeric_limits<float>::max(), -1.0f, "WWWWWWWWWWW").x;
  const float column_spacing = spacing * 2.0f;

  if (ImGuiFullscreen::BeginFullscreenWindow(
        ImVec2(0.0f, 0.0f), ImVec2(display_size.x, heading_height), "leaderboards_heading", heading_background, 0.0f,
        0.0f, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse))
  {
    bool visible, hovered;
    bool pressed = ImGuiFullscreen::MenuButtonFrame("leaderboards_heading", false, heading_height_unscaled, &visible,
                                                    &hovered, &bb.Min, &bb.Max, 0, alpha);
    UNREFERENCED_VARIABLE(pressed);

    if (visible)
    {
      const float image_height = LayoutScale(85.0f);

      const ImVec2 icon_min(bb.Min + ImVec2(padding, padding));
      const ImVec2 icon_max(icon_min + ImVec2(image_height, image_height));

      if (!s_game_icon.empty())
      {
        GPUTexture* badge = ImGuiFullscreen::GetCachedTextureAsync(s_game_icon.c_str());
        if (badge)
        {
          ImGui::GetWindowDrawList()->AddImage(badge, icon_min, icon_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                               IM_COL32(255, 255, 255, 255));
        }
      }

      float left = bb.Min.x + padding + image_height + spacing;
      float right = bb.Max.x - padding;
      float top = bb.Min.y + padding;
      SmallString text;

      if (!is_leaderboard_open)
      {
        if (ImGuiFullscreen::FloatingButton(ICON_FA_WINDOW_CLOSE, 10.0f, 10.0f, -1.0f, -1.0f, 1.0f, 0.0f, true,
                                            g_large_font) ||
            ImGuiFullscreen::WantsToCloseMenu())
        {
          FullscreenUI::ReturnToPreviousWindow();
        }
      }
      else
      {
        if (ImGuiFullscreen::FloatingButton(ICON_FA_CARET_SQUARE_LEFT, 10.0f, 10.0f, -1.0f, -1.0f, 1.0f, 0.0f, true,
                                            g_large_font) ||
            ImGuiFullscreen::WantsToCloseMenu())
        {
          close_leaderboard_on_exit = true;
        }
      }

      const ImRect title_bb(ImVec2(left, top), ImVec2(right, top + g_large_font->FontSize));
      text.assign(Achievements::GetGameTitle());

      top += g_large_font->FontSize + spacing;

      ImGui::PushFont(g_large_font);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, text.c_str(), text.end_ptr(), nullptr, ImVec2(0.0f, 0.0f),
                               &title_bb);
      ImGui::PopFont();

      if (is_leaderboard_open)
      {
        const ImRect subtitle_bb(ImVec2(left, top), ImVec2(right, top + g_large_font->FontSize));
        text.assign(s_open_leaderboard->title);

        top += g_large_font->FontSize + spacing_small;

        ImGui::PushFont(g_large_font);
        ImGui::RenderTextClipped(subtitle_bb.Min, subtitle_bb.Max, text.c_str(), text.end_ptr(), nullptr,
                                 ImVec2(0.0f, 0.0f), &subtitle_bb);
        ImGui::PopFont();

        text.assign(s_open_leaderboard->description);
      }
      else
      {
        u32 count = 0;
        for (u32 i = 0; i < s_leaderboard_list->num_buckets; i++)
          count += s_leaderboard_list->buckets[i].num_leaderboards;
        text.fmt(TRANSLATE_FS("Achievements", "This game has {} leaderboards."), count);
      }

      const ImRect summary_bb(ImVec2(left, top), ImVec2(right, top + g_medium_font->FontSize));
      top += g_medium_font->FontSize + spacing_small;

      ImGui::PushFont(g_medium_font);
      ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, text.c_str(), text.end_ptr(), nullptr,
                               ImVec2(0.0f, 0.0f), &summary_bb);

      if (!is_leaderboard_open && !Achievements::IsHardcoreModeActive())
      {
        const ImRect hardcore_warning_bb(ImVec2(left, top), ImVec2(right, top + g_medium_font->FontSize));
        top += g_medium_font->FontSize + spacing_small;

        ImGui::RenderTextClipped(
          hardcore_warning_bb.Min, hardcore_warning_bb.Max,
          TRANSLATE("Achievements",
                    "Submitting scores is disabled because hardcore mode is off. Leaderboards are read-only."),
          nullptr, nullptr, ImVec2(0.0f, 0.0f), &hardcore_warning_bb);
      }

      ImGui::PopFont();

      if (is_leaderboard_open)
      {
        const float tab_width = (ImGui::GetWindowWidth() / ImGuiFullscreen::g_layout_scale) * 0.5f;
        ImGui::SetCursorPos(ImVec2(0.0f, top + spacing_small));

        if (ImGui::IsNavInputTest(ImGuiNavInput_FocusPrev, ImGuiNavReadMode_Pressed) ||
            ImGui::IsNavInputTest(ImGuiNavInput_FocusNext, ImGuiNavReadMode_Pressed))
        {
          s_is_showing_all_leaderboard_entries = !s_is_showing_all_leaderboard_entries;
        }

        for (const bool show_all : {false, true})
        {
          const char* title =
            show_all ? TRANSLATE("Achievements", "Show Best") : TRANSLATE("Achievements", "Show Nearby");
          if (ImGuiFullscreen::NavTab(title, s_is_showing_all_leaderboard_entries == show_all, true, tab_width,
                                      tab_height_unscaled, heading_background))
          {
            s_is_showing_all_leaderboard_entries = show_all;
          }
        }

        const ImVec2 bg_pos =
          ImVec2(0.0f, ImGui::GetCurrentWindow()->DC.CursorPos.y + LayoutScale(tab_height_unscaled));
        const ImVec2 bg_size =
          ImVec2(ImGui::GetWindowWidth(),
                 spacing + LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) + spacing);
        ImGui::GetWindowDrawList()->AddRectFilled(bg_pos, bg_pos + bg_size, ImGui::GetColorU32(heading_background));

        ImGui::SetCursorPos(ImVec2(0.0f, ImGui::GetCursorPosY() + LayoutScale(tab_height_unscaled) + spacing));

        pressed =
          ImGuiFullscreen::MenuButtonFrame("legend", false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY,
                                           &visible, &hovered, &bb.Min, &bb.Max, 0, alpha);
        UNREFERENCED_VARIABLE(pressed);

        const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
        float text_start_x = bb.Min.x + LayoutScale(15.0f) + padding;

        ImGui::PushFont(g_large_font);

        const ImRect rank_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        ImGui::RenderTextClipped(rank_bb.Min, rank_bb.Max, TRANSLATE("Achievements", "Rank"), nullptr, nullptr,
                                 ImVec2(0.0f, 0.0f), &rank_bb);
        text_start_x += rank_column_width + column_spacing;

        const ImRect user_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        ImGui::RenderTextClipped(user_bb.Min, user_bb.Max, TRANSLATE("Achievements", "Name"), nullptr, nullptr,
                                 ImVec2(0.0f, 0.0f), &user_bb);
        text_start_x += name_column_width + column_spacing;

        static const char* value_headings[NUM_RC_CLIENT_LEADERBOARD_FORMATS] = {
          TRANSLATE_NOOP("Achievements", "Time"),
          TRANSLATE_NOOP("Achievements", "Score"),
          TRANSLATE_NOOP("Achievements", "Value"),
        };

        const ImRect score_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        ImGui::RenderTextClipped(
          score_bb.Min, score_bb.Max,
          Host::TranslateToCString(
            "Achievements",
            value_headings[std::min<u8>(s_open_leaderboard->format, NUM_RC_CLIENT_LEADERBOARD_FORMATS - 1)]),
          nullptr, nullptr, ImVec2(0.0f, 0.0f), &score_bb);
        text_start_x += time_column_width + column_spacing;

        const ImRect date_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        ImGui::RenderTextClipped(date_bb.Min, date_bb.Max, TRANSLATE("Achievements", "Date Submitted"), nullptr,
                                 nullptr, ImVec2(0.0f, 0.0f), &date_bb);

        ImGui::PopFont();

        const float line_thickness = LayoutScale(1.0f);
        const float line_padding = LayoutScale(5.0f);
        const ImVec2 line_start(bb.Min.x, bb.Min.y + g_large_font->FontSize + line_padding);
        const ImVec2 line_end(bb.Max.x, line_start.y);
        ImGui::GetWindowDrawList()->AddLine(line_start, line_end, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            line_thickness);
      }
    }
  }
  ImGuiFullscreen::EndFullscreenWindow();

  if (!is_leaderboard_open)
  {
    if (ImGuiFullscreen::BeginFullscreenWindow(ImVec2(0.0f, heading_height),
                                               ImVec2(display_size.x, display_size.y - heading_height), "leaderboards",
                                               background, 0.0f, 0.0f, 0))
    {
      ImGuiFullscreen::BeginMenuButtons();

      for (u32 bucket_index = 0; bucket_index < s_leaderboard_list->num_buckets; bucket_index++)
      {
        const rc_client_leaderboard_bucket_t& bucket = s_leaderboard_list->buckets[bucket_index];
        for (u32 i = 0; i < bucket.num_leaderboards; i++)
          DrawLeaderboardListEntry(bucket.leaderboards[i]);
      }

      ImGuiFullscreen::EndMenuButtons();
    }
    ImGuiFullscreen::EndFullscreenWindow();
  }
  else
  {
    if (ImGuiFullscreen::BeginFullscreenWindow(ImVec2(0.0f, heading_height),
                                               ImVec2(display_size.x, display_size.y - heading_height), "leaderboard",
                                               background, 0.0f, 0.0f, 0))
    {
      ImGuiFullscreen::BeginMenuButtons();

      if (!s_is_showing_all_leaderboard_entries)
      {
        if (s_leaderboard_nearby_entries)
        {
          for (u32 i = 0; i < s_leaderboard_nearby_entries->num_entries; i++)
          {
            DrawLeaderboardEntry(s_leaderboard_nearby_entries->entries[i],
                                 static_cast<s32>(i) == s_leaderboard_nearby_entries->user_index, rank_column_width,
                                 name_column_width, time_column_width, column_spacing);
          }
        }
        else
        {
          ImGui::PushFont(g_large_font);

          const ImVec2 pos_min(0.0f, heading_height);
          const ImVec2 pos_max(display_size.x, display_size.y);
          ImGui::RenderTextClipped(pos_min, pos_max,
                                   TRANSLATE("Achievements", "Downloading leaderboard data, please wait..."), nullptr,
                                   nullptr, ImVec2(0.5f, 0.5f));

          ImGui::PopFont();
        }
      }
      else
      {
        for (const rc_client_leaderboard_entry_list_t* list : s_leaderboard_entry_lists)
        {
          for (u32 i = 0; i < list->num_entries; i++)
          {
            DrawLeaderboardEntry(list->entries[i], static_cast<s32>(i) == list->user_index, rank_column_width,
                                 name_column_width, time_column_width, column_spacing);
          }
        }

        // Fetch next chunk if the loading indicator becomes visible (i.e. we scrolled enough).
        bool visible, hovered;
        ImGuiFullscreen::MenuButtonFrame(TRANSLATE("Achievements", "Loading..."), false,
                                         ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, &visible, &hovered,
                                         &bb.Min, &bb.Max);
        if (visible)
        {
          const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
          const ImRect title_bb(bb.Min, ImVec2(bb.Max.x, midpoint));

          ImGui::PushFont(g_large_font);
          ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, TRANSLATE("Achievements", "Loading..."), nullptr,
                                   nullptr, ImVec2(0, 0), &title_bb);
          ImGui::PopFont();

          if (!s_leaderboard_fetch_handle)
            FetchNextLeaderboardEntries();
        }
      }

      ImGuiFullscreen::EndMenuButtons();
    }
    ImGuiFullscreen::EndFullscreenWindow();
  }

  if (close_leaderboard_on_exit)
    CloseLeaderboard();
}

void Achievements::DrawLeaderboardEntry(const rc_client_leaderboard_entry_t& entry, bool is_self,
                                        float rank_column_width, float name_column_width, float time_column_width,
                                        float column_spacing)
{
  using ImGuiFullscreen::g_large_font;
  using ImGuiFullscreen::LayoutScale;

  static constexpr float alpha = 0.8f;

  ImRect bb;
  bool visible, hovered;
  bool pressed =
    ImGuiFullscreen::MenuButtonFrame(entry.user, true, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, &visible,
                                     &hovered, &bb.Min, &bb.Max, 0, alpha);
  if (!visible)
    return;

  const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
  float text_start_x = bb.Min.x + LayoutScale(15.0f);
  SmallString text;

  text.fmt("{}", entry.rank);

  ImGui::PushFont(g_large_font);

  if (is_self)
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(255, 242, 0, 255));

  const ImRect rank_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  ImGui::RenderTextClipped(rank_bb.Min, rank_bb.Max, text.c_str(), text.end_ptr(), nullptr, ImVec2(0.0f, 0.0f),
                           &rank_bb);
  text_start_x += rank_column_width + column_spacing;

  const float icon_size = bb.Max.y - bb.Min.y;
  const ImRect icon_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  GPUTexture* icon_tex = nullptr;
  if (auto it = std::find_if(s_leaderboard_user_icon_paths.begin(), s_leaderboard_user_icon_paths.end(),
                             [&entry](const auto& it) { return it.first == &entry; });
      it != s_leaderboard_user_icon_paths.end())
  {
    if (!it->second.empty())
      icon_tex = ImGuiFullscreen::GetCachedTextureAsync(it->second);
  }
  else
  {
    std::string path = Achievements::GetLeaderboardUserBadgePath(&entry);
    if (!path.empty())
    {
      icon_tex = ImGuiFullscreen::GetCachedTextureAsync(path);
      s_leaderboard_user_icon_paths.emplace_back(&entry, std::move(path));
    }
  }
  if (icon_tex)
  {
    ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(icon_tex), icon_bb.Min,
                                         icon_bb.Min + ImVec2(icon_size, icon_size));
  }

  const ImRect user_bb(ImVec2(text_start_x + column_spacing + icon_size, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  ImGui::RenderTextClipped(user_bb.Min, user_bb.Max, entry.user, nullptr, nullptr, ImVec2(0.0f, 0.0f), &user_bb);
  text_start_x += name_column_width + column_spacing;

  const ImRect score_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  ImGui::RenderTextClipped(score_bb.Min, score_bb.Max, entry.display, nullptr, nullptr, ImVec2(0.0f, 0.0f), &score_bb);
  text_start_x += time_column_width + column_spacing;

  const ImRect time_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  SmallString submit_time;
  FullscreenUI::TimeToPrintableString(&submit_time, entry.submitted);
  ImGui::RenderTextClipped(time_bb.Min, time_bb.Max, submit_time.c_str(), submit_time.end_ptr(), nullptr,
                           ImVec2(0.0f, 0.0f), &time_bb);

  if (is_self)
    ImGui::PopStyleColor();

  ImGui::PopFont();

  if (pressed)
  {
    // Anything?
  }
}
void Achievements::DrawLeaderboardListEntry(const rc_client_leaderboard_t* lboard)
{
  using ImGuiFullscreen::g_large_font;
  using ImGuiFullscreen::g_medium_font;
  using ImGuiFullscreen::LayoutScale;

  static constexpr float alpha = 0.8f;

  TinyString id_str;
  id_str.fmt("{}", lboard->id);

  ImRect bb;
  bool visible, hovered;
  bool pressed = ImGuiFullscreen::MenuButtonFrame(id_str, true, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, &visible,
                                                  &hovered, &bb.Min, &bb.Max, 0, alpha);
  if (!visible)
    return;

  const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
  const float text_start_x = bb.Min.x + LayoutScale(15.0f);
  const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  const ImRect summary_bb(ImVec2(text_start_x, midpoint), bb.Max);

  ImGui::PushFont(g_large_font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, lboard->title, nullptr, nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::PopFont();

  if (lboard->description && lboard->description[0] != '\0')
  {
    ImGui::PushFont(g_medium_font);
    ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, lboard->description, nullptr, nullptr, ImVec2(0.0f, 0.0f),
                             &summary_bb);
    ImGui::PopFont();
  }

  if (pressed)
    OpenLeaderboard(lboard);
}

void Achievements::OpenLeaderboard(const rc_client_leaderboard_t* lboard)
{
  Log_DevPrintf("Opening leaderboard '%s' (%u)", lboard->title, lboard->id);

  CloseLeaderboard();

  s_open_leaderboard = lboard;
  s_is_showing_all_leaderboard_entries = false;
  s_leaderboard_fetch_handle = rc_client_begin_fetch_leaderboard_entries_around_user(
    s_client, lboard->id, LEADERBOARD_NEARBY_ENTRIES_TO_FETCH, LeaderboardFetchNearbyCallback, nullptr);
}

void Achievements::LeaderboardFetchNearbyCallback(int result, const char* error_message,
                                                  rc_client_leaderboard_entry_list_t* list, rc_client_t* client,
                                                  void* callback_userdata)
{
  const auto lock = GetLock();

  s_leaderboard_fetch_handle = nullptr;

  if (result != RC_OK)
  {
    ImGuiFullscreen::ShowToast(TRANSLATE("Achievements", "Leaderboard download failed"), error_message);
    CloseLeaderboard();
    return;
  }

  if (s_leaderboard_nearby_entries)
    rc_client_destroy_leaderboard_entry_list(s_leaderboard_nearby_entries);
  s_leaderboard_nearby_entries = list;
}

void Achievements::LeaderboardFetchAllCallback(int result, const char* error_message,
                                               rc_client_leaderboard_entry_list_t* list, rc_client_t* client,
                                               void* callback_userdata)
{
  const auto lock = GetLock();

  s_leaderboard_fetch_handle = nullptr;

  if (result != RC_OK)
  {
    ImGuiFullscreen::ShowToast(TRANSLATE("Achievements", "Leaderboard download failed"), error_message);
    CloseLeaderboard();
    return;
  }

  s_leaderboard_entry_lists.push_back(list);
}

void Achievements::FetchNextLeaderboardEntries()
{
  u32 start = 1;
  for (rc_client_leaderboard_entry_list_t* list : s_leaderboard_entry_lists)
    start += list->num_entries;

  Log_DevPrintf("Fetching entries %u to %u", start, start + LEADERBOARD_ALL_FETCH_SIZE);

  if (s_leaderboard_fetch_handle)
    rc_client_abort_async(s_client, s_leaderboard_fetch_handle);
  s_leaderboard_fetch_handle = rc_client_begin_fetch_leaderboard_entries(
    s_client, s_open_leaderboard->id, start, LEADERBOARD_ALL_FETCH_SIZE, LeaderboardFetchAllCallback, nullptr);
}

void Achievements::CloseLeaderboard()
{
  s_leaderboard_user_icon_paths.clear();

  for (auto iter = s_leaderboard_entry_lists.rbegin(); iter != s_leaderboard_entry_lists.rend(); ++iter)
    rc_client_destroy_leaderboard_entry_list(*iter);
  s_leaderboard_entry_lists.clear();

  if (s_leaderboard_nearby_entries)
  {
    rc_client_destroy_leaderboard_entry_list(s_leaderboard_nearby_entries);
    s_leaderboard_nearby_entries = nullptr;
  }

  if (s_leaderboard_fetch_handle)
  {
    rc_client_abort_async(s_client, s_leaderboard_fetch_handle);
    s_leaderboard_fetch_handle = nullptr;
  }

  s_open_leaderboard = nullptr;
}

#ifdef ENABLE_RAINTEGRATION

#include "RA_Consoles.h"

bool Achievements::IsUsingRAIntegration()
{
  return s_using_raintegration;
}

namespace Achievements::RAIntegration {
static void InitializeRAIntegration(void* main_window_handle);

static int RACallbackIsActive();
static void RACallbackCauseUnpause();
static void RACallbackCausePause();
static void RACallbackRebuildMenu();
static void RACallbackEstimateTitle(char* buf);
static void RACallbackResetEmulator();
static void RACallbackLoadROM(const char* unused);
static unsigned char RACallbackReadMemory(unsigned int address);
static unsigned int RACallbackReadMemoryBlock(unsigned int nAddress, unsigned char* pBuffer, unsigned int nBytes);
static void RACallbackWriteMemory(unsigned int address, unsigned char value);

static bool s_raintegration_initialized = false;
} // namespace Achievements::RAIntegration

void Achievements::SwitchToRAIntegration()
{
  s_using_raintegration = true;
}

void Achievements::RAIntegration::InitializeRAIntegration(void* main_window_handle)
{
  RA_InitClient((HWND)main_window_handle, "DuckStation", g_scm_tag_str);
  RA_SetUserAgentDetail(Achievements::GetUserAgent().c_str());

  RA_InstallSharedFunctions(RACallbackIsActive, RACallbackCauseUnpause, RACallbackCausePause, RACallbackRebuildMenu,
                            RACallbackEstimateTitle, RACallbackResetEmulator, RACallbackLoadROM);
  RA_SetConsoleID(PlayStation);

  // Apparently this has to be done early, or the memory inspector doesn't work.
  // That's a bit unfortunate, because the RAM size can vary between games, and depending on the option.
  RA_InstallMemoryBank(0, RACallbackReadMemory, RACallbackWriteMemory, Bus::RAM_2MB_SIZE);
  RA_InstallMemoryBankBlockReader(0, RACallbackReadMemoryBlock);

  // Fire off a login anyway. Saves going into the menu and doing it.
  RA_AttemptLogin(0);

  s_raintegration_initialized = true;

  // this is pretty lame, but we may as well persist until we exit anyway
  std::atexit(RA_Shutdown);
}

void Achievements::RAIntegration::MainWindowChanged(void* new_handle)
{
  if (s_raintegration_initialized)
  {
    RA_UpdateHWnd((HWND)new_handle);
    return;
  }

  InitializeRAIntegration(new_handle);
}

void Achievements::RAIntegration::GameChanged()
{
  s_game_id = s_game_hash.empty() ? 0 : RA_IdentifyHash(s_game_hash.c_str());
  RA_ActivateGame(s_game_id);
}

std::vector<std::tuple<int, std::string, bool>> Achievements::RAIntegration::GetMenuItems()
{
  std::array<RA_MenuItem, 64> items;
  const int num_items = RA_GetPopupMenuItems(items.data());

  std::vector<std::tuple<int, std::string, bool>> ret;
  ret.reserve(static_cast<u32>(num_items));

  for (int i = 0; i < num_items; i++)
  {
    const RA_MenuItem& it = items[i];
    if (!it.sLabel)
      ret.emplace_back(0, std::string(), false);
    else
      ret.emplace_back(static_cast<int>(it.nID), StringUtil::WideStringToUTF8String(it.sLabel), it.bChecked);
  }

  return ret;
}

void Achievements::RAIntegration::ActivateMenuItem(int item)
{
  RA_InvokeDialog(item);
}

int Achievements::RAIntegration::RACallbackIsActive()
{
  return static_cast<int>(HasActiveGame());
}

void Achievements::RAIntegration::RACallbackCauseUnpause()
{
  Host::RunOnCPUThread([]() { System::PauseSystem(false); });
}

void Achievements::RAIntegration::RACallbackCausePause()
{
  Host::RunOnCPUThread([]() { System::PauseSystem(true); });
}

void Achievements::RAIntegration::RACallbackRebuildMenu()
{
  // unused, we build the menu on demand
}

void Achievements::RAIntegration::RACallbackEstimateTitle(char* buf)
{
  StringUtil::Strlcpy(buf, System::GetGameTitle(), 256);
}

void Achievements::RAIntegration::RACallbackResetEmulator()
{
  if (System::IsValid())
    System::ResetSystem();
}

void Achievements::RAIntegration::RACallbackLoadROM(const char* unused)
{
  // unused
  UNREFERENCED_PARAMETER(unused);
}

unsigned char Achievements::RAIntegration::RACallbackReadMemory(unsigned int address)
{
  if (!System::IsValid())
    return 0;

  u8 value = 0;
  CPU::SafeReadMemoryByte(address, &value);
  return value;
}

void Achievements::RAIntegration::RACallbackWriteMemory(unsigned int address, unsigned char value)
{
  CPU::SafeWriteMemoryByte(address, value);
}

unsigned int Achievements::RAIntegration::RACallbackReadMemoryBlock(unsigned int nAddress, unsigned char* pBuffer,
                                                                    unsigned int nBytes)
{
  if (nAddress >= Bus::g_ram_size)
    return 0;

  const u32 copy_size = std::min<u32>(Bus::g_ram_size - nAddress, nBytes);
  std::memcpy(pBuffer, Bus::g_ram + nAddress, copy_size);
  return copy_size;
}

#else

bool Achievements::IsUsingRAIntegration()
{
  return false;
}

#endif
