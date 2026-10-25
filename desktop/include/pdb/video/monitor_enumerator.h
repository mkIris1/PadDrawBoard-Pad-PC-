#pragma once

#include "pdb/video/types.h"

#include <vector>

namespace pdb::video {

class MonitorEnumerator final {
 public:
  [[nodiscard]] static HRESULT Enumerate(std::vector<MonitorInfo>* monitors);
  [[nodiscard]] static HRESULT Find(const MonitorId& id, MonitorInfo* monitor);
};

}  // namespace pdb::video
