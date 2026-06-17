#pragma once

#include "config.h"

namespace meshcli {

class MeshService;

// Top-level app wiring: parse args, init logging + DB + mesh service, launch
// the TUI (or scan-only mode). Returns an exit code.
int run_app(int argc, char** argv, MeshService& service);

} // namespace meshcli
