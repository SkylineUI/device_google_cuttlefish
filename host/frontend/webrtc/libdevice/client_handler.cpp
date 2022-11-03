/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "ClientHandler"

#include "host/frontend/webrtc/libdevice/client_handler.h"

#include <vector>

#include <json/json.h>
#include <json/writer.h>
#include <netdb.h>
#include <openssl/rand.h>

#include <android-base/logging.h>

#include "host/frontend/webrtc/libcommon/signaling.h"
#include "host/frontend/webrtc/libcommon/utils.h"
#include "host/frontend/webrtc/libdevice/keyboard.h"
#include "host/libs/config/cuttlefish_config.h"

namespace cuttlefish {
namespace webrtc_streaming {

namespace {

static constexpr auto kInputChannelLabel = "input-channel";
static constexpr auto kAdbChannelLabel = "adb-channel";
static constexpr auto kBluetoothChannelLabel = "bluetooth-channel";
static constexpr auto kCameraDataChannelLabel = "camera-data-channel";
static constexpr auto kLocationDataChannelLabel = "location-channel";
static constexpr auto kKmlLocationsDataChannelLabel = "kml-locations-channel";
static constexpr auto kGpxLocationsDataChannelLabel = "gpx-locations-channel";
static constexpr auto kCameraDataEof = "EOF";

class CvdCreateSessionDescriptionObserver
    : public webrtc::CreateSessionDescriptionObserver {
 public:
  CvdCreateSessionDescriptionObserver(
      std::weak_ptr<ClientHandler> client_handler)
      : client_handler_(client_handler) {}

  void OnSuccess(webrtc::SessionDescriptionInterface *desc) override {
    auto client_handler = client_handler_.lock();
    if (client_handler) {
      client_handler->OnCreateSDPSuccess(desc);
    }
  }
  void OnFailure(webrtc::RTCError error) override {
    auto client_handler = client_handler_.lock();
    if (client_handler) {
      client_handler->OnCreateSDPFailure(error);
    }
  }

 private:
  std::weak_ptr<ClientHandler> client_handler_;
};

class CvdSetSessionDescriptionObserver
    : public webrtc::SetSessionDescriptionObserver {
 public:
  CvdSetSessionDescriptionObserver(std::weak_ptr<ClientHandler> client_handler)
      : client_handler_(client_handler) {}

  void OnSuccess() override {
    // local description set, nothing else to do
  }
  void OnFailure(webrtc::RTCError error) override {
    auto client_handler = client_handler_.lock();
    if (client_handler) {
      client_handler->OnSetSDPFailure(error);
    }
  }

 private:
  std::weak_ptr<ClientHandler> client_handler_;
};

class CvdOnSetRemoteDescription
    : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  CvdOnSetRemoteDescription(
      std::function<void(webrtc::RTCError error)> on_error)
      : on_error_(on_error) {}

  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    on_error_(error);
  }

 private:
  std::function<void(webrtc::RTCError error)> on_error_;
};

}  // namespace

// Video streams initiating in the client may be added and removed at unexpected
// times, causing the webrtc objects to be destroyed and created every time.
// This class hides away that complexity and allows to set up sinks only once.
class ClientVideoTrackImpl : public ClientVideoTrackInterface {
 public:
  void AddOrUpdateSink(rtc::VideoSinkInterface<webrtc::VideoFrame> *sink,
                       const rtc::VideoSinkWants &wants) override {
    sink_ = sink;
    wants_ = wants;
    if (video_track_) {
      video_track_->AddOrUpdateSink(sink, wants);
    }
  }

  void SetVideoTrack(webrtc::VideoTrackInterface *track) {
    video_track_ = track;
    if (sink_) {
      video_track_->AddOrUpdateSink(sink_, wants_);
    }
  }

  void UnsetVideoTrack(webrtc::VideoTrackInterface *track) {
    if (track == video_track_) {
      video_track_ = nullptr;
    }
  }

 private:
  webrtc::VideoTrackInterface* video_track_;
  rtc::VideoSinkInterface<webrtc::VideoFrame> *sink_ = nullptr;
  rtc::VideoSinkWants wants_ = {};
};

class InputChannelHandler : public webrtc::DataChannelObserver {
 public:
  InputChannelHandler(
      rtc::scoped_refptr<webrtc::DataChannelInterface> input_channel,
      std::shared_ptr<ConnectionObserver> observer);
  ~InputChannelHandler() override;

  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer &msg) override;

 private:
  rtc::scoped_refptr<webrtc::DataChannelInterface> input_channel_;
  std::shared_ptr<ConnectionObserver> observer_;
};

class AdbChannelHandler : public webrtc::DataChannelObserver {
 public:
  AdbChannelHandler(
      rtc::scoped_refptr<webrtc::DataChannelInterface> adb_channel,
      std::shared_ptr<ConnectionObserver> observer);
  ~AdbChannelHandler() override;

  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer &msg) override;

 private:
  rtc::scoped_refptr<webrtc::DataChannelInterface> adb_channel_;
  std::shared_ptr<ConnectionObserver> observer_;
  bool channel_open_reported_ = false;
};

class ControlChannelHandler : public webrtc::DataChannelObserver {
 public:
  ControlChannelHandler(
      rtc::scoped_refptr<webrtc::DataChannelInterface> control_channel,
      std::shared_ptr<ConnectionObserver> observer);
  ~ControlChannelHandler() override;

  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer &msg) override;

  bool Send(const Json::Value &message);
  bool Send(const uint8_t *msg, size_t size, bool binary);

 private:
  rtc::scoped_refptr<webrtc::DataChannelInterface> control_channel_;
  std::shared_ptr<ConnectionObserver> observer_;
};

class BluetoothChannelHandler : public webrtc::DataChannelObserver {
 public:
  BluetoothChannelHandler(
      rtc::scoped_refptr<webrtc::DataChannelInterface> bluetooth_channel,
      std::shared_ptr<ConnectionObserver> observer);
  ~BluetoothChannelHandler() override;

  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer &msg) override;

 private:
  rtc::scoped_refptr<webrtc::DataChannelInterface> bluetooth_channel_;
  std::shared_ptr<ConnectionObserver> observer_;
  bool channel_open_reported_ = false;
};

class LocationChannelHandler : public webrtc::DataChannelObserver {
 public:
  LocationChannelHandler(
      rtc::scoped_refptr<webrtc::DataChannelInterface> location_channel,
      std::shared_ptr<ConnectionObserver> observer);
  ~LocationChannelHandler() override;

  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer &msg) override;

 private:
  rtc::scoped_refptr<webrtc::DataChannelInterface> location_channel_;
  std::shared_ptr<ConnectionObserver> observer_;
  bool channel_open_reported_ = false;
};

class KmlLocationsChannelHandler : public webrtc::DataChannelObserver {
 public:
  KmlLocationsChannelHandler(
      rtc::scoped_refptr<webrtc::DataChannelInterface> kml_locations_channel,
      std::shared_ptr<ConnectionObserver> observer);
  ~KmlLocationsChannelHandler() override;

  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer &msg) override;

 private:
  rtc::scoped_refptr<webrtc::DataChannelInterface> kml_locations_channel_;
  std::shared_ptr<ConnectionObserver> observer_;
  bool channel_open_reported_ = false;
};

class GpxLocationsChannelHandler : public webrtc::DataChannelObserver {
 public:
  GpxLocationsChannelHandler(
      rtc::scoped_refptr<webrtc::DataChannelInterface> gpx_locations_channel,
      std::shared_ptr<ConnectionObserver> observer);
  ~GpxLocationsChannelHandler() override;

  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer &msg) override;

 private:
  rtc::scoped_refptr<webrtc::DataChannelInterface> gpx_locations_channel_;
  std::shared_ptr<ConnectionObserver> observer_;
  bool channel_open_reported_ = false;
};

class CameraChannelHandler : public webrtc::DataChannelObserver {
 public:
  CameraChannelHandler(
      rtc::scoped_refptr<webrtc::DataChannelInterface> bluetooth_channel,
      std::shared_ptr<ConnectionObserver> observer);
  ~CameraChannelHandler() override;

  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer &msg) override;

 private:
  rtc::scoped_refptr<webrtc::DataChannelInterface> camera_channel_;
  std::shared_ptr<ConnectionObserver> observer_;
  std::vector<char> receive_buffer_;
};

InputChannelHandler::InputChannelHandler(
    rtc::scoped_refptr<webrtc::DataChannelInterface> input_channel,
    std::shared_ptr<ConnectionObserver> observer)
    : input_channel_(input_channel), observer_(observer) {
  input_channel->RegisterObserver(this);
}

InputChannelHandler::~InputChannelHandler() {
  input_channel_->UnregisterObserver();
}

void InputChannelHandler::OnStateChange() {
  LOG(VERBOSE) << "Input channel state changed to "
               << webrtc::DataChannelInterface::DataStateString(
                      input_channel_->state());
}

void InputChannelHandler::OnMessage(const webrtc::DataBuffer &msg) {
  if (msg.binary) {
    // TODO (jemoreira) consider binary protocol to avoid JSON parsing overhead
    LOG(ERROR) << "Received invalid (binary) data on input channel";
    return;
  }
  auto size = msg.size();

  Json::Value evt;
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> json_reader(builder.newCharReader());
  std::string errorMessage;
  auto str = msg.data.cdata<char>();
  if (!json_reader->parse(str, str + size, &evt, &errorMessage) < 0) {
    LOG(ERROR) << "Received invalid JSON object over input channel: "
               << errorMessage;
    return;
  }
  if (!evt.isMember("type") || !evt["type"].isString()) {
    LOG(ERROR) << "Input event doesn't have a valid 'type' field: "
               << evt.toStyledString();
    return;
  }
  auto event_type = evt["type"].asString();
  if (event_type == "mouse") {
    auto result =
        ValidateJsonObject(evt, "mouse",
                           {{"down", Json::ValueType::intValue},
                            {"x", Json::ValueType::intValue},
                            {"y", Json::ValueType::intValue},
                            {"display_label", Json::ValueType::stringValue}});
    if (!result.ok()) {
      LOG(ERROR) << result.error().Trace();
      return;
    }
    auto label = evt["display_label"].asString();
    int32_t down = evt["down"].asInt();
    int32_t x = evt["x"].asInt();
    int32_t y = evt["y"].asInt();

    observer_->OnTouchEvent(label, x, y, down);
  } else if (event_type == "multi-touch") {
    auto result =
        ValidateJsonObject(evt, "multi-touch",
                           {{"id", Json::ValueType::arrayValue},
                            {"down", Json::ValueType::intValue},
                            {"x", Json::ValueType::arrayValue},
                            {"y", Json::ValueType::arrayValue},
                            {"slot", Json::ValueType::arrayValue},
                            {"display_label", Json::ValueType::stringValue}});
    if (!result.ok()) {
      LOG(ERROR) << result.error().Trace();
      return;
    }

    auto label = evt["display_label"].asString();
    auto idArr = evt["id"];
    int32_t down = evt["down"].asInt();
    auto xArr = evt["x"];
    auto yArr = evt["y"];
    auto slotArr = evt["slot"];
    int size = evt["id"].size();

    observer_->OnMultiTouchEvent(label, idArr, slotArr, xArr, yArr, down, size);
  } else if (event_type == "keyboard") {
    auto result =
        ValidateJsonObject(evt, "keyboard",
                           {{"event_type", Json::ValueType::stringValue},
                            {"keycode", Json::ValueType::stringValue}});
    if (!result.ok()) {
      LOG(ERROR) << result.error().Trace();
      return;
    }
    auto down = evt["event_type"].asString() == std::string("keydown");
    auto code = DomKeyCodeToLinux(evt["keycode"].asString());
    observer_->OnKeyboardEvent(code, down);
  } else {
    LOG(ERROR) << "Unrecognized event type: " << event_type;
    return;
  }
}

AdbChannelHandler::AdbChannelHandler(
    rtc::scoped_refptr<webrtc::DataChannelInterface> adb_channel,
    std::shared_ptr<ConnectionObserver> observer)
    : adb_channel_(adb_channel), observer_(observer) {
  adb_channel->RegisterObserver(this);
}

AdbChannelHandler::~AdbChannelHandler() { adb_channel_->UnregisterObserver(); }

void AdbChannelHandler::OnStateChange() {
  LOG(VERBOSE) << "Adb channel state changed to "
               << webrtc::DataChannelInterface::DataStateString(
                      adb_channel_->state());
}

void AdbChannelHandler::OnMessage(const webrtc::DataBuffer &msg) {
  // Report the adb channel as open on the first message received instead of at
  // channel open, this avoids unnecessarily connecting to the adb daemon for
  // clients that don't use ADB.
  if (!channel_open_reported_) {
    observer_->OnAdbChannelOpen([this](const uint8_t *msg, size_t size) {
      webrtc::DataBuffer buffer(rtc::CopyOnWriteBuffer(msg, size),
                                true /*binary*/);
      // TODO (b/185832105): When the SCTP channel is congested data channel
      // messages are buffered up to 16MB, when the buffer is full the channel
      // is abruptly closed. Keep track of the buffered data to avoid losing the
      // adb data channel.
      return adb_channel_->Send(buffer);
    });
    channel_open_reported_ = true;
  }
  observer_->OnAdbMessage(msg.data.cdata(), msg.size());
}

ControlChannelHandler::ControlChannelHandler(
    rtc::scoped_refptr<webrtc::DataChannelInterface> control_channel,
    std::shared_ptr<ConnectionObserver> observer)
    : control_channel_(control_channel), observer_(observer) {
  control_channel->RegisterObserver(this);
}

ControlChannelHandler::~ControlChannelHandler() {
  control_channel_->UnregisterObserver();
}

void ControlChannelHandler::OnStateChange() {
  auto state = control_channel_->state();
  LOG(VERBOSE) << "Control channel state changed to "
               << webrtc::DataChannelInterface::DataStateString(state);
  if (state == webrtc::DataChannelInterface::kOpen) {
    observer_->OnControlChannelOpen(
        [this](const Json::Value &message) { return this->Send(message); });
  }
}

void ControlChannelHandler::OnMessage(const webrtc::DataBuffer &msg) {
  auto msg_str = msg.data.cdata<char>();
  auto size = msg.size();
  Json::Value evt;
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> json_reader(builder.newCharReader());
  std::string errorMessage;
  if (!json_reader->parse(msg_str, msg_str + size, &evt, &errorMessage)) {
    LOG(ERROR) << "Received invalid JSON object over control channel: "
               << errorMessage;
    return;
  }

  auto result = ValidateJsonObject(
      evt, "command",
      /*required_fields=*/{{"command", Json::ValueType::stringValue}},
      /*optional_fields=*/
      {
          {"button_state", Json::ValueType::stringValue},
          {"lid_switch_open", Json::ValueType::booleanValue},
          {"hinge_angle_value", Json::ValueType::intValue},
      });
  if (!result.ok()) {
    LOG(ERROR) << result.error().Trace();
    return;
  }
  auto command = evt["command"].asString();

  if (command == "device_state") {
    if (evt.isMember("lid_switch_open")) {
      observer_->OnLidStateChange(evt["lid_switch_open"].asBool());
    }
    if (evt.isMember("hinge_angle_value")) {
      observer_->OnHingeAngleChange(evt["hinge_angle_value"].asInt());
    }
    return;
  } else if (command.rfind("camera_", 0) == 0) {
    observer_->OnCameraControlMsg(evt);
    return;
  }

  auto button_state = evt["button_state"].asString();
  LOG(VERBOSE) << "Control command: " << command << " (" << button_state << ")";
  if (command == "power") {
    observer_->OnPowerButton(button_state == "down");
  } else if (command == "back") {
    observer_->OnBackButton(button_state == "down");
  } else if (command == "home") {
    observer_->OnHomeButton(button_state == "down");
  } else if (command == "menu") {
    observer_->OnMenuButton(button_state == "down");
  } else if (command == "volumedown") {
    observer_->OnVolumeDownButton(button_state == "down");
  } else if (command == "volumeup") {
    observer_->OnVolumeUpButton(button_state == "down");
  } else {
    observer_->OnCustomActionButton(command, button_state);
  }
}

bool ControlChannelHandler::Send(const Json::Value& message) {
  Json::StreamWriterBuilder factory;
  std::string message_string = Json::writeString(factory, message);
  return Send(reinterpret_cast<const uint8_t*>(message_string.c_str()),
       message_string.size(), /*binary=*/false);
}

bool ControlChannelHandler::Send(const uint8_t *msg, size_t size, bool binary) {
  webrtc::DataBuffer buffer(rtc::CopyOnWriteBuffer(msg, size), binary);
  return control_channel_->Send(buffer);
}

BluetoothChannelHandler::BluetoothChannelHandler(
    rtc::scoped_refptr<webrtc::DataChannelInterface> bluetooth_channel,
    std::shared_ptr<ConnectionObserver> observer)
    : bluetooth_channel_(bluetooth_channel), observer_(observer) {
  bluetooth_channel_->RegisterObserver(this);
}

BluetoothChannelHandler::~BluetoothChannelHandler() {
  bluetooth_channel_->UnregisterObserver();
}

void BluetoothChannelHandler::OnStateChange() {
  LOG(VERBOSE) << "Bluetooth channel state changed to "
               << webrtc::DataChannelInterface::DataStateString(
                      bluetooth_channel_->state());
}

void BluetoothChannelHandler::OnMessage(const webrtc::DataBuffer &msg) {
  // Notify bluetooth channel opening when actually using the channel,
  // it has the same reason with AdbChannelHandler::OnMessage,
  // to avoid unnecessarily connection for Rootcanal.
  if (channel_open_reported_ == false) {
    channel_open_reported_ = true;
    observer_->OnBluetoothChannelOpen([this](const uint8_t *msg, size_t size) {
      webrtc::DataBuffer buffer(rtc::CopyOnWriteBuffer(msg, size),
                                true /*binary*/);
      // TODO (b/185832105): When the SCTP channel is congested data channel
      // messages are buffered up to 16MB, when the buffer is full the channel
      // is abruptly closed. Keep track of the buffered data to avoid losing the
      // adb data channel.
      return bluetooth_channel_->Send(buffer);
    });
  }

  observer_->OnBluetoothMessage(msg.data.cdata(), msg.size());
}

LocationChannelHandler::LocationChannelHandler(
    rtc::scoped_refptr<webrtc::DataChannelInterface> location_channel,
    std::shared_ptr<ConnectionObserver> observer)
    : location_channel_(location_channel), observer_(observer) {
  location_channel_->RegisterObserver(this);
}

LocationChannelHandler::~LocationChannelHandler() {
  location_channel_->UnregisterObserver();
}

void LocationChannelHandler::OnStateChange() {
  LOG(VERBOSE) << "Location channel state changed to "
               << webrtc::DataChannelInterface::DataStateString(
                      location_channel_->state());
}

void LocationChannelHandler::OnMessage(const webrtc::DataBuffer &msg) {
  // Notify location channel opening when actually using the channel,
  // it has the same reason with AdbChannelHandler::OnMessage,
  // to avoid unnecessarily connection for Rootcanal.
  if (channel_open_reported_ == false) {
    channel_open_reported_ = true;
    observer_->OnLocationChannelOpen([this](const uint8_t *msg, size_t size) {
      webrtc::DataBuffer buffer(rtc::CopyOnWriteBuffer(msg, size),
                                true /*binary*/);
      // messages are buffered up to 16MB, when the buffer is full the channel
      // is abruptly closed. Keep track of the buffered data to avoid losing the
      // adb data channel.
      location_channel_->Send(buffer);
      return true;
    });
  }

  observer_->OnLocationMessage(msg.data.cdata(), msg.size());
}

KmlLocationsChannelHandler::KmlLocationsChannelHandler(
    rtc::scoped_refptr<webrtc::DataChannelInterface> kml_locations_channel,
    std::shared_ptr<ConnectionObserver> observer)
    : kml_locations_channel_(kml_locations_channel), observer_(observer) {
  kml_locations_channel_->RegisterObserver(this);
}

KmlLocationsChannelHandler::~KmlLocationsChannelHandler() {
  kml_locations_channel_->UnregisterObserver();
}

void KmlLocationsChannelHandler::OnStateChange() {
  LOG(VERBOSE) << "KmlLocations channel state changed to "
               << webrtc::DataChannelInterface::DataStateString(
                      kml_locations_channel_->state());
}

void KmlLocationsChannelHandler::OnMessage(const webrtc::DataBuffer &msg) {
  // Notify kml_locations channel opening when actually using the channel,
  // it has the same reason with AdbChannelHandler::OnMessage,
  // to avoid unnecessarily connection for Rootcanal.
  if (channel_open_reported_ == false) {
    channel_open_reported_ = true;
    observer_->OnKmlLocationsChannelOpen(
        [this](const uint8_t *msg, size_t size) {
          webrtc::DataBuffer buffer(rtc::CopyOnWriteBuffer(msg, size),
                                    true /*binary*/);
          kml_locations_channel_->Send(buffer);
          return true;
        });
  }

  observer_->OnKmlLocationsMessage(msg.data.cdata(), msg.size());
}

GpxLocationsChannelHandler::GpxLocationsChannelHandler(
    rtc::scoped_refptr<webrtc::DataChannelInterface> gpx_locations_channel,
    std::shared_ptr<ConnectionObserver> observer)
    : gpx_locations_channel_(gpx_locations_channel), observer_(observer) {
  gpx_locations_channel_->RegisterObserver(this);
}

GpxLocationsChannelHandler::~GpxLocationsChannelHandler() {
  gpx_locations_channel_->UnregisterObserver();
}

void GpxLocationsChannelHandler::OnStateChange() {
  LOG(VERBOSE) << "GpxLocations channel state changed to "
               << webrtc::DataChannelInterface::DataStateString(
                      gpx_locations_channel_->state());
}

void GpxLocationsChannelHandler::OnMessage(const webrtc::DataBuffer &msg) {
  // Notify gpx_locations channel opening when actually using the channel,
  // it has the same reason with AdbChannelHandler::OnMessage,
  // to avoid unnecessarily connection for Rootcanal.
  if (channel_open_reported_ == false) {
    channel_open_reported_ = true;
    observer_->OnGpxLocationsChannelOpen(
        [this](const uint8_t *msg, size_t size) {
          webrtc::DataBuffer buffer(rtc::CopyOnWriteBuffer(msg, size),
                                    true /*binary*/);
          gpx_locations_channel_->Send(buffer);
          return true;
        });
  }

  observer_->OnGpxLocationsMessage(msg.data.cdata(), msg.size());
}

CameraChannelHandler::CameraChannelHandler(
    rtc::scoped_refptr<webrtc::DataChannelInterface> camera_channel,
    std::shared_ptr<ConnectionObserver> observer)
    : camera_channel_(camera_channel), observer_(observer) {
  camera_channel_->RegisterObserver(this);
}

CameraChannelHandler::~CameraChannelHandler() {
  camera_channel_->UnregisterObserver();
}

void CameraChannelHandler::OnStateChange() {
  LOG(VERBOSE) << "Camera channel state changed to "
               << webrtc::DataChannelInterface::DataStateString(
                      camera_channel_->state());
}

void CameraChannelHandler::OnMessage(const webrtc::DataBuffer &msg) {
  auto msg_data = msg.data.cdata<char>();
  if (msg.size() == strlen(kCameraDataEof) &&
      !strncmp(msg_data, kCameraDataEof, msg.size())) {
    // Send complete buffer to observer on EOF marker
    observer_->OnCameraData(receive_buffer_);
    receive_buffer_.clear();
    return;
  }
  // Otherwise buffer up data
  receive_buffer_.insert(receive_buffer_.end(), msg_data,
                         msg_data + msg.size());
}

std::shared_ptr<ClientHandler> ClientHandler::Create(
    int client_id, std::shared_ptr<ConnectionObserver> observer,
    PeerConnectionBuilder &connection_builder,
    std::function<void(const Json::Value &)> send_to_client_cb,
    std::function<void(bool)> on_connection_changed_cb) {
  return std::shared_ptr<ClientHandler>(
      new ClientHandler(client_id, observer, connection_builder,
                        send_to_client_cb, on_connection_changed_cb));
}

ClientHandler::ClientHandler(
    int client_id, std::shared_ptr<ConnectionObserver> observer,
    PeerConnectionBuilder &connection_builder,
    std::function<void(const Json::Value &)> send_to_client_cb,
    std::function<void(bool)> on_connection_changed_cb)
    : client_id_(client_id),
      observer_(observer),
      send_to_client_(send_to_client_cb),
      on_connection_changed_cb_(on_connection_changed_cb),
      connection_builder_(connection_builder),
      camera_track_(new ClientVideoTrackImpl()) {}

ClientHandler::~ClientHandler() {
  for (auto &data_channel : data_channels_) {
    data_channel->UnregisterObserver();
  }
}

bool ClientHandler::AddDisplay(
    rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track,
    const std::string &label) {
  auto [it, inserted] = displays_.emplace(label, DisplayTrackAndSender{
                                                     .track = video_track,
                                                 });
  if (peer_connection_) {
    // Send each track as part of a different stream with the label as id
    auto err_or_sender =
        peer_connection_->AddTrack(video_track, {label} /* stream_id */);
    if (!err_or_sender.ok()) {
      LOG(ERROR) << "Failed to add video track to the peer connection";
      return false;
    }

    DisplayTrackAndSender &info = it->second;
    info.sender = err_or_sender.MoveValue();
  }
  return true;
}

bool ClientHandler::RemoveDisplay(const std::string &label) {
  auto it = displays_.find(label);
  if (it == displays_.end()) {
    return false;
  }

  if (peer_connection_) {
    DisplayTrackAndSender &info = it->second;

    bool success = peer_connection_->RemoveTrack(info.sender);
    if (!success) {
      LOG(ERROR) << "Failed to remove video track for display: " << label;
      return false;
    }
  }

  displays_.erase(it);
  return true;
}

bool ClientHandler::AddAudio(
    rtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track,
    const std::string &label) {
  // Store the audio track for when the peer connection is created
  audio_streams_.emplace_back(audio_track, label);
  if (peer_connection_) {
    // Send each track as part of a different stream with the label as id
    auto err_or_sender =
        peer_connection_->AddTrack(audio_track, {label} /* stream_id */);
    if (!err_or_sender.ok()) {
      LOG(ERROR) << "Failed to add video track to the peer connection";
      return false;
    }
  }
  return true;
}

ClientVideoTrackInterface* ClientHandler::GetCameraStream() {
  return camera_track_.get();
}

void ClientHandler::LogAndReplyError(const std::string &error_msg) const {
  LOG(ERROR) << error_msg;
  Json::Value reply;
  reply["type"] = "error";
  reply["error"] = error_msg;
  send_to_client_(reply);
}

void ClientHandler::AddPendingIceCandidates() {
  // Add any ice candidates that arrived before the remote description
  for (auto& candidate: pending_ice_candidates_) {
    peer_connection_->AddIceCandidate(std::move(candidate),
                                      [this](webrtc::RTCError error) {
                                        if (!error.ok()) {
                                          LogAndReplyError(error.message());
                                        }
                                      });
  }
  pending_ice_candidates_.clear();
}

bool ClientHandler::BuildPeerConnection(
    const std::vector<webrtc::PeerConnectionInterface::IceServer>
        &ice_servers) {
  peer_connection_ = connection_builder_.Build(this, ice_servers);
  if (!peer_connection_) {
    return false;
  }

  // Re-add the video and audio tracks after the peer connection has been
  // created
  decltype(displays_) tmp_displays;
  tmp_displays.swap(displays_);
  for (auto &[label, info] : tmp_displays) {
    if (!AddDisplay(info.track, label)) {
      return false;
    }
  }
  decltype(audio_streams_) tmp_audio_streams;
  tmp_audio_streams.swap(audio_streams_);
  for (auto &pair : tmp_audio_streams) {
    auto &audio_track = pair.first;
    auto &label = pair.second;
    if (!AddAudio(audio_track, label)) {
      return false;
    }
  }

  // libwebrtc configures the video encoder with a start bitrate of just 300kbs
  // which causes it to drop the first 4 frames it receives. Any value over 2Mbs
  // will be capped at 2Mbs when passed to the encoder by the peer_connection
  // object, so we pass the maximum possible value here.
  webrtc::BitrateSettings bitrate_settings;
  bitrate_settings.start_bitrate_bps = 2000000;  // 2Mbs
  peer_connection_->SetBitrate(bitrate_settings);

  // At least one data channel needs to be created on the side that makes the
  // SDP offer (the device) for data channels to be enabled at all.
  // This channel is meant to carry control commands from the client.
  auto control_channel = peer_connection_->CreateDataChannel(
      "device-control", nullptr /* config */);
  if (!control_channel) {
    LOG(ERROR) << "Failed to create control data channel";
    return false;
  }
  control_handler_.reset(new ControlChannelHandler(control_channel, observer_));

  return true;
}

void ClientHandler::OnCreateSDPSuccess(
    webrtc::SessionDescriptionInterface *desc) {
  std::string offer_str;
  desc->ToString(&offer_str);
  std::string sdp_type = desc->type();
  peer_connection_->SetLocalDescription(
      // The peer connection wraps this raw pointer with a scoped_refptr, so
      // it's guaranteed to be deleted at some point
      new rtc::RefCountedObject<CvdSetSessionDescriptionObserver>(
          weak_from_this()),
      desc);
  // The peer connection takes ownership of the description so it should not be
  // used after this
  desc = nullptr;

  Json::Value reply;
  reply["type"] = sdp_type;
  reply["sdp"] = offer_str;

  send_to_client_(reply);
}

void ClientHandler::OnCreateSDPFailure(webrtc::RTCError error) {
  LogAndReplyError(error.message());
  Close();
}

void ClientHandler::OnSetSDPFailure(webrtc::RTCError error) {
  LogAndReplyError(error.message());
  LOG(ERROR) << "Error setting local description: Either there is a bug in "
                "libwebrtc or the local description was (incorrectly) modified "
                "after creating it";
  Close();
}

Result<void> ClientHandler::CreateOffer() {
  peer_connection_->CreateOffer(
      // No memory leak here because this is a ref counted object and the
      // peer connection immediately wraps it with a scoped_refptr
      new rtc::RefCountedObject<CvdCreateSessionDescriptionObserver>(
          weak_from_this()),
      webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  // The created offer wil be sent to the client on
  // OnSuccess(webrtc::SessionDescriptionInterface* desc)
  return {};
}

Result<void> ClientHandler::OnOfferRequestMsg(
    const std::vector<webrtc::PeerConnectionInterface::IceServer>
        &ice_servers) {
  // The peer connection must be created on the first request-offer
  CF_EXPECT(BuildPeerConnection(ice_servers), "Failed to create peer connection");
  return {};
}

Result<void> ClientHandler::OnOfferMsg(
    std::unique_ptr<webrtc::SessionDescriptionInterface> offer) {
  rtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface> observer(
      new rtc::RefCountedObject<CvdOnSetRemoteDescription>(
          [this](webrtc::RTCError error) {
            if (!error.ok()) {
              LogAndReplyError(error.message());
              // The remote description was rejected, this client can't be
              // trusted anymore.
              Close();
              return;
            }
            remote_description_added_ = true;
            AddPendingIceCandidates();
            peer_connection_->CreateAnswer(
                // No memory leak here because this is a ref counted objects and
                // the peer connection immediately wraps it with a scoped_refptr
                new rtc::RefCountedObject<CvdCreateSessionDescriptionObserver>(
                    weak_from_this()),
                webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
          }));
  peer_connection_->SetRemoteDescription(std::move(offer), observer);
  return {};
}

Result<void> ClientHandler::OnAnswerMsg(
    std::unique_ptr<webrtc::SessionDescriptionInterface> answer) {
  rtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface> observer(
      new rtc::RefCountedObject<CvdOnSetRemoteDescription>(
          [this](webrtc::RTCError error) {
            if (!error.ok()) {
              LogAndReplyError(error.message());
              // The remote description was rejected, this client can't be
              // trusted anymore.
              Close();
            }
          }));
  peer_connection_->SetRemoteDescription(std::move(answer), observer);
  remote_description_added_ = true;
  AddPendingIceCandidates();
  return {};
}

Result<void> ClientHandler::OnIceCandidateMsg(
    std::unique_ptr<webrtc::IceCandidateInterface> candidate) {
  if (remote_description_added_) {
    peer_connection_->AddIceCandidate(std::move(candidate),
                                      [this](webrtc::RTCError error) {
                                        if (!error.ok()) {
                                          LogAndReplyError(error.message());
                                        }
                                      });
  } else {
    // Store the ice candidate to be added later if it arrives before the
    // remote description. This could happen if the client uses polling
    // instead of websockets because the candidates are generated immediately
    // after the remote (offer) description is set and the events and the ajax
    // calls are asynchronous.
    pending_ice_candidates_.push_back(std::move(candidate));
  }
  return {};
}

Result<void> ClientHandler::OnErrorMsg(const std::string &msg) {
  // The client shouldn't reply with error to the device
  LOG(ERROR) << "Client error message: " << msg;
  // Don't return an error, the message was handled successfully.
  return {};
}

void ClientHandler::HandleMessage(const Json::Value &message) {
  auto result = HandleSignalingMessage(message, *this);
  if (!result.ok()) {
    LogAndReplyError(result.error().Trace());
  }
}

void ClientHandler::Close() {
  // We can't simply call peer_connection_->Close() here because this method
  // could be called from one of the PeerConnectionObserver callbacks and that
  // would lead to a deadlock (Close eventually tries to destroy an object that
  // will then wait for the callback to return -> deadlock). Destroying the
  // peer_connection_ has the same effect. The only alternative is to postpone
  // that operation until after the callback returns.
  on_connection_changed_cb_(false);
}

void ClientHandler::OnConnectionChange(
    webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
  switch (new_state) {
    case webrtc::PeerConnectionInterface::PeerConnectionState::kNew:
      break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kConnecting:
      break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:
      LOG(VERBOSE) << "Client " << client_id_ << ": WebRTC connected";
      observer_->OnConnected();
      on_connection_changed_cb_(true);
      break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected:
      LOG(VERBOSE) << "Client " << client_id_ << ": Connection disconnected";
      Close();
      break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:
      LOG(ERROR) << "Client " << client_id_ << ": Connection failed";
      Close();
      break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:
      LOG(VERBOSE) << "Client " << client_id_ << ": Connection closed";
      Close();
      break;
  }
}

void ClientHandler::OnIceCandidate(
    const webrtc::IceCandidateInterface *candidate) {
  std::string candidate_sdp;
  candidate->ToString(&candidate_sdp);
  auto sdp_mid = candidate->sdp_mid();
  auto line_index = candidate->sdp_mline_index();

  Json::Value reply;
  reply["type"] = "ice-candidate";
  reply["mid"] = sdp_mid;
  reply["mLineIndex"] = static_cast<Json::UInt64>(line_index);
  reply["candidate"] = candidate_sdp;

  send_to_client_(reply);
}

void ClientHandler::OnDataChannel(
    rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
  auto label = data_channel->label();
  if (label == kInputChannelLabel) {
    input_handler_.reset(new InputChannelHandler(data_channel, observer_));
  } else if (label == kAdbChannelLabel) {
    adb_handler_.reset(new AdbChannelHandler(data_channel, observer_));
  } else if (label == kBluetoothChannelLabel) {
    bluetooth_handler_.reset(
        new BluetoothChannelHandler(data_channel, observer_));
  } else if (label == kCameraDataChannelLabel) {
    camera_data_handler_.reset(
        new CameraChannelHandler(data_channel, observer_));
  } else if (label == kLocationDataChannelLabel) {
    location_handler_.reset(
        new LocationChannelHandler(data_channel, observer_));
  } else if (label == kKmlLocationsDataChannelLabel) {
    kml_location_handler_.reset(
        new KmlLocationsChannelHandler(data_channel, observer_));
  } else if (label == kGpxLocationsDataChannelLabel) {
    gpx_location_handler_.reset(
        new GpxLocationsChannelHandler(data_channel, observer_));
  } else {
    LOG(VERBOSE) << "Data channel connected: " << label;
    data_channels_.push_back(data_channel);
  }
}

void ClientHandler::OnRenegotiationNeeded() {
  LOG(VERBOSE) << "Client " << client_id_ << " needs renegotiation";
  auto result = CreateOffer();
  if (!result.ok()) {
    LOG(ERROR) << "Failed to create offer on renegotiation: "
               << result.error().Trace();
  }
}

void ClientHandler::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
  std::string state_str;
  switch (new_state) {
    case webrtc::PeerConnectionInterface::IceGatheringState::kIceGatheringNew:
      state_str = "NEW";
      break;
    case webrtc::PeerConnectionInterface::IceGatheringState::
        kIceGatheringGathering:
      state_str = "GATHERING";
      break;
    case webrtc::PeerConnectionInterface::IceGatheringState::
        kIceGatheringComplete:
      state_str = "COMPLETE";
      break;
    default:
      state_str = "UNKNOWN";
  }
  LOG(VERBOSE) << "Client " << client_id_
               << ": ICE Gathering state set to: " << state_str;
}

void ClientHandler::OnIceCandidateError(const std::string &host_candidate,
                                        const std::string &url, int error_code,
                                        const std::string &error_text) {
  LOG(VERBOSE) << "Gathering of an ICE candidate (host candidate: "
               << host_candidate << ", url: " << url
               << ") failed: " << error_text;
}

void ClientHandler::OnIceCandidateError(const std::string &address, int port,
                                        const std::string &url, int error_code,
                                        const std::string &error_text) {
  LOG(VERBOSE) << "Gathering of an ICE candidate (address: " << address
               << ", port: " << port << ", url: " << url
               << ") failed: " << error_text;
}

void ClientHandler::OnSignalingChange(
    webrtc::PeerConnectionInterface::SignalingState new_state) {
  // ignore
}
void ClientHandler::OnStandardizedIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  switch (new_state) {
    case webrtc::PeerConnectionInterface::kIceConnectionNew:
      LOG(DEBUG) << "ICE connection state: New";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionChecking:
      LOG(DEBUG) << "ICE connection state: Checking";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionConnected:
      LOG(DEBUG) << "ICE connection state: Connected";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionCompleted:
      LOG(DEBUG) << "ICE connection state: Completed";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionFailed:
      LOG(DEBUG) << "ICE connection state: Failed";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionDisconnected:
      LOG(DEBUG) << "ICE connection state: Disconnected";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionClosed:
      LOG(DEBUG) << "ICE connection state: Closed";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionMax:
      LOG(DEBUG) << "ICE connection state: Max";
      break;
  }
}
void ClientHandler::OnIceCandidatesRemoved(
    const std::vector<cricket::Candidate> &candidates) {
  // ignore
}
void ClientHandler::OnTrack(
    rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
  auto track = transceiver->receiver()->track();
  if (track && track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
    // It's ok to take the raw pointer here because we make sure to unset it
    // when the track is removed
    camera_track_->SetVideoTrack(
        static_cast<webrtc::VideoTrackInterface *>(track.get()));
  }
}
void ClientHandler::OnRemoveTrack(
    rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
  auto track = receiver->track();
  if (track && track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
    // this only unsets if the track matches the one already in store
    camera_track_->UnsetVideoTrack(
        reinterpret_cast<webrtc::VideoTrackInterface *>(track.get()));
  }
}

}  // namespace webrtc_streaming
}  // namespace cuttlefish