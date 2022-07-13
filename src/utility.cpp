#include "utility.h"

std::pair<glm::vec3, glm::vec3> min_max_of(std::span<glm::vec3 const> v) {

    if (v.empty()) return { {}, {} };

    glm::vec3 lmin = v[0];
    glm::vec3 lmax = v[0];

    for (auto const& lv : v) {
        lmin = glm::min(lv, lmin);
        lmax = glm::max(lv, lmax);
    }

    return { lmin, lmax };
}


std::pair<glm::vec3, glm::vec3> min_max_of(std::span<double const> x,
                                           std::span<double const> y,
                                           std::span<double const> z) {
    if (x.empty() or y.empty() or z.empty()) return { {}, {} };

    glm::vec3 lmin(x[0], y[0], z[0]);
    glm::vec3 lmax(x[0], y[0], z[0]);

    for (size_t i = 0; i < x.size(); i++) {
        lmin = glm::min(glm::vec3 { x[i], y[i], z[i] }, lmin);
        lmax = glm::max(glm::vec3 { x[i], y[i], z[i] }, lmax);
    }

    return { lmin, lmax };
}

void update_instances(std::span<glm::mat4 const> instances,
                      noo::DocumentTPtr          doc,
                      noo::ObjectTPtr            object,
                      noo::MeshTPtr              mesh) {
    auto src_bytes = (const char*)instances.data();

    QByteArray array(src_bytes, instances.size_bytes());

    auto new_buffer = noo::create_buffer(
        doc,
        noo::BufferData {
            .source = noo::BufferInlineSource { .data = array },
        });

    auto view = noo::create_buffer_view(doc,
                                        noo::BufferViewData {
                                            .source_buffer = new_buffer,
                                            .type   = noo::ViewType::UNKNOWN,
                                            .offset = 0,
                                            .length = (uint64_t)array.size(),
                                        });

    noo::ObjectUpdateData update { .definition =
                                       noo::ObjectRenderableDefinition {
                                           .mesh      = mesh,
                                           .instances = noo::InstanceInfo {
                                               .view   = view,
                                               .stride = 0,
                                           } } };

    noo::update_object(object, update);
}
