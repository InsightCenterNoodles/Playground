#include "playground.h"

#include "xdmfimporter.h"

#include "variant_tools.h"

#include <glm/gtx/quaternion.hpp>

#include <string>
#include <string_view>

#include <QColor>
#include <QCommandLineParser>
#include <QDebug>
#include <QFileInfo>
#include <QVariantAnimation>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

std::span<glm::vec3> convert_vec3(aiVector3D* src, size_t count) {
    return { reinterpret_cast<glm::vec3*>(src), count };
}

glm::u8vec4 convert_col(aiColor4D const& src) {
    return (glm::vec4(src[0], src[1], src[2], src[3]) * 255.0f);
}

glm::u16vec2 convert_tex(aiVector3D const& src) {
    return (glm::vec2(src[0], src[1]) * 65535.0f);
}

QColor convert_qcol(aiColor4D const& src) {
    return QColor::fromRgbF(src.r, src.g, src.b, src.a);
}

QDebug operator<<(QDebug debug, glm::vec4 const& c) {
    QDebugStateSaver saver(debug);
    debug.nospace() << '<' << c.x << ", " << c.y << ", " << c.z << ", " << c.w
                    << '>';

    return debug;
}

QDebug operator<<(QDebug debug, glm::mat4 const& c) {
    QDebugStateSaver saver(debug);
    debug.nospace() << "[\n " << c[0] << "\n " << c[1] << "\n " << c[2] << "\n "
                    << c[3] << "\n]";

    return debug;
}

// =============================================================================

static auto normal_callbacks = noo::EntityCallbacks::EnableCallback {
    .transform_position = true,
    .transform_rotation = true,
    .transform_scale    = true,
};

void ModelCallbacks::update_transform() {
    auto l = m_model.lock();
    if (!l) return;

    noo::ObjectUpdateData update;
    update.transform = l->recompute_transform();

    noo::update_object(get_host(), update);
}

ModelCallbacks::ModelCallbacks(noo::ObjectT* t, std::shared_ptr<Model> s)
    : noo::EntityCallbacks(t, normal_callbacks), m_model(s) { }

void ModelCallbacks::set_position(glm::vec3 p) {
    if (auto sp = m_model.lock()) {
        sp->position = p;
        update_transform();
    }
}
void ModelCallbacks::set_rotation(glm::quat q) {
    if (auto sp = m_model.lock()) {
        sp->rotation = q;
        update_transform();
    }
}
void ModelCallbacks::set_scale(glm::vec3 s) {
    if (auto sp = m_model.lock()) {
        sp->scale = s;
        update_transform();
    }
}

// =============================================================================

glm::mat4 Model::recompute_transform() {
    glm::mat4 ret(1);
    ret = glm::translate(ret, position);
    ret = ret * glm::mat4_cast(rotation);
    ret = glm::scale(ret, scale);
    return ret;
}


// =============================================================================

#define GET_MATKEY(MAT, KEY, TYPE)                                             \
    ({                                                                         \
        std::optional<TYPE> ret;                                               \
        ret.emplace();                                                         \
        if (AI_SUCCESS != MAT.Get(KEY, *ret)) { ret.reset(); }                 \
        ret;                                                                   \
    })


struct Importer {
    aiScene const&         scene;
    noo::DocumentTPtrRef   doc;
    noo::ObjectTPtr        root;
    std::shared_ptr<Model> model_ref;
    Model&                 thing;

    std::unordered_map<unsigned, noo::MeshTPtr> converted_meshes;

    noo::MaterialTPtr import_material(aiMaterial const& m) {
        qDebug() << "Adding new material";

        noo::MaterialData mdata;

        {
            auto base_color = GET_MATKEY(m, AI_MATKEY_BASE_COLOR, aiColor4D);
            if (!base_color) {
                base_color = GET_MATKEY(m, AI_MATKEY_COLOR_DIFFUSE, aiColor4D);
            }
            if (!base_color) { base_color = aiColor4D(1, 1, 1, 1); }

            mdata.pbr_info.base_color = convert_qcol(base_color.value());
        }

        {
            auto metallic = GET_MATKEY(m, AI_MATKEY_METALLIC_FACTOR, float);
            if (!metallic) {
                metallic = GET_MATKEY(m, AI_MATKEY_SPECULAR_FACTOR, float);
            }

            mdata.pbr_info.metallic = metallic.value_or(1);
        }

        {
            auto roughness = GET_MATKEY(m, AI_MATKEY_ROUGHNESS_FACTOR, float);
            if (!roughness) {
                roughness = GET_MATKEY(m, AI_MATKEY_GLOSSINESS_FACTOR, float);
            }

            mdata.pbr_info.roughness = roughness.value_or(1);
        }

        mdata.double_sided = GET_MATKEY(m, AI_MATKEY_TWOSIDED, bool);

        return noo::create_material(doc, mdata);
    }


    noo::MeshTPtr import_mesh(aiMesh const& mesh) {
        qDebug() << "Adding new mesh from scene...";

        qDebug() << "Num Verts" << mesh.mNumVertices;

        static_assert(sizeof(glm::vec3) == sizeof(aiVector3D));

        qDebug() << "Adding positions";

        noo::MeshSource source;
        source.positions = convert_vec3(mesh.mVertices, mesh.mNumVertices);

        for (auto const& v : source.positions) {
            thing.min_bb = glm::min(thing.min_bb, v);
            thing.max_bb = glm::max(thing.max_bb, v);
        }

        qDebug() << "Model BB Min" << thing.min_bb.x << thing.min_bb.y
                 << thing.min_bb.z;
        qDebug() << "Model BB Max" << thing.max_bb.x << thing.max_bb.y
                 << thing.max_bb.z;

        if (mesh.mNormals) {
            qDebug() << "Adding normals";
            source.normals = convert_vec3(mesh.mNormals, mesh.mNumVertices);
        }

        if (mesh.mTangents) {
            qDebug() << "Adding tangents";
            source.normals = convert_vec3(mesh.mTangents, mesh.mNumVertices);
        }

        std::vector<glm::u8vec4> converted_colors;

        if (mesh.mColors[0]) {
            qDebug() << "Adding colors[0]";
            auto channel = mesh.mColors[0];

            converted_colors.reserve(mesh.mNumVertices);

            for (size_t i = 0; i < mesh.mNumVertices; i++) {
                converted_colors.push_back(convert_col(channel[i]));
            }

            source.colors = converted_colors;
        }

        std::vector<glm::u16vec2> converted_textures;

        if (mesh.HasTextureCoords(0)) {
            qDebug() << "Adding uv[0]";
            auto channel = mesh.mTextureCoords[0];

            converted_textures.reserve(mesh.mNumVertices);

            for (size_t i = 0; i < mesh.mNumVertices; i++) {
                converted_textures.push_back(convert_tex(channel[i]));
            }

            source.textures = converted_textures;
        }

        std::vector<uint32_t> indicies;

        if (mesh.mPrimitiveTypes & aiPrimitiveType::aiPrimitiveType_LINE) {
            qDebug() << "Adding LINE" << mesh.mNumFaces;
            for (size_t i = 0; i < mesh.mNumFaces; i++) {
                auto const& face = mesh.mFaces[i];
                assert(face.mNumIndices >= 2);
                indicies.emplace_back(face.mIndices[0]);
                indicies.emplace_back(face.mIndices[1]);
            }
            source.type = noo::MeshSource::LINE;

        } else if (mesh.mPrimitiveTypes &
                   aiPrimitiveType::aiPrimitiveType_TRIANGLE) {
            qDebug() << "Adding TRIANGLES" << mesh.mNumFaces;

            for (size_t i = 0; i < mesh.mNumFaces; i++) {
                auto const& face = mesh.mFaces[i];
                assert(face.mNumIndices >= 3);
                indicies.emplace_back(face.mIndices[0]);
                indicies.emplace_back(face.mIndices[1]);
                indicies.emplace_back(face.mIndices[2]);
            }
            source.type = noo::MeshSource::TRIANGLE;
        }

        source.index_format = noo::Format::U32;

        source.indices = std::as_writable_bytes(std::span(indicies));

        auto const& material = *scene.mMaterials[mesh.mMaterialIndex];

        source.material = import_material(material);

        return noo::create_mesh(doc, source);
    }


    void process_import_tree(aiNode const& node, noo::ObjectTPtr parent) {
        qDebug() << "Handling new node...";

        noo::ObjectData new_obj_data;

        if (node.mName.length) new_obj_data.name = node.mName.C_Str();

        if (parent) new_obj_data.parent = parent;

        glm::mat4& transform = new_obj_data.transform.emplace();

        for (int i = 0; i < (4 * 4); i++) {
            // from Row major to column major
            glm::value_ptr(transform)[i] = (node.mTransformation[0])[i];
        }

        qDebug() << "Transformation:" << transform;

        // if this is the first object, we add some callbacks.
        if (!thing.object) {
            new_obj_data.create_callbacks = [=](noo::ObjectT* t) {
                return std::make_unique<ModelCallbacks>(t, model_ref);
            };
        }

        auto this_node = noo::create_object(doc, new_obj_data);

        if (thing.object) {
            thing.other_objects.push_back(this_node);
        } else {
            thing.object = this_node;
        }


        if (node.mNumMeshes) {
            qDebug() << "Adding sub-meshes:" << node.mNumMeshes;

            // create bits. we could pack this into patches...
            // but for now, just create multiple objects

            for (unsigned mi = 0; mi < node.mNumMeshes; mi++) {
                noo::ObjectData sub_obj_data;

                auto src_mesh_id = node.mMeshes[mi];

                auto iter = converted_meshes.find(src_mesh_id);

                if (iter == converted_meshes.end()) {

                    auto new_mesh = import_mesh(*scene.mMeshes[src_mesh_id]);

                    bool ok;
                    std::tie(iter, ok) =
                        converted_meshes.try_emplace(src_mesh_id, new_mesh);
                }

                sub_obj_data.definition =
                    noo::ObjectRenderableDefinition { .mesh = iter->second };

                sub_obj_data.parent = this_node;

                sub_obj_data.tags = QStringList()
                                    << noo::names::tag_user_hidden;

                auto sub_obj = noo::create_object(doc, sub_obj_data);

                thing.other_objects.push_back(sub_obj);
            }
        }

        for (unsigned ci = 0; ci < node.mNumChildren; ci++) {
            process_import_tree(*node.mChildren[ci], this_node);
        }
    }
};


std::variant<ModelPtr, QString> import_ai_scene(aiScene const&       scene,
                                                noo::DocumentTPtrRef doc,
                                                noo::ObjectTPtr collective_root,
                                                int             id) {
    auto new_model = std::make_shared<Model>();
    new_model->id  = id;

    Importer imp {
        .scene     = scene,
        .doc       = doc,
        .root      = collective_root,
        .model_ref = new_model,
        .thing     = *new_model,
    };

    imp.process_import_tree(*(scene.mRootNode), collective_root);

    return new_model;
}


std::variant<ModelPtr, QString> make_thing(int                  id,
                                           QString              path,
                                           noo::DocumentTPtrRef doc,
                                           noo::ObjectTPtr collective_root) {

    QFileInfo info(path);

    if (!info.exists(path)) return "File does not exist.";

    Assimp::Importer importer;

    importer.RegisterLoader(new XDMFAssimpImporter);

    auto path_str = path.toStdString();

    auto* scene = importer.ReadFile(
        path_str,
        // aiProcess_CalcTangentSpace |
        aiProcess_Triangulate | aiProcess_GenNormals |
            aiProcess_FixInfacingNormals | aiProcess_JoinIdenticalVertices |
            aiProcess_SortByPType);

    if (!scene) {
        return QString("Unable to import file: ") + importer.GetErrorString();
    }


    return import_ai_scene(*scene, doc, collective_root, id);
}

// =============================================================================

void Playground::add_model(QString path) {
    qInfo() << "Loading" << path;
    auto result = make_thing(m_id_counter, path, m_doc, m_collective_root);

    auto err = std::get_if<QString>(&result);

    if (err) {
        qWarning() << "Unable to import" << path << " | reason:" << *err;
        return;
    }

    auto ptr = std::get<std::shared_ptr<Model>>(result);

    if (!ptr) {
        qWarning() << "Unable to import, skipping";
        return;
    }

    m_thing_list[m_id_counter] = ptr;

    m_id_counter++;

    qInfo() << "Done adding model.";
}

void Playground::update_root_tf() {
    // lets set up a simple scale

    glm::vec3 total_min_bb = glm::vec3(std::numeric_limits<float>::max());
    glm::vec3 total_max_bb = glm::vec3(std::numeric_limits<float>::lowest());

    for (auto const& v : qAsConst(m_thing_list)) {
        total_min_bb = glm::min(v->min_bb, total_min_bb);
        total_max_bb = glm::max(v->max_bb, total_max_bb);
    }

    if (glm::any(glm::greaterThan(total_min_bb, total_max_bb))) { return; }

    qDebug() << "Total BB Min" << total_min_bb.x << total_min_bb.y
             << total_min_bb.z;
    qDebug() << "Total BB Max" << total_max_bb.x << total_max_bb.y
             << total_max_bb.z;

    // translate to center

    auto delta    = total_max_bb - total_min_bb;
    auto max_comp = glm::compMax(delta);

    auto center = (delta / 2.0f) + total_min_bb;

    qDebug() << "Bounds" << delta.x << delta.y << delta.z;

    glm::mat4 tf(1);

    tf = glm::scale(tf, glm::vec3(1.0f / max_comp));
    tf = glm::translate(tf, -center);

    noo::ObjectUpdateData ob {
        .transform = tf,
    };

    noo::update_object(m_collective_root, ob);
}

Playground::Playground() {

    QCommandLineParser parser;
    parser.setApplicationDescription("Geometry export tool for NOODLES");
    parser.addHelpOption();
    parser.addVersionOption();

    parser.addPositionalArgument("files", "Geometry files to import");

    m_server = noo::create_server(parser);

    auto args = parser.positionalArguments();

    Q_ASSERT(m_server);

    m_doc = noo::get_document(m_server.get());

    Q_ASSERT(m_doc);

    noo::DocumentData docup;

    //    {
    //        QVector<noo::MethodTPtr> methods;

    //        auto ptr = make_new_point_plot_method(*this);
    //        methods.push_back(ptr);

    //        docup.method_list = methods;
    //    }

    noo::update_document(m_doc, docup);

    auto add_light = [this](glm::vec3 p, QColor color, float i) {
        noo::LightData light_data;
        light_data.color     = color;
        light_data.intensity = i;
        light_data.type      = noo::DirectionLight {};

        auto light = noo::create_light(m_doc, light_data);

        noo::ObjectData nd;

        nd.transform = glm::lookAt(p, glm::vec3 { 0 }, glm::vec3 { 0, 1, 0 });

        nd.lights.emplace().push_back(light);

        nd.tags.emplace().push_back(noo::names::tag_user_hidden);

        m_lights.emplace_back(noo::create_object(m_doc, nd));
    };

    add_light({ 1, 1, 1 }, Qt::white, 4);

    add_light({ 1, 0, 0 }, Qt::white, 4);

    auto start_time = std::chrono::high_resolution_clock::now();

    {
        noo::ObjectData obdata = {
            .name = "Scene Root",
            .tags = QStringList() << noo::names::tag_user_hidden,
        };

        m_collective_root = noo::create_object(m_doc, obdata);
    }

    for (auto const& fname : args) {
        add_model(fname);
    }

    auto end_time = std::chrono::high_resolution_clock::now();

    qInfo() << "Done loading models:"
            << std::chrono::duration<double>(end_time - start_time).count()
            << "seconds";

    update_root_tf();
}

Playground::~Playground() { }
