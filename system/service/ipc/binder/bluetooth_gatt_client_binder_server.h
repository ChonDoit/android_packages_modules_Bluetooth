//
//  Copyright (C) 2015 Google, Inc.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at:
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//

#pragma once

#include <base/macros.h>

#include "service/common/bluetooth/binder/IBluetoothGattClient.h"
#include "service/common/bluetooth/binder/IBluetoothGattClientCallback.h"

namespace bluetooth {
class Adapter;
}  // namespace bluetooth

namespace ipc {
namespace binder {

// Implements the server side of the IBluetoothGattClient interface.
class BluetoothGattClientBinderServer : public BnBluetoothGattClient {
 public:
  explicit BluetoothGattClientBinderServer(bluetooth::Adapter* adapter);
  ~BluetoothGattClientBinderServer() override = default;

  // IBluetoothGattClient overrides:
  bool RegisterClient(
      const android::sp<IBluetoothGattClientCallback>& callback) override;
  void UnregisterClient(int client_id) override;
  void UnregisterAll() override;

 private:
  bluetooth::Adapter* adapter_;  // weak

  DISALLOW_COPY_AND_ASSIGN(BluetoothGattClientBinderServer);
};

}  // namespace binder
}  // namespace ipc
