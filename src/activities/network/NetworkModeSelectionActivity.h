#pragma once

#include <functional>

#include "I18nKeys.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT };

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 */
class NetworkModeSelectionActivity final : public Activity {
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;
  const bool includeCalibre;
  const StrId headerTitle;

 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool includeCalibre = true,
                                       StrId headerTitle = StrId::STR_FILE_TRANSFER)
      : Activity("NetworkModeSelection", renderer, mappedInput), includeCalibre(includeCalibre), headerTitle(headerTitle) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  void onModeSelected(NetworkMode mode);
  void onCancel();

 private:
  int menuItemCount() const { return includeCalibre ? 3 : 2; }
};
