#pragma once

#include <NetworkUdp.h>

#include <atomic>
#include <string>
#include <vector>

#include "NetworkModeSelectionActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct SharikOffer {
  IPAddress ip;
  uint16_t port = 0;
  uint16_t replyPort = 0;
  std::string name;
  std::string sender;
  std::string id;

  std::string url() const {
    return "http://" + std::string(ip.toString().c_str()) + ":" + std::to_string(port) + "/";
  }
};

enum class SharikReceiveState { MODE_SELECTION, WIFI_SELECTION, LISTENING, DOWNLOADING, ERROR };

/** Receives EPUB offers from Sharik-compatible senders on the local Wi-Fi network. */
class SharikReceiveActivity final : public Activity {
  static constexpr uint16_t DISCOVERY_PORT = 54545;
  static constexpr uint8_t MULTICAST_GROUP[4] = {239, 10, 10, 100};

  ButtonNavigator buttonNavigator;
  SharikReceiveState state = SharikReceiveState::MODE_SELECTION;
  NetworkUDP udp;
  bool udpActive = false;
  bool isApMode = false;
  bool didUnloadFonts_ = false;
  int selectedIndex = 0;
  std::vector<SharikOffer> offers;
  std::string statusMessage;
  std::string errorMessage;
  std::atomic<size_t> downloadProgress{0};
  std::atomic<size_t> downloadTotal{0};

  void launchModeSelection();
  void onNetworkModeSelected(NetworkMode mode);
  void onWifiSelectionComplete(bool connected);
  void startAccessPoint();
  void startListening();
  void stopListening();
  void pollOffers();
  void receiveSelected();
  void setStatus(std::string status);
  void setError(std::string error);

  static std::string safeFilename(const std::string& name);
  static std::string uniqueDestination(const std::string& name);

 public:
  explicit SharikReceiveActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SharikReceive", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == SharikReceiveState::LISTENING; }
  bool preventAutoSleep() override { return true; }
};
