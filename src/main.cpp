#include "app/app.h"
#include "mesh/mesh_service.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#else
    // Writes to closed sockets must not terminate the process; rely on the
    // EPIPE errno instead (e.g. SSL_shutdown after a peer disconnect).
    std::signal(SIGPIPE, SIG_IGN);
#endif

    meshcli::MeshService service;
    return meshcli::run_app(argc, argv, service);
}
