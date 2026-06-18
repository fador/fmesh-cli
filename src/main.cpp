#include "app/app.h"
#include "mesh/mesh_service.h"

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    meshcli::MeshService service;
    return meshcli::run_app(argc, argv, service);
}
