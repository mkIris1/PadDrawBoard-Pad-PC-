#include "pdb/video/backpressure_policy.h"

namespace pdb::video {

TransportAction EncodedBackpressurePolicy::OnTransportWouldBlock() {
  if (reset_pending_) return TransportAction::kHold;
  reset_pending_ = true;
  ++reset_requests_;
  return TransportAction::kRequestStreamReset;
}

}  // namespace pdb::video
