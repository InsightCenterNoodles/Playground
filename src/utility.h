#pragma once

#include "noo_include_glm.h"

#include <noo_server_interface.h>

#include <span>

std::pair<glm::vec3, glm::vec3> min_max_of(std::span<glm::vec3 const>);

std::pair<glm::vec3, glm::vec3> min_max_of(std::span<double const> x,
                                           std::span<double const> y,
                                           std::span<double const> z);

void update_instances(std::span<glm::mat4 const> instances,
                      noo::DocumentTPtr          doc,
                      noo::ObjectTPtr            object,
                      noo::MeshTPtr              mesh);
