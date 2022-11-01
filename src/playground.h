#pragma once

#include <noo_server_interface.h>

#include <memory>

struct Model;

class ModelCallbacks : public noo::EntityCallbacks {

    std::weak_ptr<Model> m_model;

    void update_transform();

public:
    ModelCallbacks(noo::ObjectT*, std::shared_ptr<Model>);

    void set_position(glm::vec3) override;
    void set_rotation(glm::quat) override;
    void set_scale(glm::vec3) override;
};

struct Model {
    int id;

    glm::vec3 position = glm::vec3(0);
    glm::quat rotation = glm::quat();
    glm::vec3 scale    = glm::vec3(1);

    glm::vec3 min_bb = glm::vec3(std::numeric_limits<float>::max());
    glm::vec3 max_bb = glm::vec3(std::numeric_limits<float>::lowest());

    glm::mat4 recompute_transform();

    noo::ObjectTPtr object;

    std::vector<noo::ObjectTPtr> other_objects;
};

using ModelPtr = std::shared_ptr<Model>;


class Playground : public QObject {
    Q_OBJECT

    // Noodles stuff
    noo::ServerTPtr m_server;

    noo::DocumentTPtr m_doc;

    std::vector<noo::ObjectTPtr> m_lights;

    noo::ObjectTPtr m_collective_root;

    int                                m_id_counter = 0;
    QHash<int, std::shared_ptr<Model>> m_thing_list;

    void add_model(QString);

    void update_root_tf();

public:
    Playground();

    ~Playground();

    std::shared_ptr<noo::DocumentT> document();

    noo::ObjectTPtr plot_root();
};
