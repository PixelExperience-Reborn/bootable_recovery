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
#pragma once

#include <cstdio>
#include <optional>
#include <string>

namespace recovery {

struct StageValues {
  int current;
  int maximum;
};

// A stage is optional for normal boots into recovery. Only dereference it
// when present, while preserving AOSP's established "current/maximum" format.
inline std::optional<StageValues> ParseRecoveryStage(const std::optional<std::string>& stage) {
  if (!stage.has_value()) {
    return std::nullopt;
  }

  StageValues parsed{0, 0};
  if (std::sscanf(stage->c_str(), "%d/%d", &parsed.current, &parsed.maximum) != 2) {
    return std::nullopt;
  }
  return parsed;
}

}  // namespace recovery
