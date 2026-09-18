#include "SharikReceiveActivity.h"

#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr const char* AP_SSID = "CrossMosa-Reader";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;

bool isMultiFileOffer(const char* name) {
  const char* colon = std::strchr(name, ':');
  if (!colon || colon == name) return false;
  for (const char* p = name; p < colon; ++p) {
    if (*p < '0' || *p > '9') return false;
  }
  return true;
}
}

void SharikReceiveActivity::onEnter() {
  Activity::onEnter();

  state = SharikReceiveState::MODE_SELECTION;
  isApMode = false;
  selectedIndex = 0;
  offers.clear();
  statusMessage.clear();
  errorMessage.clear();
  downloadProgress = 0;
  downloadTotal = 0;
  launchModeSelection();
}

void SharikReceiveActivity::launchModeSelection() {
  state = SharikReceiveState::MODE_SELECTION;
  requestUpdate();
  startActivityForResult(
      std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput, false, StrId::STR_RECEIVE_EPUB),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          onGoHome();
        } else {
          onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
        }
      });
}

void SharikReceiveActivity::onExit() {
  Activity::onExit();
  stopListening();

  if (didUnloadFonts_ && WiFi.getMode() == WIFI_MODE_NULL) {
    extern SdCardFontSystem sdFontSystem;
    sdFontSystem.ensureLoaded(renderer);
    didUnloadFonts_ = false;
  }

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    if (isApMode) {
      WiFi.softAPdisconnect(true);
    } else {
      WiFi.disconnect(false);
    }
    delay(30);
    silentRestart();
  }
}

void SharikReceiveActivity::onNetworkModeSelected(const NetworkMode mode) {
  extern SdCardFontSystem sdFontSystem;
  sdFontSystem.unloadForLowMemory(renderer);
  didUnloadFonts_ = true;

  isApMode = mode == NetworkMode::CREATE_HOTSPOT;
  if (mode == NetworkMode::JOIN_NETWORK) {
    state = SharikReceiveState::WIFI_SELECTION;
    WiFi.mode(WIFI_STA);
    requestUpdate();
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             onWifiSelectionComplete(!result.isCancelled);
                           });
  } else {
    startAccessPoint();
  }
}

void SharikReceiveActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    startListening();
  } else {
    launchModeSelection();
  }
}

void SharikReceiveActivity::startAccessPoint() {
  state = SharikReceiveState::LISTENING;
  requestUpdate();

  WiFi.mode(WIFI_AP);
  delay(100);
  if (!WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS)) {
    setError("Unable to start hotspot");
    state = SharikReceiveState::ERROR;
    requestUpdate();
    return;
  }
  startListening();
}

void SharikReceiveActivity::startListening() {
  if (udpActive) {
    udp.stop();
    udpActive = false;
  }

  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  {
    RenderLock lock;
    offers.clear();
    selectedIndex = 0;
    errorMessage.clear();
  }
  downloadProgress = 0;
  downloadTotal = 0;

  const IPAddress group(MULTICAST_GROUP[0], MULTICAST_GROUP[1], MULTICAST_GROUP[2], MULTICAST_GROUP[3]);
  udpActive = udp.beginMulticast(group, DISCOVERY_PORT);
  if (!udpActive) {
    // Sharik also sends a limited broadcast. This fallback supports APs and
    // Wi-Fi drivers that do not allow multicast group membership.
    udpActive = udp.begin(DISCOVERY_PORT);
  }

  if (!udpActive) {
    setError("Cannot listen for Sharik senders");
    state = SharikReceiveState::ERROR;
    requestUpdate();
    return;
  }

  state = SharikReceiveState::LISTENING;
  String address = isApMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  setStatus(std::string(tr(STR_SHARIK_WAITING)) + " " + address.c_str());
  requestUpdate();
}

void SharikReceiveActivity::stopListening() {
  if (udpActive) {
    udp.stop();
    udpActive = false;
  }
}

void SharikReceiveActivity::pollOffers() {
  if (!udpActive) return;

  for (int packets = 0; packets < 8; ++packets) {
    const int packetSize = udp.parsePacket();
    if (packetSize <= 0) return;

    char buffer[768];
    const int readSize = udp.read(buffer, sizeof(buffer) - 1);
    if (readSize <= 0) continue;
    buffer[readSize] = '\0';

    JsonDocument document;
    if (deserializeJson(document, buffer)) continue;
    if (strcmp(document["type"] | "", "file") != 0) continue;

    const char* name = document["name"] | "";
    const int port = document["port"] | 0;
    // Sharik names multi-file offers as "N: name1 name2" and serves an HTML
    // listing at /. This receiver intentionally handles one EPUB per offer;
    // do not mistake that listing for an EPUB binary.
    if (name[0] == '\0' || isMultiFileOffer(name) || port <= 0 || port > 65535 ||
        !FsHelpers::hasEpubExtension(std::string_view(name)))
      continue;

    const IPAddress senderIp = udp.remoteIP();
    const uint16_t replyPort = udp.remotePort();
    const std::string senderName = std::string(document["deviceName"] | "Sharik sender");
    const std::string offerId = std::string(senderIp.toString().c_str()) + ":" + std::to_string(port) + ":" + name;

    bool alreadySeen = false;
    {
      RenderLock lock;
      alreadySeen = std::any_of(offers.begin(), offers.end(), [&offerId](const SharikOffer& offer) {
        return offer.id == offerId;
      });
    }
    if (alreadySeen) continue;

    const char reply[] = "CrossMosa";
    udp.beginPacket(senderIp, replyPort);
    udp.write(reinterpret_cast<const uint8_t*>(reply), sizeof(reply) - 1);
    udp.endPacket();

    {
      RenderLock lock;
      offers.push_back(SharikOffer{senderIp, static_cast<uint16_t>(port), replyPort, name, senderName, offerId});
    }
    requestUpdate();
  }
}

void SharikReceiveActivity::loop() {
  if (state == SharikReceiveState::LISTENING) {
    pollOffers();

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome(HomeMenuItem::SHARIK_RECEIVE);
      return;
    }

    if (!offers.empty()) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      const int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
      switch (handleListTouch(selectedIndex, static_cast<int>(offers.size()), contentTop, contentHeight, true)) {
        case ListTouchResult::Activated:
          receiveSelected();
          return;
        case ListTouchResult::Consumed:
          return;
        case ListTouchResult::None:
          break;
      }

      buttonNavigator.onNext([this] {
        selectedIndex = ButtonNavigator::nextIndex(selectedIndex, offers.size());
        requestUpdate();
      });
      buttonNavigator.onPrevious([this] {
        selectedIndex = ButtonNavigator::previousIndex(selectedIndex, offers.size());
        requestUpdate();
      });
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) receiveSelected();
    return;
  }

  if (state == SharikReceiveState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome(HomeMenuItem::SHARIK_RECEIVE);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startListening();
    }
  }
}

void SharikReceiveActivity::receiveSelected() {
  if (offers.empty() || selectedIndex < 0 || selectedIndex >= static_cast<int>(offers.size())) return;

  SharikOffer offer;
  {
    RenderLock lock;
    offer = offers[selectedIndex];
  }

  state = SharikReceiveState::DOWNLOADING;
  downloadProgress = 0;
  downloadTotal = 0;
  setStatus(std::string(tr(STR_SHARIK_RECEIVING)) + " " + offer.name);
  requestUpdate(true);

  const std::string destination = uniqueDestination(safeFilename(offer.name));
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
  const auto result = HttpDownloader::downloadToFile(
      offer.url(), destination,
      [this, &lastRenderedPercent, &lastProgressUpdateMs](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
        const unsigned long now = millis();
        if (percent >= 100 || lastRenderedPercent < 0 || percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
            now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
      });

  if (result == HttpDownloader::OK) {
    clearBookCache(destination);
    setStatus(std::string(tr(STR_SHARIK_RECEIVED)) + ": " + destination);
    state = SharikReceiveState::LISTENING;
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_OPEN_BOOK_NOW), destination),
        [this, destination](const ActivityResult& result) {
          if (!result.isCancelled) onSelectBook(destination);
        });
  } else {
    setError(std::string(tr(STR_DOWNLOAD_FAILED)) +
             (HttpDownloader::lastError[0] == '\0' ? "" : std::string(" (")) +
             (HttpDownloader::lastError[0] == '\0' ? "" : std::string(HttpDownloader::lastError) + ")"));
    state = SharikReceiveState::ERROR;
  }
  requestUpdate();
}

void SharikReceiveActivity::setStatus(std::string status) {
  RenderLock lock;
  statusMessage = std::move(status);
}

void SharikReceiveActivity::setError(std::string error) {
  RenderLock lock;
  errorMessage = std::move(error);
}

std::string SharikReceiveActivity::safeFilename(const std::string& input) {
  const size_t slash = input.find_last_of("/\\");
  std::string name = slash == std::string::npos ? input : input.substr(slash + 1);
  std::replace_if(name.begin(), name.end(), [](const unsigned char c) {
    return c < 32 || std::strchr("/:*?\"<>|", c) != nullptr;
  }, '_');
  while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
  while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
  if (name.empty() || name == "." || name == "..") name = "received.epub";
  return name;
}

std::string SharikReceiveActivity::uniqueDestination(const std::string& name) {
  auto pathFor = [](const std::string& filename) { return "/" + filename; };
  if (!Storage.exists(pathFor(name).c_str())) return pathFor(name);

  const size_t dot = name.find_last_of('.');
  const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
  const std::string extension = dot == std::string::npos ? "" : name.substr(dot);
  for (int index = 1; index <= 999; ++index) {
    const std::string candidate = stem + " (" + std::to_string(index) + ")" + extension;
    if (!Storage.exists(pathFor(candidate).c_str())) return pathFor(candidate);
  }
  return pathFor(name);
}

void SharikReceiveActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  const char* subtitle = nullptr;
  if (state == SharikReceiveState::LISTENING && !statusMessage.empty()) subtitle = statusMessage.c_str();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_RECEIVE_EPUB), subtitle);

  if (state == SharikReceiveState::LISTENING) {
    if (offers.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_SHARIK_WAITING));
    } else {
      GUI.drawList(
          renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(offers.size()), selectedIndex,
          [this](const int index) { return offers[index].name; },
          [this](const int index) { return offers[index].sender; }, [](const int) { return UIIcon::Wifi; });
    }
  } else if (state == SharikReceiveState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, statusMessage.c_str());
    const size_t total = downloadTotal.load();
    if (total > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress.load(), total);
    }
  } else if (state == SharikReceiveState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_DOWNLOAD_FAILED));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 20, errorMessage.c_str());
  }

  const char* firstHint = state == SharikReceiveState::ERROR ? tr(STR_BACK) : tr(STR_BACK);
  const char* secondHint = state == SharikReceiveState::ERROR ? tr(STR_RETRY) : tr(STR_SELECT);
  GUI.drawButtonHints(renderer, firstHint, secondHint, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  renderer.displayBuffer();
}
