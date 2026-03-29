#pragma once

#include <stddef.h>
#include <stdint.h>

namespace robot {

class AudioRecorder {
 public:
  bool begin();
  bool record(int16_t *buffer, size_t sampleCount) const;
};

}  // namespace robot
