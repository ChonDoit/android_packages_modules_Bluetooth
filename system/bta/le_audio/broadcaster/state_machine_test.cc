/*
 * Copyright 2021 HIMSA II K/S - www.himsa.com.
 * Represented by EHIMA - www.ehima.com
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

#include "state_machine.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../le_audio_types.h"
#include "ble_advertiser.h"
#include "btm_iso_api.h"
#include "mock_ble_advertising_manager.h"
#include "mock_iso_manager.h"
#include "stack/include/ble_advertiser.h"
#include "state_machine.h"

using namespace bluetooth::hci::iso_manager;

using bluetooth::hci::IsoManager;
using testing::_;
using testing::Mock;
using testing::SaveArg;
using testing::Test;

std::map<std::string, int> mock_function_count_map;

// Disables most likely false-positives from base::SplitString()
extern "C" const char* __asan_default_options() {
  return "detect_container_overflow=0";
}

void btsnd_hcic_ble_rand(base::Callback<void(BT_OCTET8)> cb) {}

namespace le_audio {
namespace broadcaster {
namespace {

class MockBroadcastStatMachineCallbacks
    : public IBroadcastStateMachineCallbacks {
 public:
  MockBroadcastStatMachineCallbacks() = default;
  MockBroadcastStatMachineCallbacks(const MockBroadcastStatMachineCallbacks&) =
      delete;
  MockBroadcastStatMachineCallbacks& operator=(
      const MockBroadcastStatMachineCallbacks&) = delete;

  ~MockBroadcastStatMachineCallbacks() override = default;

  MOCK_METHOD((void), OnStateMachineCreateStatus,
              (uint8_t instance_id, bool initialized), (override));
  MOCK_METHOD((void), OnStateMachineDestroyed, (uint8_t instance_id),
              (override));
  MOCK_METHOD((void), OnStateMachineEvent,
              (uint8_t instance_id, BroadcastStateMachine::State state,
               const void* data),
              (override));
  MOCK_METHOD((void), OnOwnAddressResponse,
              (uint8_t instance_id, uint8_t addr_type, RawAddress addr),
              (override));
  MOCK_METHOD((uint8_t), GetNumRetransmit, (uint8_t instance_id), (override));
  MOCK_METHOD((uint32_t), GetSduItv, (uint8_t instance_id), (override));
  MOCK_METHOD((uint16_t), GetMaxTransportLatency, (uint8_t instance_id),
              (override));
};

class StateMachineTest : public Test {
 protected:
  void SetUp() override {
    mock_function_count_map.clear();
    BleAdvertisingManager::Initialize(nullptr);

    ble_advertising_manager_ = BleAdvertisingManager::Get();
    mock_ble_advertising_manager_ =
        static_cast<MockBleAdvertisingManager*>(ble_advertising_manager_.get());

    sm_callbacks_.reset(new MockBroadcastStatMachineCallbacks());
    BroadcastStateMachine::Initialize(sm_callbacks_.get());

    ON_CALL(*mock_ble_advertising_manager_, StartAdvertisingSet)
        .WillByDefault([](base::Callback<void(uint8_t, int8_t, uint8_t)> cb,
                          tBTM_BLE_ADV_PARAMS* params,
                          std::vector<uint8_t> advertise_data,
                          std::vector<uint8_t> scan_response_data,
                          tBLE_PERIODIC_ADV_PARAMS* periodic_params,
                          std::vector<uint8_t> periodic_data, uint16_t duration,
                          uint8_t maxExtAdvEvents,
                          base::Callback<void(uint8_t, uint8_t)> timeout_cb) {
          static uint8_t advertiser_id = 1;
          uint8_t tx_power = 32;
          uint8_t status = 0;
          cb.Run(advertiser_id++, tx_power, status);
        });

    ON_CALL(*mock_ble_advertising_manager_, Enable)
        .WillByDefault(
            [](uint8_t advertiser_id, bool enable,
               base::Callback<void(uint8_t /* status */)> cb, uint16_t duration,
               uint8_t maxExtAdvEvents,
               base::Callback<void(uint8_t /* status */)> timeout_cb) {
              cb.Run(0);
            });

    ON_CALL(*mock_ble_advertising_manager_, GetOwnAddress)
        .WillByDefault(
            [](uint8_t inst_id, BleAdvertisingManager::GetAddressCallback cb) {
              uint8_t address_type = 0x02;
              RawAddress address;
              const uint8_t addr[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
              address.FromOctets(addr);
              cb.Run(address_type, address);
            });

    ON_CALL(*(sm_callbacks_.get()), OnStateMachineCreateStatus)
        .WillByDefault([this](uint8_t instance_id, bool initialized) {
          auto instance_it =
              std::find_if(pending_broadcasts_.begin(),
                           pending_broadcasts_.end(), [instance_id](auto& up) {
                             return (up->GetInstanceId() == instance_id);
                           });
          if (instance_it != pending_broadcasts_.end()) {
            if (initialized) {
              broadcasts_[instance_id] = std::move(*instance_it);
            }
            pending_broadcasts_.erase(instance_it);
          }
          instance_creation_promise_.set_value(instance_id);
        });

    ON_CALL(*(sm_callbacks_.get()), OnStateMachineDestroyed)
        .WillByDefault([this](uint8_t instance_id) {
          if (broadcasts_.count(instance_id)) {
            instance_destruction_promise_.set_value(instance_id);
          }
        });

    ConfigureIsoManagerMock();
  }

  void ConfigureIsoManagerMock() {
    iso_manager_ = IsoManager::GetInstance();
    ASSERT_NE(iso_manager_, nullptr);
    iso_manager_->Start();

    mock_iso_manager_ = MockIsoManager::GetInstance();
    ASSERT_NE(mock_iso_manager_, nullptr);

    ON_CALL(*mock_iso_manager_, CreateBig)
        .WillByDefault([this](uint8_t big_id, big_create_params p) {
          if (!broadcasts_[big_id]) return;

          big_create_cmpl_evt evt;
          evt.big_id = big_id;

          // For test convenience lets encode big_id into conn_hdl's
          // MSB
          static uint8_t conn_lsb = 1;
          uint16_t conn_msb = ((uint16_t)big_id) << 8;
          for (auto i = 0; i < p.num_bis; ++i) {
            evt.conn_handles.push_back(conn_msb | conn_lsb++);
          }

          broadcasts_[big_id]->HandleHciEvent(HCI_BLE_CREATE_BIG_CPL_EVT, &evt);
        });

    ON_CALL(*mock_iso_manager_, SetupIsoDataPath)
        .WillByDefault([this](uint16_t conn_handle, iso_data_path_params p) {
          // Get the big_id encoded in conn_handle's MSB
          if (!broadcasts_[conn_handle >> 8]) return;
          broadcasts_[conn_handle >> 8]->OnSetupIsoDataPath(0, conn_handle);
        });

    ON_CALL(*mock_iso_manager_, RemoveIsoDataPath)
        .WillByDefault([this](uint16_t conn_handle, uint8_t iso_direction) {
          // Get the big_id encoded in conn_handle's MSB
          if (!broadcasts_[conn_handle >> 8]) return;
          broadcasts_[conn_handle >> 8]->OnRemoveIsoDataPath(0, conn_handle);
        });

    ON_CALL(*mock_iso_manager_, TerminateBig)
        .WillByDefault([this](uint8_t big_id, uint8_t reason) {
          if (!broadcasts_[big_id]) return;
          big_terminate_cmpl_evt evt;
          evt.big_id = big_id;
          evt.reason = reason;

          broadcasts_[big_id]->HandleHciEvent(HCI_BLE_TERM_BIG_CPL_EVT, &evt);
        });
  }

  void TearDown() override {
    iso_manager_->Stop();
    mock_iso_manager_ = nullptr;

    broadcasts_.clear();
    sm_callbacks_.reset();
  }

  uint8_t InstantiateStateMachine(
      LeAudioBroadcaster::AudioProfile profile =
          LeAudioBroadcaster::AudioProfile::SONIFICATION) {
    // We will get the state machine create status update in an async callback
    // so let's wait for it here.
    instance_creation_promise_ = std::promise<uint8_t>();
    std::future<uint8_t> instance_future =
        instance_creation_promise_.get_future();

    auto codec_wrapper =
        BroadcastCodecWrapper::getCodecConfigForProfile(profile);
    pending_broadcasts_.push_back(BroadcastStateMachine::CreateInstance({
        .codec_wrapper = std::move(codec_wrapper),
        // .streaming_phy = ,
        // .announcement = ,
        // .broadcast_code = ,
    }));
    pending_broadcasts_.back()->Initialize();
    return instance_future.get();
  }

  base::WeakPtr<BleAdvertisingManager> ble_advertising_manager_;

  MockBleAdvertisingManager* mock_ble_advertising_manager_;
  IsoManager* iso_manager_;
  MockIsoManager* mock_iso_manager_;

  std::map<uint8_t, std::unique_ptr<BroadcastStateMachine>> broadcasts_;
  std::vector<std::unique_ptr<BroadcastStateMachine>> pending_broadcasts_;
  std::unique_ptr<MockBroadcastStatMachineCallbacks> sm_callbacks_;
  std::promise<uint8_t> instance_creation_promise_;
  std::promise<uint8_t> instance_destruction_promise_;
};

TEST_F(StateMachineTest, CreateInstanceFailed) {
  EXPECT_CALL(*mock_ble_advertising_manager_, StartAdvertisingSet)
      .WillOnce([](base::Callback<void(uint8_t, int8_t, uint8_t)> cb,
                   tBTM_BLE_ADV_PARAMS* params,
                   std::vector<uint8_t> advertise_data,
                   std::vector<uint8_t> scan_response_data,
                   tBLE_PERIODIC_ADV_PARAMS* periodic_params,
                   std::vector<uint8_t> periodic_data, uint16_t duration,
                   uint8_t maxExtAdvEvents,
                   base::Callback<void(uint8_t, uint8_t)> timeout_cb) {
        uint8_t advertiser_id = 1;
        uint8_t tx_power = 0;
        uint8_t status = 1;
        cb.Run(advertiser_id, tx_power, status);
      });

  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineCreateStatus(_, false))
      .Times(1);

  uint8_t instance_id = InstantiateStateMachine();
  ASSERT_NE(instance_id, BroadcastStateMachine::kInstanceIdUndefined);
  ASSERT_TRUE(pending_broadcasts_.empty());
  ASSERT_TRUE(broadcasts_.empty());
}

TEST_F(StateMachineTest, CreateInstanceTimeout) {
  EXPECT_CALL(*mock_ble_advertising_manager_, StartAdvertisingSet)
      .WillOnce([](base::Callback<void(uint8_t, int8_t, uint8_t)> cb,
                   tBTM_BLE_ADV_PARAMS* params,
                   std::vector<uint8_t> advertise_data,
                   std::vector<uint8_t> scan_response_data,
                   tBLE_PERIODIC_ADV_PARAMS* periodic_params,
                   std::vector<uint8_t> periodic_data, uint16_t duration,
                   uint8_t maxExtAdvEvents,
                   base::Callback<void(uint8_t, uint8_t)> timeout_cb) {
        uint8_t advertiser_id = 1;
        uint8_t status = 1;
        timeout_cb.Run(advertiser_id, status);
      });

  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineCreateStatus(_, false))
      .Times(1);

  uint8_t instance_id = InstantiateStateMachine();
  ASSERT_NE(instance_id, BroadcastStateMachine::kInstanceIdUndefined);
  ASSERT_TRUE(pending_broadcasts_.empty());
  ASSERT_TRUE(broadcasts_.empty());
}

TEST_F(StateMachineTest, CreateInstanceSuccess) {
  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineCreateStatus(_, true))
      .Times(1);

  uint8_t instance_id = InstantiateStateMachine();
  ASSERT_NE(instance_id, BroadcastStateMachine::kInstanceIdUndefined);
  ASSERT_TRUE(pending_broadcasts_.empty());
  ASSERT_FALSE(broadcasts_.empty());
  ASSERT_EQ(broadcasts_[instance_id]->GetInstanceId(), instance_id);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::CONFIGURED);
}

TEST_F(StateMachineTest, DestroyInstanceSuccess) {
  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineCreateStatus(_, true))
      .Times(1);

  uint8_t instance_id = InstantiateStateMachine();
  ASSERT_NE(instance_id, BroadcastStateMachine::kInstanceIdUndefined);
  ASSERT_FALSE(broadcasts_.empty());

  instance_destruction_promise_ = std::promise<uint8_t>();
  std::future<uint8_t> instance_future =
      instance_destruction_promise_.get_future();

  broadcasts_.clear();
  EXPECT_EQ(instance_future.get(), instance_id);
}

TEST_F(StateMachineTest, GetAdvertisingAddress) {
  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineCreateStatus(_, true))
      .Times(1);

  uint8_t instance_id = InstantiateStateMachine();
  EXPECT_CALL(*(sm_callbacks_.get()), OnOwnAddressResponse(instance_id, _, _))
      .Times(1);
  broadcasts_[instance_id]->RequestOwnAddress();
}

TEST_F(StateMachineTest, Mute) {
  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineCreateStatus(_, true))
      .Times(1);

  uint8_t instance_id = InstantiateStateMachine();
  ASSERT_TRUE(pending_broadcasts_.empty());
  ASSERT_FALSE(broadcasts_.empty());

  ASSERT_FALSE(broadcasts_[instance_id]->IsMuted());
  broadcasts_[instance_id]->SetMuted(true);
  ASSERT_TRUE(broadcasts_[instance_id]->IsMuted());
  broadcasts_[instance_id]->SetMuted(false);
  ASSERT_FALSE(broadcasts_[instance_id]->IsMuted());
}

static BasicAudioAnnouncementData prepareAnnouncement(
    const BroadcastCodecWrapper& codec_config, std::vector<uint8_t> metadata) {
  BasicAudioAnnouncementData announcement;

  announcement.presentation_delay = 0x004E20;

  announcement.subgroup_configs = {{
      .codec_config =
          {
              .codec_id = codec_config.GetLeAudioCodecId().coding_format,
              .vendor_company_id =
                  codec_config.GetLeAudioCodecId().vendor_company_id,
              .vendor_codec_id =
                  codec_config.GetLeAudioCodecId().vendor_codec_id,
              .codec_specific_params = codec_config.GetCodecSpecData(),
          },
      .metadata = metadata,
      .bis_configs = {},
  }};

  for (uint8_t i = 0; i < codec_config.GetNumChannels(); ++i) {
    announcement.subgroup_configs[0].bis_configs.push_back(
        {.codec_specific_params = {}, .bis_index = i});
  }

  return announcement;
}

TEST_F(StateMachineTest, UpdateAnnouncement) {
  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineCreateStatus(_, true))
      .Times(1);

  uint8_t instance_id = InstantiateStateMachine();
  std::vector<uint8_t> metadata = {};
  BroadcastCodecWrapper codec_config(
      {.coding_format = le_audio::types::kLeAudioCodingFormatLC3,
       .vendor_company_id = le_audio::types::kLeAudioVendorCompanyIdUndefined,
       .vendor_codec_id = le_audio::types::kLeAudioVendorCodecIdUndefined},
      {.num_channels = LeAudioCodecConfiguration::kChannelNumberMono,
       .sample_rate = LeAudioCodecConfiguration::kSampleRate16000,
       .bits_per_sample = LeAudioCodecConfiguration::kBitsPerSample16,
       .data_interval_us = LeAudioCodecConfiguration::kInterval10000Us},
      32000, 40);
  auto announcement = prepareAnnouncement(codec_config, metadata);

  std::vector<uint8_t> data;
  EXPECT_CALL(*mock_ble_advertising_manager_,
              SetPeriodicAdvertisingData(instance_id, _, _))
      .Times(2)
      .WillRepeatedly(SaveArg<1>(&data));
  broadcasts_[instance_id]->UpdateBroadcastAnnouncement(
      std::move(announcement));

  uint8_t first_len = data.size();
  ASSERT_NE(first_len, 0);  // Non-zero length
  ASSERT_EQ(data[1], BTM_BLE_AD_TYPE_SERVICE_DATA_TYPE);
  ASSERT_EQ(data[2], (kBasicAudioAnnouncementServiceUuid & 0x00FF));
  ASSERT_EQ(data[3], ((kBasicAudioAnnouncementServiceUuid >> 8) & 0x00FF));
  // The rest of the packet data is already covered by the announcement tests

  // Verify that changes in the announcement makes a difference
  metadata = {0x02, 0x01, 0x03};
  announcement = prepareAnnouncement(codec_config, metadata);
  broadcasts_[instance_id]->UpdateBroadcastAnnouncement(
      std::move(announcement));
  uint8_t second_len = data.size();

  // These should differ by the difference in metadata
  ASSERT_EQ(first_len + metadata.size(), second_len);
}

TEST_F(StateMachineTest, ProcessMessageStartWhenConfigured) {
  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineCreateStatus(_, true))
      .Times(1);

  auto sound_profile = LeAudioBroadcaster::AudioProfile::MEDIA;
  uint8_t num_channels = 2;

  uint8_t instance_id = InstantiateStateMachine(sound_profile);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::CONFIGURED);

  uint8_t num_bises = 0;
  EXPECT_CALL(*mock_iso_manager_, CreateBig)
      .WillOnce([this, &num_bises](uint8_t big_id, big_create_params p) {
        if (!broadcasts_[big_id]) return;

        num_bises = p.num_bis;

        big_create_cmpl_evt evt;
        evt.big_id = big_id;

        // For test convenience lets encode big_id into conn_hdl's
        // MSB
        static uint8_t conn_lsb = 1;
        uint16_t conn_msb = ((uint16_t)big_id) << 8;
        for (auto i = 0; i < p.num_bis; ++i) {
          evt.conn_handles.push_back(conn_msb | conn_lsb++);
        }

        broadcasts_[big_id]->HandleHciEvent(HCI_BLE_CREATE_BIG_CPL_EVT, &evt);
      });

  EXPECT_CALL(*mock_iso_manager_, SetupIsoDataPath).Times(num_channels);
  EXPECT_CALL(*mock_iso_manager_, RemoveIsoDataPath).Times(0);
  EXPECT_CALL(*(sm_callbacks_.get()),
              OnStateMachineEvent(instance_id,
                                  BroadcastStateMachine::State::STREAMING, _))
      .Times(1);
  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::START);

  // Verify the right number of BISes in the BIG being created
  ASSERT_EQ(num_bises, num_channels);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STREAMING);
}

TEST_F(StateMachineTest, ProcessMessageStopWhenConfigured) {
  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineCreateStatus(_, true))
      .Times(1);

  uint8_t instance_id =
      InstantiateStateMachine(LeAudioBroadcaster::AudioProfile::MEDIA);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::CONFIGURED);

  EXPECT_CALL(*mock_iso_manager_, SetupIsoDataPath).Times(0);
  EXPECT_CALL(*mock_iso_manager_, RemoveIsoDataPath).Times(0);
  EXPECT_CALL(*(sm_callbacks_.get()),
              OnStateMachineEvent(instance_id,
                                  BroadcastStateMachine::State::STOPPING, _))
      .Times(1);
  EXPECT_CALL(*(sm_callbacks_.get()),
              OnStateMachineEvent(instance_id,
                                  BroadcastStateMachine::State::STOPPED, _))
      .Times(1);
  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::STOP);

  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STOPPED);
}

TEST_F(StateMachineTest, ProcessMessageSuspendWhenConfigured) {
  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineCreateStatus(_, true))
      .Times(1);

  uint8_t instance_id =
      InstantiateStateMachine(LeAudioBroadcaster::AudioProfile::MEDIA);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::CONFIGURED);

  EXPECT_CALL(*mock_iso_manager_, SetupIsoDataPath).Times(0);
  EXPECT_CALL(*mock_iso_manager_, RemoveIsoDataPath).Times(0);
  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineEvent(instance_id, _, _))
      .Times(0);
  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::SUSPEND);
  // There shall be no change in state
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::CONFIGURED);
}

TEST_F(StateMachineTest, ProcessMessageStartWhenStreaming) {
  uint8_t instance_id =
      InstantiateStateMachine(LeAudioBroadcaster::AudioProfile::MEDIA);

  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::START);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STREAMING);

  EXPECT_CALL(*mock_iso_manager_, SetupIsoDataPath).Times(0);
  EXPECT_CALL(*mock_iso_manager_, RemoveIsoDataPath).Times(0);
  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineEvent(instance_id, _, _))
      .Times(0);
  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::START);

  // There shall be no change in state
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STREAMING);
}

TEST_F(StateMachineTest, ProcessMessageStopWhenStreaming) {
  uint8_t instance_id =
      InstantiateStateMachine(LeAudioBroadcaster::AudioProfile::MEDIA);

  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::START);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STREAMING);

  EXPECT_CALL(*mock_iso_manager_, SetupIsoDataPath).Times(0);
  EXPECT_CALL(*mock_iso_manager_, RemoveIsoDataPath).Times(2);
  EXPECT_CALL(*(sm_callbacks_.get()),
              OnStateMachineEvent(instance_id,
                                  BroadcastStateMachine::State::STOPPING, _))
      .Times(1);
  EXPECT_CALL(*(sm_callbacks_.get()),
              OnStateMachineEvent(instance_id,
                                  BroadcastStateMachine::State::STOPPED, _))
      .Times(1);
  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::STOP);

  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STOPPED);
}

TEST_F(StateMachineTest, ProcessMessageSuspendWhenStreaming) {
  uint8_t instance_id =
      InstantiateStateMachine(LeAudioBroadcaster::AudioProfile::MEDIA);

  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::START);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STREAMING);

  EXPECT_CALL(*mock_iso_manager_, SetupIsoDataPath).Times(0);
  EXPECT_CALL(*mock_iso_manager_, RemoveIsoDataPath).Times(2);
  EXPECT_CALL(*(sm_callbacks_.get()),
              OnStateMachineEvent(instance_id,
                                  BroadcastStateMachine::State::CONFIGURED, _))
      .Times(1);
  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::SUSPEND);

  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::CONFIGURED);
}

TEST_F(StateMachineTest, ProcessMessageStartWhenStopped) {
  uint8_t instance_id =
      InstantiateStateMachine(LeAudioBroadcaster::AudioProfile::MEDIA);

  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::STOP);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STOPPED);

  EXPECT_CALL(*mock_iso_manager_, SetupIsoDataPath).Times(2);
  EXPECT_CALL(*mock_iso_manager_, RemoveIsoDataPath).Times(0);
  EXPECT_CALL(*(sm_callbacks_.get()),
              OnStateMachineEvent(instance_id,
                                  BroadcastStateMachine::State::CONFIGURING, _))
      .Times(1);
  EXPECT_CALL(*(sm_callbacks_.get()),
              OnStateMachineEvent(instance_id,
                                  BroadcastStateMachine::State::STREAMING, _))
      .Times(1);
  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::START);

  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STREAMING);
}

TEST_F(StateMachineTest, ProcessMessageStopWhenStopped) {
  uint8_t instance_id =
      InstantiateStateMachine(LeAudioBroadcaster::AudioProfile::MEDIA);

  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::STOP);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STOPPED);

  EXPECT_CALL(*mock_iso_manager_, SetupIsoDataPath).Times(0);
  EXPECT_CALL(*mock_iso_manager_, RemoveIsoDataPath).Times(0);
  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineEvent(instance_id, _, _))
      .Times(0);
  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::STOP);

  // There shall be no change in state
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STOPPED);
}

TEST_F(StateMachineTest, ProcessMessageSuspendWhenStopped) {
  uint8_t instance_id =
      InstantiateStateMachine(LeAudioBroadcaster::AudioProfile::MEDIA);

  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::STOP);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STOPPED);

  EXPECT_CALL(*mock_iso_manager_, SetupIsoDataPath).Times(0);
  EXPECT_CALL(*mock_iso_manager_, RemoveIsoDataPath).Times(0);
  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineEvent(instance_id, _, _))
      .Times(0);
  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::SUSPEND);

  // There shall be no change in state
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STOPPED);
}

TEST_F(StateMachineTest, OnSetupIsoDataPathError) {
  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineCreateStatus(_, true))
      .Times(1);

  uint8_t instance_id =
      InstantiateStateMachine(LeAudioBroadcaster::AudioProfile::MEDIA);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::CONFIGURED);

  EXPECT_CALL(*mock_iso_manager_, SetupIsoDataPath)
      .WillOnce([this](uint16_t conn_handle, iso_data_path_params p) {
        // Get the big_id encoded in conn_handle's MSB
        if (!broadcasts_[conn_handle >> 8]) return;
        broadcasts_[conn_handle >> 8]->OnSetupIsoDataPath(0, conn_handle);
      })
      .WillOnce([this](uint16_t conn_handle, iso_data_path_params p) {
        // Get the big_id encoded in conn_handle's MSB
        if (!broadcasts_[conn_handle >> 8]) return;
        broadcasts_[conn_handle >> 8]->OnSetupIsoDataPath(1, conn_handle);
      })
      .RetiresOnSaturation();
  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::START);

  // On datapath setup failure we should go back to configured with BIG being
  // destroyed. Maybe it will work out next time for the new BIG.
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::CONFIGURED);

  // And still be able to start again
  ON_CALL(*mock_iso_manager_, SetupIsoDataPath)
      .WillByDefault([this](uint16_t conn_handle, iso_data_path_params p) {
        // Get the big_id encoded in conn_handle's MSB
        if (!broadcasts_[conn_handle >> 8]) return;
        broadcasts_[conn_handle >> 8]->OnSetupIsoDataPath(0, conn_handle);
      });
  EXPECT_CALL(*mock_iso_manager_, SetupIsoDataPath).Times(2);

  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::START);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STREAMING);
}

TEST_F(StateMachineTest, OnRemoveIsoDataPathError) {
  uint8_t instance_id =
      InstantiateStateMachine(LeAudioBroadcaster::AudioProfile::MEDIA);

  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::START);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STREAMING);

  EXPECT_CALL(*mock_iso_manager_, RemoveIsoDataPath)
      .WillOnce([this](uint16_t conn_handle, uint8_t iso_direction) {
        // Get the big_id encoded in conn_handle's MSB
        if (!broadcasts_[conn_handle >> 8]) return;
        broadcasts_[conn_handle >> 8]->OnRemoveIsoDataPath(0, conn_handle);
      })
      .WillOnce([this](uint16_t conn_handle, uint8_t iso_direction) {
        // Get the big_id encoded in conn_handle's MSB
        if (!broadcasts_[conn_handle >> 8]) return;
        broadcasts_[conn_handle >> 8]->OnRemoveIsoDataPath(1, conn_handle);
      })
      .RetiresOnSaturation();
  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::SUSPEND);

  // On datapath teardown failure we should stay in CONFIGURED with BIG being
  // destroyed.
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::CONFIGURED);

  // And still be able to start again
  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::START);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STREAMING);
}

TEST_F(StateMachineTest, GetConfig) {
  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineCreateStatus(_, true))
      .Times(1);

  auto sound_profile = LeAudioBroadcaster::AudioProfile::MEDIA;
  uint8_t num_channels = 2;

  uint8_t instance_id = InstantiateStateMachine(sound_profile);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::CONFIGURED);

  std::optional<BigConfig> const& big_cfg =
      broadcasts_[instance_id]->GetBigConfig();
  ASSERT_FALSE(big_cfg.has_value());

  broadcasts_[instance_id]->ProcessMessage(
      BroadcastStateMachine::Message::START);
  ASSERT_EQ(broadcasts_[instance_id]->GetState(),
            BroadcastStateMachine::State::STREAMING);

  ASSERT_TRUE(big_cfg.has_value());
  ASSERT_EQ(big_cfg->status, 0);
  ASSERT_EQ(big_cfg->big_id, instance_id);
  ASSERT_EQ(big_cfg->connection_handles.size(), num_channels);
}

TEST_F(StateMachineTest, AnnouncementUUIDs) {
  std::vector<uint8_t> a_data;
  std::vector<uint8_t> p_data;

  EXPECT_CALL(*mock_ble_advertising_manager_, StartAdvertisingSet)
      .WillOnce([&p_data, &a_data](
                    base::Callback<void(uint8_t, int8_t, uint8_t)> cb,
                    tBTM_BLE_ADV_PARAMS* params,
                    std::vector<uint8_t> advertise_data,
                    std::vector<uint8_t> scan_response_data,
                    tBLE_PERIODIC_ADV_PARAMS* periodic_params,
                    std::vector<uint8_t> periodic_data, uint16_t duration,
                    uint8_t maxExtAdvEvents,
                    base::Callback<void(uint8_t, uint8_t)> timeout_cb) {
        uint8_t advertiser_id = 1;
        uint8_t tx_power = 0;
        uint8_t status = 0;

        // Since we are not using these buffers in this callback it is safe to
        // move them.
        a_data = std::move(advertise_data);
        p_data = std::move(periodic_data);

        cb.Run(advertiser_id, tx_power, status);
      });

  EXPECT_CALL(*(sm_callbacks_.get()), OnStateMachineCreateStatus(_, true))
      .Times(1);

  uint8_t instance_id = InstantiateStateMachine();
  ASSERT_NE(instance_id, BroadcastStateMachine::kInstanceIdUndefined);

  // Check ext. advertising data for Broadcast Announcement UUID
  ASSERT_NE(a_data[0], 0);     // size
  ASSERT_EQ(a_data[1], 0x16);  // BTM_BLE_AD_TYPE_SERVICE_DATA_TYPE
  ASSERT_EQ(a_data[2], (kBroadcastAudioAnnouncementServiceUuid & 0x00FF));
  ASSERT_EQ(a_data[3],
            ((kBroadcastAudioAnnouncementServiceUuid >> 8) & 0x00FF));

  // Check periodic data for Basic Announcement UUID
  ASSERT_NE(p_data[0], 0);     // size
  ASSERT_EQ(p_data[1], 0x16);  // BTM_BLE_AD_TYPE_SERVICE_DATA_TYPE
  ASSERT_EQ(p_data[2], (kBasicAudioAnnouncementServiceUuid & 0x00FF));
  ASSERT_EQ(p_data[3], ((kBasicAudioAnnouncementServiceUuid >> 8) & 0x00FF));
}

}  // namespace
}  // namespace broadcaster
}  // namespace le_audio
