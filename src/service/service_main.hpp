#pragma once

namespace gtg::service {

// Enters the Service Control Manager dispatcher. This must only be called for
// the --service command line mode.
int Run();

}  // namespace gtg::service
