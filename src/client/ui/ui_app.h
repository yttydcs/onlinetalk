#pragma once

#include <SDL.h>
#include <SDL_ttf.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "client/file_transfer/file_transfer_manager.h"
#include "client/net/client_api.h"
#include "client/net/net_client.h"
#include "client/state/client_state.h"
#include "common/config.h"

namespace onlinetalk::client {

class UiApp {
 public:
  UiApp(const onlinetalk::common::ClientConfig& config,
        NetClient& net,
        ClientApi& api,
        ClientState& state,
        FileTransferManager& transfers);
  ~UiApp();

  bool init(std::string* error);
  void run();
  void shutdown();

 private:
  struct TextInput;
  struct UiInput;
  struct UiRect;
  struct UiTheme;
  struct GroupEntry;
  struct PendingGroupAction;
  class TextCache;

  bool handleTextInput(const char* text);
  bool handleKeyDown(SDL_Keycode key);
  void processNetwork();
  void handlePacket(const onlinetalk::common::Packet& packet);
  void updateConnection();
  void renderFrame(const UiInput& input);

  void renderAuthScreen(const UiRect& bounds, const UiInput& input);
  void renderChatScreen(const UiRect& bounds, const UiInput& input);
  void renderTopBar(const UiRect& bounds, const UiInput& input);
  void renderMessageArea(const UiRect& bounds, const UiInput& input);
  void renderUserList(const UiRect& bounds, const UiInput& input);
  void renderGroupList(const UiRect& bounds, const UiInput& input);
  void renderGroupActions(const UiRect& bounds, const UiInput& input);
  void renderFileList(const UiRect& bounds, const UiInput& input);
  void renderTransfers(const UiRect& bounds, const UiInput& input);
  void renderInputArea(const UiRect& bounds, const UiInput& input);

  void selectConversation(const std::string& type, const std::string& id);
  void setStatusMessage(const std::string& message, SDL_Color color, uint32_t duration_ms);
  void onLoginRequested();
  void onRegisterRequested();
  void onSendMessage();
  void onSendFile();
  void onGroupAction(const PendingGroupAction& action);
  void onDownloadFile(const FileNotice& notice);

  bool hasFocus(const TextInput& input) const;
  void setFocus(TextInput* input);

  const onlinetalk::common::ClientConfig& config_;
  NetClient& net_;
  ClientApi& api_;
  ClientState& state_;
  FileTransferManager& transfers_;

  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  TTF_Font* font_ = nullptr;
  TTF_Font* font_small_ = nullptr;
  TTF_Font* font_emoji_ = nullptr;

  std::unique_ptr<TextCache> text_cache_;
  std::unique_ptr<TextCache> text_cache_small_;
  std::unique_ptr<TextCache> text_cache_emoji_;

  bool running_ = false;
  bool show_register_ = false;

  TextInput* focused_input_ = nullptr;
  TextInput login_user_input_;
  TextInput login_password_input_;
  TextInput register_user_input_;
  TextInput register_nick_input_;
  TextInput register_password_input_;
  TextInput chat_input_;
  TextInput file_path_input_;
  TextInput group_id_input_;
  TextInput group_name_input_;
  TextInput group_target_input_;

  std::string active_type_;
  std::string active_id_;
  int message_scroll_y_ = 0;
  bool stick_to_bottom_ = true;
  size_t last_message_count_ = 0;
  uint32_t last_history_request_ms_ = 0;
  int user_scroll_y_ = 0;
  int group_scroll_y_ = 0;
  int file_scroll_y_ = 0;

  std::vector<GroupEntry> groups_;
  std::unordered_map<uint64_t, PendingGroupAction> group_requests_;

  std::string saved_user_id_;
  std::string saved_password_;
  uint32_t last_reconnect_ms_ = 0;
  bool was_connected_ = true;

  std::string status_message_;
  SDL_Color status_color_{255, 255, 255, 255};
  uint32_t status_until_ms_ = 0;

  UiTheme theme_;
};

}  // namespace onlinetalk::client
