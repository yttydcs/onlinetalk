#include "client/ui/ui_app.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

#include "common/log.h"

namespace onlinetalk::client {

struct UiApp::UiRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;

  bool contains(int px, int py) const {
    return px >= x && px < (x + w) && py >= y && py < (y + h);
  }

  SDL_Rect toSdl() const {
    return SDL_Rect{x, y, w, h};
  }
};

struct UiApp::UiTheme {
  SDL_Color background{20, 22, 28, 255};
  SDL_Color panel{30, 33, 41, 255};
  SDL_Color panel_alt{38, 42, 52, 255};
  SDL_Color text{230, 230, 235, 255};
  SDL_Color text_muted{160, 165, 175, 255};
  SDL_Color border{52, 58, 70, 255};
  SDL_Color input_bg{26, 29, 36, 255};
  SDL_Color accent{72, 160, 255, 255};
  SDL_Color ok{70, 190, 130, 255};
  SDL_Color warn{230, 170, 90, 255};
  SDL_Color danger{220, 90, 90, 255};
  SDL_Color button{56, 94, 120, 255};
  SDL_Color button_hover{76, 124, 156, 255};
};

struct UiApp::TextInput {
  std::string value;
  std::string placeholder;
  size_t max_len = 256;
  bool password = false;
};

struct UiApp::UiInput {
  int mouse_x = 0;
  int mouse_y = 0;
  bool mouse_down = false;
  bool mouse_clicked = false;
  int wheel_y = 0;
};

struct UiApp::GroupEntry {
  std::string group_id;
  std::string name;
};

struct UiApp::PendingGroupAction {
  enum class Type {
    Create,
    Join,
    Leave,
    Rename,
    Dissolve,
    Kick,
    SetAdmin
  };

  Type type = Type::Join;
  std::string group_id;
  std::string group_name;
  std::string target_user_id;
  bool make_admin = false;
};

class UiApp::TextCache {
 public:
  struct Entry {
    SDL_Texture* texture = nullptr;
    int w = 0;
    int h = 0;
    SDL_Color color{0, 0, 0, 255};
    int wrap_width = 0;
  };

  TextCache(SDL_Renderer* renderer, TTF_Font* font) : renderer_(renderer), font_(font) {}

  ~TextCache() {
    clear();
  }

  void setFont(TTF_Font* font) {
    if (font_ != font) {
      clear();
      font_ = font;
    }
  }

  void clear() {
    for (auto& entry : cache_) {
      if (entry.second.texture) {
        SDL_DestroyTexture(entry.second.texture);
      }
    }
    cache_.clear();
  }

  const Entry* get(const std::string& text, SDL_Color color, int wrap_width) {
    if (!font_ || !renderer_ || text.empty()) {
      return nullptr;
    }
    const auto key = makeKey(text, color, wrap_width);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
      return &it->second;
    }

    SDL_Surface* surface = nullptr;
    if (wrap_width > 0) {
      surface = TTF_RenderUTF8_Blended_Wrapped(font_, text.c_str(), color, wrap_width);
    } else {
      surface = TTF_RenderUTF8_Blended(font_, text.c_str(), color);
    }
    if (!surface) {
      return nullptr;
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
    if (!texture) {
      SDL_FreeSurface(surface);
      return nullptr;
    }
    Entry entry;
    entry.texture = texture;
    entry.w = surface->w;
    entry.h = surface->h;
    entry.color = color;
    entry.wrap_width = wrap_width;
    SDL_FreeSurface(surface);
    auto inserted = cache_.emplace(key, std::move(entry));
    return &inserted.first->second;
  }

 private:
  static std::string makeKey(const std::string& text, SDL_Color color, int wrap_width) {
    std::ostringstream oss;
    oss << wrap_width << ':'
        << static_cast<int>(color.r) << ','
        << static_cast<int>(color.g) << ','
        << static_cast<int>(color.b) << ','
        << static_cast<int>(color.a) << ':'
        << text;
    return oss.str();
  }

  SDL_Renderer* renderer_ = nullptr;
  TTF_Font* font_ = nullptr;
  std::unordered_map<std::string, Entry> cache_;
};

namespace {

constexpr int kHeaderHeight = 48;
constexpr int kLeftPanelWidth = 260;
constexpr int kRightPanelWidth = 320;
constexpr int kInputHeight = 96;
constexpr int kPadding = 10;
constexpr int kRowHeight = 24;
constexpr int kScrollStep = 24;
constexpr uint32_t kStatusDurationMs = 5000;

void popBackUtf8(std::string* text) {
  if (!text || text->empty()) {
    return;
  }
  size_t i = text->size() - 1;
  while (i > 0 && (static_cast<unsigned char>((*text)[i]) & 0xC0) == 0x80) {
    --i;
  }
  text->erase(i);
}

std::string formatBytes(int64_t size) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(size);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(value >= 10.0 ? 0 : 1) << value << ' ' << units[unit];
  return oss.str();
}

std::string formatTimestamp(int64_t epoch) {
  if (epoch <= 0) {
    return "-";
  }
  std::time_t t = static_cast<std::time_t>(epoch);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
  return buffer;
}

bool parseJson(const std::string& text, nlohmann::json* out) {
  try {
    *out = nlohmann::json::parse(text);
    return true;
  } catch (...) {
    return false;
  }
}

std::string resolvePathWithBases(const std::string& path,
                                 const std::vector<std::filesystem::path>& bases) {
  if (path.empty()) {
    return "";
  }
  std::filesystem::path candidate(path);
  if (candidate.is_absolute()) {
    if (std::filesystem::exists(candidate)) {
      return candidate.string();
    }
  } else if (std::filesystem::exists(candidate)) {
    return candidate.string();
  }
  for (const auto& base : bases) {
    if (base.empty()) {
      continue;
    }
    auto combined = base / candidate;
    if (std::filesystem::exists(combined)) {
      return combined.string();
    }
  }
  return "";
}

std::vector<std::string> emojiPalette() {
  return {
      std::string(u8"\U0001F600"),
      std::string(u8"\U0001F602"),
      std::string(u8"\U0001F60D"),
      std::string(u8"\U0001F44D"),
      std::string(u8"\U0001F680"),
      std::string(u8"\U0001F389")
  };
}

}  // namespace

UiApp::UiApp(const onlinetalk::common::ClientConfig& config,
             NetClient& net,
             ClientApi& api,
             ClientState& state,
             FileTransferManager& transfers)
    : config_(config), net_(net), api_(api), state_(state), transfers_(transfers) {
  login_user_input_.placeholder = "User ID";
  login_password_input_.placeholder = "Password";
  login_password_input_.password = true;

  register_user_input_.placeholder = "User ID";
  register_nick_input_.placeholder = "Nickname";
  register_password_input_.placeholder = "Password";
  register_password_input_.password = true;

  chat_input_.placeholder = "Type a message...";
  file_path_input_.placeholder = "File path";
  group_id_input_.placeholder = "Group ID";
  group_name_input_.placeholder = "Group Name";
  group_target_input_.placeholder = "Target User";
}

UiApp::~UiApp() {
  shutdown();
}

bool UiApp::init(std::string* error) {
  if (window_ || renderer_) {
    return true;
  }

  window_ = SDL_CreateWindow("OnlineTalk",
                             SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED,
                             config_.window_width,
                             config_.window_height,
                             SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (!window_) {
    if (error) {
      *error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
    }
    return false;
  }

  renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer_) {
    if (error) {
      *error = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
    }
    return false;
  }
  SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

  std::vector<std::filesystem::path> bases;
  if (char* base = SDL_GetBasePath()) {
    bases.emplace_back(base);
    SDL_free(base);
  }
  bases.emplace_back(std::filesystem::current_path());

  std::vector<std::string> text_candidates = {
      "assets/fonts/NotoSans-Regular.ttf",
      "assets/fonts/DejaVuSans.ttf",
  };

  std::string text_font_path;
  for (const auto& candidate : text_candidates) {
    text_font_path = resolvePathWithBases(candidate, bases);
    if (!text_font_path.empty()) {
      break;
    }
  }
  if (text_font_path.empty()) {
    if (error) {
      *error = "text font not found (assets/fonts/NotoSans-Regular.ttf)";
    }
    return false;
  }

  font_ = TTF_OpenFont(text_font_path.c_str(), 18);
  font_small_ = TTF_OpenFont(text_font_path.c_str(), 14);
  if (!font_ || !font_small_) {
    if (error) {
      *error = std::string("TTF_OpenFont failed: ") + TTF_GetError();
    }
    return false;
  }

  std::string emoji_path = resolvePathWithBases(config_.emoji_font_path, bases);
  if (emoji_path.empty()) {
    emoji_path = resolvePathWithBases("assets/fonts/NotoColorEmoji.ttf", bases);
  }
  if (!emoji_path.empty()) {
    font_emoji_ = TTF_OpenFont(emoji_path.c_str(), 20);
  }
  if (!font_emoji_) {
    font_emoji_ = font_;
  }

  text_cache_ = std::make_unique<TextCache>(renderer_, font_);
  text_cache_small_ = std::make_unique<TextCache>(renderer_, font_small_);
  text_cache_emoji_ = std::make_unique<TextCache>(renderer_, font_emoji_);
  return true;
}

void UiApp::run() {
  running_ = true;
  SDL_StartTextInput();

  while (running_) {
    UiInput input;
    SDL_GetMouseState(&input.mouse_x, &input.mouse_y);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
        case SDL_QUIT:
          running_ = false;
          break;
        case SDL_MOUSEMOTION:
          input.mouse_x = event.motion.x;
          input.mouse_y = event.motion.y;
          break;
        case SDL_MOUSEBUTTONDOWN:
          if (event.button.button == SDL_BUTTON_LEFT) {
            input.mouse_down = true;
            input.mouse_x = event.button.x;
            input.mouse_y = event.button.y;
          }
          break;
        case SDL_MOUSEBUTTONUP:
          if (event.button.button == SDL_BUTTON_LEFT) {
            input.mouse_down = false;
            input.mouse_clicked = true;
            input.mouse_x = event.button.x;
            input.mouse_y = event.button.y;
          }
          break;
        case SDL_MOUSEWHEEL:
          input.wheel_y += event.wheel.y;
          break;
        case SDL_TEXTINPUT:
          handleTextInput(event.text.text);
          break;
        case SDL_KEYDOWN:
          handleKeyDown(event.key.keysym.sym);
          break;
        default:
          break;
      }
    }

    processNetwork();
    updateConnection();
    renderFrame(input);
    SDL_Delay(16);
  }

  SDL_StopTextInput();
}

void UiApp::shutdown() {
  if (text_cache_) {
    text_cache_->clear();
    text_cache_.reset();
  }
  if (text_cache_small_) {
    text_cache_small_->clear();
    text_cache_small_.reset();
  }
  if (text_cache_emoji_) {
    text_cache_emoji_->clear();
    text_cache_emoji_.reset();
  }
  if (font_emoji_ && font_emoji_ != font_) {
    TTF_CloseFont(font_emoji_);
  }
  if (font_) {
    TTF_CloseFont(font_);
  }
  if (font_small_) {
    TTF_CloseFont(font_small_);
  }
  font_emoji_ = nullptr;
  font_ = nullptr;
  font_small_ = nullptr;

  if (renderer_) {
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
  }
  if (window_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
}

bool UiApp::handleTextInput(const char* text) {
  if (!focused_input_) {
    return false;
  }
  if (!text || *text == '\0') {
    return false;
  }
  if (focused_input_->value.size() + std::strlen(text) > focused_input_->max_len) {
    return false;
  }
  focused_input_->value.append(text);
  return true;
}

bool UiApp::handleKeyDown(SDL_Keycode key) {
  if (key == SDLK_ESCAPE) {
    focused_input_ = nullptr;
    return true;
  }
  if (!focused_input_) {
    return false;
  }
  if (key == SDLK_BACKSPACE) {
    popBackUtf8(&focused_input_->value);
    return true;
  }
  if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
    if (focused_input_ == &chat_input_) {
      onSendMessage();
    } else if (focused_input_ == &file_path_input_) {
      onSendFile();
    } else if (focused_input_ == &login_user_input_ ||
               focused_input_ == &login_password_input_) {
      onLoginRequested();
    } else if (focused_input_ == &register_user_input_ ||
               focused_input_ == &register_nick_input_ ||
               focused_input_ == &register_password_input_) {
      onRegisterRequested();
    }
    return true;
  }
  return false;
}

void UiApp::processNetwork() {
  onlinetalk::common::Packet packet;
  while (net_.pollPacket(&packet)) {
    transfers_.handlePacket(net_, packet);
    state_.applyPacket(packet);
    handlePacket(packet);
  }
}

void UiApp::handlePacket(const onlinetalk::common::Packet& packet) {
  nlohmann::json meta;
  if (!parseJson(packet.meta_json, &meta)) {
    return;
  }
  const auto type = static_cast<onlinetalk::common::PacketType>(packet.header.type);
  if (type == onlinetalk::common::PacketType::AuthOk) {
    const bool registered = meta.value("registered", false);
    const bool logged_in = meta.value("logged_in", false);
    if (registered && !logged_in) {
      setStatusMessage("Registered. Please login.", theme_.ok, kStatusDurationMs);
    } else if (logged_in) {
      setStatusMessage("Login success.", theme_.ok, kStatusDurationMs);
    }
    return;
  }
  if (type == onlinetalk::common::PacketType::AuthError) {
    const auto message = meta.value("message", "login failed");
    setStatusMessage(message, theme_.danger, kStatusDurationMs);
    return;
  }
  if (type == onlinetalk::common::PacketType::FileDone) {
    const auto name = meta.value("file_name", "");
    if (!name.empty()) {
      setStatusMessage("File available: " + name, theme_.ok, kStatusDurationMs);
    }
    return;
  }

  if (type == onlinetalk::common::PacketType::GroupCreate ||
      type == onlinetalk::common::PacketType::GroupJoin ||
      type == onlinetalk::common::PacketType::GroupLeave ||
      type == onlinetalk::common::PacketType::GroupAdmin) {
    const auto status = meta.value("status", "");
    const auto message = meta.value("message", "");
    auto req_it = group_requests_.find(packet.header.request_id);
    if (!status.empty() && status != "ok") {
      if (!message.empty()) {
        setStatusMessage(message, theme_.danger, kStatusDurationMs);
      }
      if (req_it != group_requests_.end()) {
        group_requests_.erase(req_it);
      }
      return;
    }

    if (req_it != group_requests_.end()) {
      const auto& action = req_it->second;
      if (action.type == PendingGroupAction::Type::Create) {
        const auto group_id = meta.value("group_id", action.group_id);
        const auto name = meta.value("name", action.group_name);
        if (!group_id.empty()) {
          auto it = std::find_if(groups_.begin(), groups_.end(),
                                 [&](const GroupEntry& entry) { return entry.group_id == group_id; });
          if (it == groups_.end()) {
            groups_.push_back({group_id, name.empty() ? group_id : name});
          }
        }
      } else if (action.type == PendingGroupAction::Type::Join) {
        if (!action.group_id.empty()) {
          auto it = std::find_if(groups_.begin(), groups_.end(),
                                 [&](const GroupEntry& entry) { return entry.group_id == action.group_id; });
          if (it == groups_.end()) {
            const auto name = action.group_name.empty() ? action.group_id : action.group_name;
            groups_.push_back({action.group_id, name});
          }
        }
      } else if (action.type == PendingGroupAction::Type::Leave ||
                 action.type == PendingGroupAction::Type::Dissolve) {
        groups_.erase(std::remove_if(groups_.begin(), groups_.end(),
                                     [&](const GroupEntry& entry) {
                                       return entry.group_id == action.group_id;
                                     }),
                      groups_.end());
        if (active_type_ == "group" && active_id_ == action.group_id) {
          active_type_.clear();
          active_id_.clear();
        }
      } else if (action.type == PendingGroupAction::Type::Rename) {
        for (auto& entry : groups_) {
          if (entry.group_id == action.group_id) {
            if (!action.group_name.empty()) {
              entry.name = action.group_name;
            }
            break;
          }
        }
      }
      group_requests_.erase(req_it);
    }
  }
}

void UiApp::updateConnection() {
  if (net_.isRunning()) {
    was_connected_ = true;
    return;
  }
  if (was_connected_) {
    const auto net_error = net_.lastError();
    if (!net_error.empty()) {
      setStatusMessage("Disconnected: " + net_error, theme_.warn, kStatusDurationMs);
    } else {
      setStatusMessage("Disconnected from server.", theme_.warn, kStatusDurationMs);
    }
    was_connected_ = false;
  }
  const uint32_t now = SDL_GetTicks();
  if (now - last_reconnect_ms_ < 2000) {
    return;
  }
  last_reconnect_ms_ = now;
  net_.stop();
  std::string error;
  if (net_.connectTo(config_.server_host, config_.server_port, &error)) {
    net_.start();
    was_connected_ = true;
    setStatusMessage("Reconnected.", theme_.ok, kStatusDurationMs);
    if (!saved_user_id_.empty() && !saved_password_.empty()) {
      api_.sendLogin(saved_user_id_, saved_password_, &error);
    }
    if (!active_type_.empty() && !active_id_.empty()) {
      state_.resetHistoryCursor(active_type_, active_id_);
      api_.fetchHistory(active_type_, active_id_, 0, config_.history_page_size, &error);
    }
    transfers_.resumeTransfers(net_, &error);
  } else {
    setStatusMessage("Reconnect failed: " + error, theme_.danger, kStatusDurationMs);
  }
}

void UiApp::renderFrame(const UiInput& input) {
  int width = 0;
  int height = 0;
  SDL_GetWindowSize(window_, &width, &height);
  UiRect full{0, 0, width, height};

  SDL_SetRenderDrawColor(renderer_,
                         theme_.background.r,
                         theme_.background.g,
                         theme_.background.b,
                         theme_.background.a);
  SDL_RenderClear(renderer_);

  renderTopBar(full, input);
  UiRect body{0, kHeaderHeight, width, height - kHeaderHeight};
  if (!state_.loggedIn()) {
    renderAuthScreen(body, input);
  } else {
    renderChatScreen(body, input);
  }
  SDL_RenderPresent(renderer_);
}

void UiApp::renderTopBar(const UiRect& bounds, const UiInput& input) {
  UiRect bar{bounds.x, bounds.y, bounds.w, kHeaderHeight};
  SDL_SetRenderDrawColor(renderer_,
                         theme_.panel.r,
                         theme_.panel.g,
                         theme_.panel.b,
                         theme_.panel.a);
  auto rect = bar.toSdl();
  SDL_RenderFillRect(renderer_, &rect);

  const std::string title = "OnlineTalk";
  if (auto entry = text_cache_->get(title, theme_.text, 0)) {
    SDL_Rect dst{bar.x + kPadding, bar.y + 14, entry->w, entry->h};
    SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
  }

  std::string status = net_.isRunning() ? "Connected" : "Disconnected";
  SDL_Color status_color = net_.isRunning() ? theme_.ok : theme_.danger;
  if (auto entry = text_cache_small_->get(status, status_color, 0)) {
    SDL_Rect dst{bar.x + bar.w - kPadding - entry->w, bar.y + 16, entry->w, entry->h};
    SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
  }

  if (state_.loggedIn()) {
    std::string user_label = state_.nickname().empty() ? state_.userId() : state_.nickname();
    if (!user_label.empty()) {
      if (auto entry = text_cache_small_->get(user_label, theme_.text_muted, 0)) {
        SDL_Rect dst{bar.x + bar.w - kPadding - entry->w, bar.y + 4, entry->w, entry->h};
        SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
      }
    }
  }

  const uint32_t now = SDL_GetTicks();
  if (!status_message_.empty() && now < status_until_ms_) {
    if (auto entry = text_cache_small_->get(status_message_, status_color_, 0)) {
      SDL_Rect dst{bar.x + 160, bar.y + 16, entry->w, entry->h};
      SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
    }
  }
  (void)input;
}

void UiApp::renderAuthScreen(const UiRect& bounds, const UiInput& input) {
  const int panel_w = std::min(480, bounds.w - 2 * kPadding);
  const int panel_h = 320;
  const int panel_x = bounds.x + (bounds.w - panel_w) / 2;
  const int panel_y = bounds.y + (bounds.h - panel_h) / 2;
  UiRect panel{panel_x, panel_y, panel_w, panel_h};

  SDL_SetRenderDrawColor(renderer_,
                         theme_.panel_alt.r,
                         theme_.panel_alt.g,
                         theme_.panel_alt.b,
                         theme_.panel_alt.a);
  auto rect = panel.toSdl();
  SDL_RenderFillRect(renderer_, &rect);

  UiRect tab_login{panel.x + 20, panel.y + 16, 100, 26};
  UiRect tab_register{panel.x + 130, panel.y + 16, 120, 26};

  const bool click_login = input.mouse_clicked && tab_login.contains(input.mouse_x, input.mouse_y);
  const bool click_register = input.mouse_clicked && tab_register.contains(input.mouse_x, input.mouse_y);
  if (click_login) {
    show_register_ = false;
  } else if (click_register) {
    show_register_ = true;
  }

  SDL_SetRenderDrawColor(renderer_,
                         theme_.button.r,
                         theme_.button.g,
                         theme_.button.b,
                         theme_.button.a);
  auto tab_rect = tab_login.toSdl();
  SDL_RenderFillRect(renderer_, &tab_rect);
  tab_rect = tab_register.toSdl();
  SDL_RenderFillRect(renderer_, &tab_rect);

  if (auto entry = text_cache_small_->get("Login", theme_.text, 0)) {
    SDL_Rect dst{tab_login.x + 18, tab_login.y + 6, entry->w, entry->h};
    SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
  }
  if (auto entry = text_cache_small_->get("Register", theme_.text, 0)) {
    SDL_Rect dst{tab_register.x + 12, tab_register.y + 6, entry->w, entry->h};
    SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
  }

  bool clicked_input = false;
  auto drawInput = [&](UiRect input_rect, TextInput& field) {
    SDL_SetRenderDrawColor(renderer_,
                           theme_.input_bg.r,
                           theme_.input_bg.g,
                           theme_.input_bg.b,
                           theme_.input_bg.a);
    SDL_Rect sdl_rect = input_rect.toSdl();
    SDL_RenderFillRect(renderer_, &sdl_rect);

    const bool focused = hasFocus(field);
    SDL_Color border = focused ? theme_.accent : theme_.border;
    SDL_SetRenderDrawColor(renderer_, border.r, border.g, border.b, border.a);
    SDL_RenderDrawRect(renderer_, &sdl_rect);

    std::string display = field.password ? std::string(field.value.size(), '*') : field.value;
    if (display.empty()) {
      display = field.placeholder;
      if (auto entry = text_cache_small_->get(display, theme_.text_muted, 0)) {
        SDL_Rect dst{input_rect.x + 8, input_rect.y + 6, entry->w, entry->h};
        SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
      }
    } else if (auto entry = text_cache_->get(display, theme_.text, input_rect.w - 16)) {
      SDL_Rect dst{input_rect.x + 8, input_rect.y + 6, entry->w, entry->h};
      SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
    }

    if (input.mouse_clicked && input_rect.contains(input.mouse_x, input.mouse_y)) {
      setFocus(&field);
      clicked_input = true;
    }
  };

  int field_y = panel.y + 70;
  if (!show_register_) {
    drawInput({panel.x + 20, field_y, panel.w - 40, 32}, login_user_input_);
    field_y += 44;
    drawInput({panel.x + 20, field_y, panel.w - 40, 32}, login_password_input_);

    UiRect login_button{panel.x + 20, panel.y + panel.h - 60, 120, 32};
    SDL_SetRenderDrawColor(renderer_,
                           theme_.button.r,
                           theme_.button.g,
                           theme_.button.b,
                           theme_.button.a);
    SDL_Rect btn = login_button.toSdl();
    SDL_RenderFillRect(renderer_, &btn);
    if (auto entry = text_cache_small_->get("Login", theme_.text, 0)) {
      SDL_Rect dst{login_button.x + 36, login_button.y + 8, entry->w, entry->h};
      SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
    }
    if (input.mouse_clicked && login_button.contains(input.mouse_x, input.mouse_y)) {
      onLoginRequested();
    }
  } else {
    drawInput({panel.x + 20, field_y, panel.w - 40, 32}, register_user_input_);
    field_y += 44;
    drawInput({panel.x + 20, field_y, panel.w - 40, 32}, register_nick_input_);
    field_y += 44;
    drawInput({panel.x + 20, field_y, panel.w - 40, 32}, register_password_input_);

    UiRect reg_button{panel.x + 20, panel.y + panel.h - 60, 120, 32};
    SDL_SetRenderDrawColor(renderer_,
                           theme_.button.r,
                           theme_.button.g,
                           theme_.button.b,
                           theme_.button.a);
    SDL_Rect btn = reg_button.toSdl();
    SDL_RenderFillRect(renderer_, &btn);
    if (auto entry = text_cache_small_->get("Register", theme_.text, 0)) {
      SDL_Rect dst{reg_button.x + 26, reg_button.y + 8, entry->w, entry->h};
      SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
    }
    if (input.mouse_clicked && reg_button.contains(input.mouse_x, input.mouse_y)) {
      onRegisterRequested();
    }
  }

  if (input.mouse_clicked && !clicked_input) {
    focused_input_ = nullptr;
  }
}

void UiApp::renderChatScreen(const UiRect& bounds, const UiInput& input) {
  UiRect left{bounds.x, bounds.y, kLeftPanelWidth, bounds.h};
  UiRect right{bounds.x + bounds.w - kRightPanelWidth, bounds.y, kRightPanelWidth, bounds.h};
  UiRect center{left.x + left.w, bounds.y, bounds.w - left.w - right.w, bounds.h};

  SDL_SetRenderDrawColor(renderer_,
                         theme_.panel.r,
                         theme_.panel.g,
                         theme_.panel.b,
                         theme_.panel.a);
  SDL_Rect left_rect = left.toSdl();
  SDL_RenderFillRect(renderer_, &left_rect);
  SDL_Rect right_rect = right.toSdl();
  SDL_RenderFillRect(renderer_, &right_rect);

  SDL_SetRenderDrawColor(renderer_,
                         theme_.panel_alt.r,
                         theme_.panel_alt.g,
                         theme_.panel_alt.b,
                         theme_.panel_alt.a);
  SDL_Rect center_rect = center.toSdl();
  SDL_RenderFillRect(renderer_, &center_rect);

  UiRect message_area{center.x + kPadding,
                      center.y + kPadding + 26,
                      center.w - 2 * kPadding,
                      center.h - kPadding - kInputHeight - 26};
  UiRect input_area{center.x + kPadding,
                    center.y + center.h - kInputHeight - kPadding,
                    center.w - 2 * kPadding,
                    kInputHeight};

  UiRect user_list_area{left.x + kPadding,
                        left.y + kPadding + 22,
                        left.w - 2 * kPadding,
                        (left.h / 2) - 32};
  UiRect group_list_area{left.x + kPadding,
                         left.y + left.h / 2 + kPadding + 22,
                         left.w - 2 * kPadding,
                         (left.h / 2) - 32 - kPadding};

  UiRect group_action_area{right.x + kPadding,
                           right.y + kPadding + 22,
                           right.w - 2 * kPadding,
                           220};
  UiRect file_list_area{right.x + kPadding,
                        group_action_area.y + group_action_area.h + kPadding + 22,
                        right.w - 2 * kPadding,
                        right.h - group_action_area.h - 2 * kPadding - 120 - 44};
  UiRect transfer_area{right.x + kPadding,
                       right.y + right.h - 120 - kPadding,
                       right.w - 2 * kPadding,
                       120};

  if (input.wheel_y != 0) {
    if (message_area.contains(input.mouse_x, input.mouse_y)) {
      message_scroll_y_ -= input.wheel_y * kScrollStep;
      stick_to_bottom_ = false;
    } else if (user_list_area.contains(input.mouse_x, input.mouse_y)) {
      user_scroll_y_ -= input.wheel_y * kScrollStep;
    } else if (group_list_area.contains(input.mouse_x, input.mouse_y)) {
      group_scroll_y_ -= input.wheel_y * kScrollStep;
    } else if (file_list_area.contains(input.mouse_x, input.mouse_y)) {
      file_scroll_y_ -= input.wheel_y * kScrollStep;
    }
  }

  if (auto entry = text_cache_small_->get("Online Users", theme_.text_muted, 0)) {
    SDL_Rect dst{user_list_area.x, user_list_area.y - 18, entry->w, entry->h};
    SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
  }
  if (auto entry = text_cache_small_->get("Groups", theme_.text_muted, 0)) {
    SDL_Rect dst{group_list_area.x, group_list_area.y - 18, entry->w, entry->h};
    SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
  }
  if (auto entry = text_cache_small_->get("Group Actions", theme_.text_muted, 0)) {
    SDL_Rect dst{group_action_area.x, group_action_area.y - 18, entry->w, entry->h};
    SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
  }
  if (auto entry = text_cache_small_->get("Files", theme_.text_muted, 0)) {
    SDL_Rect dst{file_list_area.x, file_list_area.y - 18, entry->w, entry->h};
    SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
  }
  if (auto entry = text_cache_small_->get("Transfers", theme_.text_muted, 0)) {
    SDL_Rect dst{transfer_area.x, transfer_area.y - 18, entry->w, entry->h};
    SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
  }

  renderUserList(user_list_area, input);
  renderGroupList(group_list_area, input);
  renderGroupActions(group_action_area, input);
  renderMessageArea(message_area, input);
  renderInputArea(input_area, input);
  renderFileList(file_list_area, input);
  renderTransfers(transfer_area, input);
}

void UiApp::renderUserList(const UiRect& bounds, const UiInput& input) {
  const auto& users = state_.onlineUsers();
  const int total_height = static_cast<int>(users.size()) * kRowHeight;
  const int max_scroll = std::max(0, total_height - bounds.h);
  user_scroll_y_ = std::clamp(user_scroll_y_, 0, max_scroll);

  int y = bounds.y - user_scroll_y_;
  for (const auto& user : users) {
    UiRect row{bounds.x, y, bounds.w, kRowHeight - 2};
    if (row.y + row.h < bounds.y) {
      y += kRowHeight;
      continue;
    }
    if (row.y > bounds.y + bounds.h) {
      break;
    }
    SDL_Color color = theme_.text;
    if (active_type_ == "private" && active_id_ == user.user_id) {
      SDL_SetRenderDrawColor(renderer_,
                             theme_.accent.r,
                             theme_.accent.g,
                             theme_.accent.b,
                             80);
      auto rect = row.toSdl();
      SDL_RenderFillRect(renderer_, &rect);
    }
    const std::string label = user.nickname.empty() ? user.user_id : user.nickname;
    if (auto entry = text_cache_small_->get(label, color, 0)) {
      SDL_Rect dst{row.x + 6, row.y + 4, entry->w, entry->h};
      SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
    }
    if (input.mouse_clicked && row.contains(input.mouse_x, input.mouse_y)) {
      selectConversation("private", user.user_id);
    }
    y += kRowHeight;
  }
}

void UiApp::renderGroupList(const UiRect& bounds, const UiInput& input) {
  const int total_height = static_cast<int>(groups_.size()) * kRowHeight;
  const int max_scroll = std::max(0, total_height - bounds.h);
  group_scroll_y_ = std::clamp(group_scroll_y_, 0, max_scroll);

  int y = bounds.y - group_scroll_y_;
  for (const auto& group : groups_) {
    UiRect row{bounds.x, y, bounds.w, kRowHeight - 2};
    if (row.y + row.h < bounds.y) {
      y += kRowHeight;
      continue;
    }
    if (row.y > bounds.y + bounds.h) {
      break;
    }
    SDL_Color color = theme_.text;
    if (active_type_ == "group" && active_id_ == group.group_id) {
      SDL_SetRenderDrawColor(renderer_,
                             theme_.accent.r,
                             theme_.accent.g,
                             theme_.accent.b,
                             80);
      auto rect = row.toSdl();
      SDL_RenderFillRect(renderer_, &rect);
    }
    const std::string label = group.name.empty() ? group.group_id : group.name;
    if (auto entry = text_cache_small_->get(label, color, 0)) {
      SDL_Rect dst{row.x + 6, row.y + 4, entry->w, entry->h};
      SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
    }
    if (input.mouse_clicked && row.contains(input.mouse_x, input.mouse_y)) {
      selectConversation("group", group.group_id);
      group_id_input_.value = group.group_id;
      if (!group.name.empty()) {
        group_name_input_.value = group.name;
      }
    }
    y += kRowHeight;
  }
}

void UiApp::renderGroupActions(const UiRect& bounds, const UiInput& input) {
  bool clicked_input = false;
  auto drawInput = [&](UiRect input_rect, TextInput& field) {
    SDL_SetRenderDrawColor(renderer_,
                           theme_.input_bg.r,
                           theme_.input_bg.g,
                           theme_.input_bg.b,
                           theme_.input_bg.a);
    SDL_Rect sdl_rect = input_rect.toSdl();
    SDL_RenderFillRect(renderer_, &sdl_rect);
    SDL_Color border = hasFocus(field) ? theme_.accent : theme_.border;
    SDL_SetRenderDrawColor(renderer_, border.r, border.g, border.b, border.a);
    SDL_RenderDrawRect(renderer_, &sdl_rect);
    const std::string display = field.value.empty() ? field.placeholder : field.value;
    SDL_Color color = field.value.empty() ? theme_.text_muted : theme_.text;
    if (auto entry = text_cache_small_->get(display, color, input_rect.w - 16)) {
      SDL_Rect dst{input_rect.x + 6, input_rect.y + 6, entry->w, entry->h};
      SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
    }
    if (input.mouse_clicked && input_rect.contains(input.mouse_x, input.mouse_y)) {
      setFocus(&field);
      clicked_input = true;
    }
  };

  int y = bounds.y;
  drawInput({bounds.x, y, bounds.w, 28}, group_id_input_);
  y += 36;
  drawInput({bounds.x, y, bounds.w, 28}, group_name_input_);
  y += 36;
  drawInput({bounds.x, y, bounds.w, 28}, group_target_input_);
  y += 40;

  UiRect btn_create{bounds.x, y, 70, 26};
  UiRect btn_join{bounds.x + 80, y, 70, 26};
  UiRect btn_leave{bounds.x + 160, y, 70, 26};
  y += 34;
  UiRect btn_rename{bounds.x, y, 70, 26};
  UiRect btn_dissolve{bounds.x + 80, y, 70, 26};
  UiRect btn_kick{bounds.x + 160, y, 70, 26};
  y += 34;
  UiRect btn_admin{bounds.x, y, 110, 26};
  UiRect btn_admin_off{bounds.x + 120, y, 110, 26};

  auto drawButton = [&](UiRect rect, const std::string& label) {
    const bool hover = rect.contains(input.mouse_x, input.mouse_y);
    SDL_Color color = hover ? theme_.button_hover : theme_.button;
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
    SDL_Rect sdl_rect = rect.toSdl();
    SDL_RenderFillRect(renderer_, &sdl_rect);
    if (auto entry = text_cache_small_->get(label, theme_.text, 0)) {
      SDL_Rect dst{rect.x + 8, rect.y + 6, entry->w, entry->h};
      SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
    }
  };

  drawButton(btn_create, "Create");
  drawButton(btn_join, "Join");
  drawButton(btn_leave, "Leave");
  drawButton(btn_rename, "Rename");
  drawButton(btn_dissolve, "Dissolve");
  drawButton(btn_kick, "Kick");
  drawButton(btn_admin, "Make Admin");
  drawButton(btn_admin_off, "Rm Admin");

  if (input.mouse_clicked && !clicked_input) {
    focused_input_ = nullptr;
  }

  if (!input.mouse_clicked) {
    return;
  }

  if (btn_create.contains(input.mouse_x, input.mouse_y)) {
    PendingGroupAction action;
    action.type = PendingGroupAction::Type::Create;
    action.group_name = group_name_input_.value;
    onGroupAction(action);
  } else if (btn_join.contains(input.mouse_x, input.mouse_y)) {
    PendingGroupAction action;
    action.type = PendingGroupAction::Type::Join;
    action.group_id = group_id_input_.value;
    action.group_name = group_name_input_.value;
    onGroupAction(action);
  } else if (btn_leave.contains(input.mouse_x, input.mouse_y)) {
    PendingGroupAction action;
    action.type = PendingGroupAction::Type::Leave;
    action.group_id = group_id_input_.value;
    onGroupAction(action);
  } else if (btn_rename.contains(input.mouse_x, input.mouse_y)) {
    PendingGroupAction action;
    action.type = PendingGroupAction::Type::Rename;
    action.group_id = group_id_input_.value;
    action.group_name = group_name_input_.value;
    onGroupAction(action);
  } else if (btn_dissolve.contains(input.mouse_x, input.mouse_y)) {
    PendingGroupAction action;
    action.type = PendingGroupAction::Type::Dissolve;
    action.group_id = group_id_input_.value;
    onGroupAction(action);
  } else if (btn_kick.contains(input.mouse_x, input.mouse_y)) {
    PendingGroupAction action;
    action.type = PendingGroupAction::Type::Kick;
    action.group_id = group_id_input_.value;
    action.target_user_id = group_target_input_.value;
    onGroupAction(action);
  } else if (btn_admin.contains(input.mouse_x, input.mouse_y)) {
    PendingGroupAction action;
    action.type = PendingGroupAction::Type::SetAdmin;
    action.group_id = group_id_input_.value;
    action.target_user_id = group_target_input_.value;
    action.make_admin = true;
    onGroupAction(action);
  } else if (btn_admin_off.contains(input.mouse_x, input.mouse_y)) {
    PendingGroupAction action;
    action.type = PendingGroupAction::Type::SetAdmin;
    action.group_id = group_id_input_.value;
    action.target_user_id = group_target_input_.value;
    action.make_admin = false;
    onGroupAction(action);
  }
}

void UiApp::renderMessageArea(const UiRect& bounds, const UiInput& input) {
  const auto* conversation = state_.getConversation(active_type_, active_id_);
  if (!conversation || conversation->messages.empty()) {
    if (auto entry = text_cache_small_->get("No messages", theme_.text_muted, 0)) {
      SDL_Rect dst{bounds.x + 8, bounds.y + 6, entry->w, entry->h};
      SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
    }
    return;
  }

  std::vector<std::string> lines;
  lines.reserve(conversation->messages.size());
  for (const auto& message : conversation->messages) {
    std::string line = "[" + formatTimestamp(message.created_at) + "] ";
    line += message.sender_nickname.empty() ? message.sender_id : message.sender_nickname;
    line += ": ";
    line += message.content;
    lines.push_back(std::move(line));
  }

  const int wrap_width = bounds.w - 12;
  int content_height = 0;
  std::vector<int> heights;
  heights.reserve(lines.size());
  for (const auto& line : lines) {
    const auto* entry = text_cache_->get(line, theme_.text, wrap_width);
    int h = entry ? entry->h : kRowHeight;
    heights.push_back(h);
    content_height += h + 6;
  }

  const int max_scroll = std::max(0, content_height - bounds.h);
  message_scroll_y_ = std::clamp(message_scroll_y_, 0, max_scroll);

  if (stick_to_bottom_) {
    message_scroll_y_ = max_scroll;
  }

  if (conversation->messages.size() != last_message_count_) {
    if (message_scroll_y_ >= max_scroll - 10) {
      stick_to_bottom_ = true;
      message_scroll_y_ = max_scroll;
    }
    last_message_count_ = conversation->messages.size();
  }

  int y = bounds.y - message_scroll_y_;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (y + heights[i] < bounds.y) {
      y += heights[i] + 6;
      continue;
    }
    if (y > bounds.y + bounds.h) {
      break;
    }
    const auto* entry = text_cache_->get(lines[i], theme_.text, wrap_width);
    if (entry) {
      SDL_Rect dst{bounds.x + 6, y, entry->w, entry->h};
      SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
    }
    y += heights[i] + 6;
  }

  if (input.mouse_clicked && bounds.contains(input.mouse_x, input.mouse_y)) {
    setFocus(&chat_input_);
  }

  const uint32_t now = SDL_GetTicks();
  if (message_scroll_y_ <= 0 &&
      state_.hasMoreHistory(active_type_, active_id_) &&
      now - last_history_request_ms_ > 800) {
    const auto before_id = state_.nextHistoryBeforeId(active_type_, active_id_);
    std::string error;
    if (api_.fetchHistory(active_type_, active_id_, before_id, config_.history_page_size, &error) == 0) {
      setStatusMessage("History fetch failed: " + error, theme_.danger, kStatusDurationMs);
    } else {
      last_history_request_ms_ = now;
    }
  }
}

void UiApp::renderInputArea(const UiRect& bounds, const UiInput& input) {
  SDL_SetRenderDrawColor(renderer_,
                         theme_.panel_alt.r,
                         theme_.panel_alt.g,
                         theme_.panel_alt.b,
                         theme_.panel_alt.a);
  SDL_Rect rect = bounds.toSdl();
  SDL_RenderFillRect(renderer_, &rect);

  bool clicked_input = false;
  auto drawInput = [&](UiRect input_rect, TextInput& field) {
    SDL_SetRenderDrawColor(renderer_,
                           theme_.input_bg.r,
                           theme_.input_bg.g,
                           theme_.input_bg.b,
                           theme_.input_bg.a);
    SDL_Rect sdl_rect = input_rect.toSdl();
    SDL_RenderFillRect(renderer_, &sdl_rect);
    SDL_Color border = hasFocus(field) ? theme_.accent : theme_.border;
    SDL_SetRenderDrawColor(renderer_, border.r, border.g, border.b, border.a);
    SDL_RenderDrawRect(renderer_, &sdl_rect);
    std::string display = field.value.empty() ? field.placeholder : field.value;
    SDL_Color color = field.value.empty() ? theme_.text_muted : theme_.text;
    if (auto entry = text_cache_->get(display, color, input_rect.w - 16)) {
      SDL_Rect dst{input_rect.x + 6, input_rect.y + 6, entry->w, entry->h};
      SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
    }
    if (input.mouse_clicked && input_rect.contains(input.mouse_x, input.mouse_y)) {
      setFocus(&field);
      clicked_input = true;
    }
  };

  UiRect message_input{bounds.x, bounds.y, bounds.w - 90, 32};
  UiRect send_button{bounds.x + bounds.w - 80, bounds.y, 80, 32};
  drawInput(message_input, chat_input_);

  SDL_SetRenderDrawColor(renderer_,
                         theme_.button.r,
                         theme_.button.g,
                         theme_.button.b,
                         theme_.button.a);
  SDL_Rect btn = send_button.toSdl();
  SDL_RenderFillRect(renderer_, &btn);
  if (auto entry = text_cache_small_->get("Send", theme_.text, 0)) {
    SDL_Rect dst{send_button.x + 20, send_button.y + 8, entry->w, entry->h};
    SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
  }
  if (input.mouse_clicked && send_button.contains(input.mouse_x, input.mouse_y)) {
    onSendMessage();
  }

  const auto emojis = emojiPalette();
  int emoji_x = bounds.x;
  int emoji_y = bounds.y + 40;
  for (const auto& emoji : emojis) {
    UiRect emoji_rect{emoji_x, emoji_y, 28, 28};
    SDL_SetRenderDrawColor(renderer_,
                           theme_.input_bg.r,
                           theme_.input_bg.g,
                           theme_.input_bg.b,
                           theme_.input_bg.a);
    SDL_Rect e_rect = emoji_rect.toSdl();
    SDL_RenderFillRect(renderer_, &e_rect);
    if (auto entry = text_cache_emoji_->get(emoji, theme_.text, 0)) {
      SDL_Rect dst{emoji_rect.x + 6, emoji_rect.y + 2, entry->w, entry->h};
      SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
    }
    if (input.mouse_clicked && emoji_rect.contains(input.mouse_x, input.mouse_y)) {
      chat_input_.value.append(emoji);
      setFocus(&chat_input_);
    }
    emoji_x += 32;
  }

  UiRect file_input{bounds.x, bounds.y + 68, bounds.w - 120, 26};
  UiRect file_button{bounds.x + bounds.w - 110, bounds.y + 68, 110, 26};
  drawInput(file_input, file_path_input_);

  SDL_SetRenderDrawColor(renderer_,
                         theme_.button.r,
                         theme_.button.g,
                         theme_.button.b,
                         theme_.button.a);
  SDL_Rect file_btn = file_button.toSdl();
  SDL_RenderFillRect(renderer_, &file_btn);
  if (auto entry = text_cache_small_->get("Send File", theme_.text, 0)) {
    SDL_Rect dst{file_button.x + 18, file_button.y + 6, entry->w, entry->h};
    SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
  }
  if (input.mouse_clicked && file_button.contains(input.mouse_x, input.mouse_y)) {
    onSendFile();
  }

  if (input.mouse_clicked && !clicked_input &&
      !send_button.contains(input.mouse_x, input.mouse_y) &&
      !file_button.contains(input.mouse_x, input.mouse_y)) {
    focused_input_ = nullptr;
  }
}

void UiApp::renderFileList(const UiRect& bounds, const UiInput& input) {
  const auto* conversation = state_.getConversation(active_type_, active_id_);
  if (!conversation || conversation->files.empty()) {
    if (auto entry = text_cache_small_->get("No files", theme_.text_muted, 0)) {
      SDL_Rect dst{bounds.x + 6, bounds.y + 4, entry->w, entry->h};
      SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
    }
    return;
  }

  const int total_height = static_cast<int>(conversation->files.size()) * kRowHeight;
  const int max_scroll = std::max(0, total_height - bounds.h);
  file_scroll_y_ = std::clamp(file_scroll_y_, 0, max_scroll);

  const auto& downloads = transfers_.downloadStates();
  int y = bounds.y - file_scroll_y_;
  for (const auto& notice : conversation->files) {
    UiRect row{bounds.x, y, bounds.w, kRowHeight - 2};
    if (row.y + row.h < bounds.y) {
      y += kRowHeight;
      continue;
    }
    if (row.y > bounds.y + bounds.h) {
      break;
    }

    const std::string label = notice.file_name + " (" + formatBytes(notice.file_size) + ")";
    if (auto entry = text_cache_small_->get(label, theme_.text, bounds.w - 80)) {
      SDL_Rect dst{row.x + 4, row.y + 4, entry->w, entry->h};
      SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
    }

    UiRect btn{row.x + row.w - 70, row.y + 2, 66, row.h - 4};
    const auto it = downloads.find(notice.file_id);
    std::string btn_label = "Download";
    SDL_Color btn_color = theme_.button;
    if (it != downloads.end() && it->second.done) {
      btn_label = "Done";
      btn_color = theme_.ok;
    }
    SDL_SetRenderDrawColor(renderer_, btn_color.r, btn_color.g, btn_color.b, btn_color.a);
    SDL_Rect btn_rect = btn.toSdl();
    SDL_RenderFillRect(renderer_, &btn_rect);
    if (auto entry = text_cache_small_->get(btn_label, theme_.text, 0)) {
      SDL_Rect dst{btn.x + 6, btn.y + 4, entry->w, entry->h};
      SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
    }
    if (input.mouse_clicked && btn.contains(input.mouse_x, input.mouse_y)) {
      if (it == downloads.end() || !it->second.done) {
        onDownloadFile(notice);
      }
    }
    y += kRowHeight;
  }
}

void UiApp::renderTransfers(const UiRect& bounds, const UiInput& input) {
  int y = bounds.y;
  const int bar_w = bounds.w - 8;
  const int bar_h = 10;

  auto drawTransfer = [&](const TransferState& state, const std::string& prefix) {
    std::string label = prefix + state.file_name;
    if (auto entry = text_cache_small_->get(label, theme_.text, bounds.w - 10)) {
      SDL_Rect dst{bounds.x + 4, y, entry->w, entry->h};
      SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
    }
    y += 16;

    SDL_SetRenderDrawColor(renderer_,
                           theme_.border.r,
                           theme_.border.g,
                           theme_.border.b,
                           theme_.border.a);
    SDL_Rect bar{bounds.x + 4, y, bar_w, bar_h};
    SDL_RenderFillRect(renderer_, &bar);
    const int filled = static_cast<int>(bar_w * state.progress());
    SDL_Color fill = state.failed ? theme_.danger : theme_.accent;
    SDL_SetRenderDrawColor(renderer_, fill.r, fill.g, fill.b, fill.a);
    SDL_Rect fill_rect{bounds.x + 4, y, filled, bar_h};
    SDL_RenderFillRect(renderer_, &fill_rect);
    y += bar_h + 6;
  };

  for (const auto& entry : transfers_.uploadStates()) {
    drawTransfer(entry.second, "Up: ");
    if (y > bounds.y + bounds.h - 20) {
      break;
    }
  }
  for (const auto& entry : transfers_.downloadStates()) {
    drawTransfer(entry.second, "Down: ");
    if (y > bounds.y + bounds.h - 20) {
      break;
    }
  }
  (void)input;
}

void UiApp::selectConversation(const std::string& type, const std::string& id) {
  if (type.empty() || id.empty()) {
    return;
  }
  if (active_type_ == type && active_id_ == id) {
    return;
  }
  active_type_ = type;
  active_id_ = id;
  message_scroll_y_ = 0;
  stick_to_bottom_ = true;
  last_message_count_ = 0;
  state_.resetHistoryCursor(type, id);
  std::string error;
  if (api_.fetchHistory(type, id, 0, config_.history_page_size, &error) == 0) {
    setStatusMessage("History fetch failed: " + error, theme_.danger, kStatusDurationMs);
  }
}

void UiApp::setStatusMessage(const std::string& message, SDL_Color color, uint32_t duration_ms) {
  status_message_ = message;
  status_color_ = color;
  status_until_ms_ = SDL_GetTicks() + duration_ms;
}

void UiApp::onLoginRequested() {
  if (login_user_input_.value.empty() || login_password_input_.value.empty()) {
    setStatusMessage("User ID and password required.", theme_.warn, kStatusDurationMs);
    return;
  }
  std::string error;
  if (api_.sendLogin(login_user_input_.value, login_password_input_.value, &error) == 0) {
    setStatusMessage("Login failed: " + error, theme_.danger, kStatusDurationMs);
    return;
  }
  saved_user_id_ = login_user_input_.value;
  saved_password_ = login_password_input_.value;
}

void UiApp::onRegisterRequested() {
  if (register_user_input_.value.empty() ||
      register_nick_input_.value.empty() ||
      register_password_input_.value.empty()) {
    setStatusMessage("User ID, nickname, and password required.", theme_.warn, kStatusDurationMs);
    return;
  }
  std::string error;
  if (api_.sendRegister(register_user_input_.value,
                        register_nick_input_.value,
                        register_password_input_.value,
                        &error) == 0) {
    setStatusMessage("Register failed: " + error, theme_.danger, kStatusDurationMs);
  }
}

void UiApp::onSendMessage() {
  if (chat_input_.value.empty()) {
    return;
  }
  if (active_type_.empty() || active_id_.empty()) {
    setStatusMessage("Select a conversation first.", theme_.warn, kStatusDurationMs);
    return;
  }
  std::string error;
  if (api_.sendMessage(active_type_, active_id_, chat_input_.value, &error) == 0) {
    setStatusMessage("Send failed: " + error, theme_.danger, kStatusDurationMs);
    return;
  }
  chat_input_.value.clear();
}

void UiApp::onSendFile() {
  if (file_path_input_.value.empty()) {
    setStatusMessage("File path required.", theme_.warn, kStatusDurationMs);
    return;
  }
  if (active_type_.empty() || active_id_.empty()) {
    setStatusMessage("Select a conversation first.", theme_.warn, kStatusDurationMs);
    return;
  }
  UploadRequest req;
  req.conversation_type = active_type_;
  req.conversation_id = active_id_;
  req.file_path = file_path_input_.value;
  std::string error;
  if (!transfers_.beginUpload(net_, req, nullptr, &error)) {
    setStatusMessage("Upload failed: " + error, theme_.danger, kStatusDurationMs);
    return;
  }
  setStatusMessage("Upload started.", theme_.ok, kStatusDurationMs);
}

void UiApp::onGroupAction(const PendingGroupAction& action) {
  if (!state_.loggedIn()) {
    setStatusMessage("Login required.", theme_.warn, kStatusDurationMs);
    return;
  }
  std::string error;
  uint64_t request_id = 0;
  switch (action.type) {
    case PendingGroupAction::Type::Create:
      if (action.group_name.empty()) {
        setStatusMessage("Group name required.", theme_.warn, kStatusDurationMs);
        return;
      }
      request_id = api_.createGroup(action.group_name, &error);
      break;
    case PendingGroupAction::Type::Join:
      if (action.group_id.empty()) {
        setStatusMessage("Group ID required.", theme_.warn, kStatusDurationMs);
        return;
      }
      request_id = api_.joinGroup(action.group_id, &error);
      break;
    case PendingGroupAction::Type::Leave:
      if (action.group_id.empty()) {
        setStatusMessage("Group ID required.", theme_.warn, kStatusDurationMs);
        return;
      }
      request_id = api_.leaveGroup(action.group_id, &error);
      break;
    case PendingGroupAction::Type::Rename:
      if (action.group_id.empty() || action.group_name.empty()) {
        setStatusMessage("Group ID and name required.", theme_.warn, kStatusDurationMs);
        return;
      }
      request_id = api_.renameGroup(action.group_id, action.group_name, &error);
      break;
    case PendingGroupAction::Type::Dissolve:
      if (action.group_id.empty()) {
        setStatusMessage("Group ID required.", theme_.warn, kStatusDurationMs);
        return;
      }
      request_id = api_.dissolveGroup(action.group_id, &error);
      break;
    case PendingGroupAction::Type::Kick:
      if (action.group_id.empty() || action.target_user_id.empty()) {
        setStatusMessage("Group ID and target user required.", theme_.warn, kStatusDurationMs);
        return;
      }
      request_id = api_.kickFromGroup(action.group_id, action.target_user_id, &error);
      break;
    case PendingGroupAction::Type::SetAdmin:
      if (action.group_id.empty() || action.target_user_id.empty()) {
        setStatusMessage("Group ID and target user required.", theme_.warn, kStatusDurationMs);
        return;
      }
      request_id = api_.setGroupAdmin(action.group_id, action.target_user_id, action.make_admin, &error);
      break;
  }
  if (request_id == 0) {
    setStatusMessage("Group action failed: " + error, theme_.danger, kStatusDurationMs);
    return;
  }
  group_requests_[request_id] = action;
}

void UiApp::onDownloadFile(const FileNotice& notice) {
  DownloadRequest req;
  req.conversation_type = notice.conversation_type;
  req.conversation_id = notice.conversation_id;
  req.file_id = notice.file_id;
  req.file_name = notice.file_name;
  req.file_size = notice.file_size;
  req.sha256 = notice.sha256;
  std::string error;
  if (!transfers_.beginDownload(net_, req, nullptr, &error)) {
    setStatusMessage("Download failed: " + error, theme_.danger, kStatusDurationMs);
    return;
  }
  setStatusMessage("Download started.", theme_.ok, kStatusDurationMs);
}

bool UiApp::hasFocus(const TextInput& input) const {
  return focused_input_ == &input;
}

void UiApp::setFocus(TextInput* input) {
  focused_input_ = input;
}

}  // namespace onlinetalk::client
