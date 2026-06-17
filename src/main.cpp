#include "app/app.h"
#include "mesh/mesh_service.h"

int main(int argc, char** argv) {
    meshcli::MeshService service;
    return meshcli::run_app(argc, argv, service);
}
