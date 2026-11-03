/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include <cassert>
#include <optional>
#include <string>

#include "recovery_stage.h"

int main() {
  using recovery::ParseRecoveryStage;
  using recovery::StageValues;

  // Recovery may be started without a BCB stage. This must never throw.
  const std::optional<std::string> missing;
  assert(!ParseRecoveryStage(missing).has_value());

  // Preserve valid AOSP recovery stage syntax.
  const auto normal = ParseRecoveryStage(std::optional<std::string>("2/5"));
  assert(normal.has_value());
  assert(normal->current == 2 && normal->maximum == 5);

  // Omitted and malformed stage values must not advance the progress UI.
  assert(!ParseRecoveryStage(std::optional<std::string>("")).has_value());
  assert(!ParseRecoveryStage(std::optional<std::string>("pending")).has_value());
  assert(!ParseRecoveryStage(std::optional<std::string>("2/")).has_value());
  assert(!ParseRecoveryStage(std::optional<std::string>("/5")).has_value());

  // Match AOSP's old scanf parsing for numeric stages.
  const auto completed = ParseRecoveryStage(std::optional<std::string>("5/5"));
  assert(completed.has_value() && completed->current == 5 && completed->maximum == 5);
  return 0;
}
