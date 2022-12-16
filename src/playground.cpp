#include "playground.h"

#include "xdmfimporter.h"

#include "variant_tools.h"

#include <glm/gtx/quaternion.hpp>

#include <string>
#include <string_view>

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QCommandLineParser>
#include <QDebug>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
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
    qDebug() << Q_FUNC_INFO << p.x << p.y << p.z;
    if (auto sp = m_model.lock()) {
        sp->position = p;
        update_transform();
    }
}
void ModelCallbacks::set_rotation(glm::quat q) {
    qDebug() << Q_FUNC_INFO << q.x << q.y << q.z << q.w;
    if (auto sp = m_model.lock()) {
        sp->rotation = q;
        update_transform();
    }
}
void ModelCallbacks::set_scale(glm::vec3 s) {
    qDebug() << Q_FUNC_INFO << s.x << s.y << s.z;
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

    qCritical() << ret[0].x << ret[0].y << ret[0].z << ret[0].w;
    qCritical() << ret[1].x << ret[1].y << ret[1].z << ret[1].w;
    qCritical() << ret[2].x << ret[2].y << ret[2].z << ret[2].w;
    qCritical() << ret[3].x << ret[3].y << ret[3].z << ret[3].w;

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
    ImportOptions          options;

    std::unordered_map<unsigned, noo::MeshTPtr> converted_meshes;
    QHash<QString, noo::TextureTPtr>            converted_textures;

    noo::TextureTPtr find_texture_type(aiMaterial const&          m,
                                       std::vector<aiTextureType> types) {
        for (auto type : types) {
            if (m.GetTextureCount(type) < 1) continue;

            aiString path;
            m.GetTexture(type, 0, &path);

            qDebug() << "Texture path at" << path.C_Str();

            return import_texture(QString::fromUtf8(path.C_Str(), path.length));
        }

        return {};
    }

    noo::TextureTPtr import_texture(aiTexture const& tex) {
        qDebug() << "TEX" << tex.achFormatHint << tex.mWidth << tex.mHeight
                 << tex.mFilename.C_Str();

        if (tex.mHeight == 0) {
            qDebug() << "Texture is compressed";

            return import_texture(QByteArray((char*)tex.pcData, tex.mWidth),
                                  "");
        }

        qCritical() << "Image conversion is not yet supported";

        return nullptr;
    }

    noo::TextureTPtr import_texture(QString path) {
        if (converted_textures.contains(path)) return converted_textures[path];

        qDebug() << "Loading texture from path:" << path;

        if (path.startsWith("*")) {
            qDebug() << "Appears to be path to builtin";
            bool ok;
            int  index = path.mid(1).toInt(&ok);
            if (!ok or index >= scene.mNumTextures) {
                qDebug() << "Apparently not. Bailing.";
                return {};
            }

            auto ret                 = import_texture(*scene.mTextures[index]);
            converted_textures[path] = ret;
            return ret;
        }

        qDebug() << "Path is external, loading";

        QMimeDatabase db;
        auto          type = db.mimeTypeForFile(path);

        if (type.inherits("image/png") or type.inherits("image/jpeg")) {
            // just use as is
            QFile file(path);
            file.open(QFile::ReadOnly);

            auto ret                 = import_texture(file.readAll(), path);
            converted_textures[path] = ret;
            return ret;
        }

        QByteArray bytes;
        {
            QBuffer      out_stream(&bytes);
            QImageWriter writer(&out_stream, "png");
            QImage       img(path);
            writer.write(img);
        }

        auto ret                 = import_texture(bytes, path);
        converted_textures[path] = ret;
        return ret;
    }

    noo::TextureTPtr import_texture(QByteArray const& array, QString name) {
        qDebug() << "Loading raw texture" << array.size() << "bytes";

        auto new_buffer = noo::create_buffer(
            doc,
            noo::BufferData { .name   = "Buffer for" + name,
                              .source = noo::BufferInlineSource {
                                  .data = array,
                              } });

        auto new_buffer_view =
            noo::create_buffer_view(doc,
                                    noo::BufferViewData {
                                        .source_buffer = new_buffer,
                                        .type   = noo::ViewType::IMAGE_INFO,
                                        .offset = 0,
                                        .length = (uint64_t)array.length(),
                                    });

        auto new_image = noo::create_image(doc,
                                           noo::ImageData {
                                               .name   = name,
                                               .source = new_buffer_view,
                                           });

        auto tex_data = noo::TextureData { .name = name, .image = new_image };

        if (options.force_samplers_to_nearest) {
            qDebug() << "Adding sampler hack";
            noo::SamplerData sampler_data {
                .mag_filter = noo::MagFilter::NEAREST,
                .min_filter = noo::MinFilter::NEAREST,
                .wrap_s     = noo::SamplerMode::CLAMP_TO_EDGE,
                .wrap_t     = noo::SamplerMode::CLAMP_TO_EDGE,
            };

            tex_data.sampler = noo::create_sampler(doc, sampler_data);
        }

        auto new_texture = noo::create_texture(doc, tex_data);

        return new_texture;
    }

    noo::MaterialTPtr import_material(aiMaterial const& m) {
        qDebug() << "Adding new material";

        noo::MaterialData mdata;

        auto& pbr = mdata.pbr_info.emplace();

        {
            auto base_color = GET_MATKEY(m, AI_MATKEY_BASE_COLOR, aiColor4D);
            if (!base_color) {
                base_color = GET_MATKEY(m, AI_MATKEY_COLOR_DIFFUSE, aiColor4D);
            }
            if (!base_color) { base_color = aiColor4D(1, 1, 1, 1); }

            pbr.base_color = convert_qcol(base_color.value());
        }

        {
            auto metallic = GET_MATKEY(m, AI_MATKEY_METALLIC_FACTOR, float);
            if (!metallic) {
                metallic = GET_MATKEY(m, AI_MATKEY_SPECULAR_FACTOR, float);
            }

            pbr.metallic = metallic.value_or(1);
        }

        {
            auto roughness = GET_MATKEY(m, AI_MATKEY_ROUGHNESS_FACTOR, float);
            if (!roughness) {
                roughness = GET_MATKEY(m, AI_MATKEY_GLOSSINESS_FACTOR, float);
            }

            pbr.roughness = roughness.value_or(1);
        }

        mdata.double_sided = GET_MATKEY(m, AI_MATKEY_TWOSIDED, bool);

        if (options.double_sided) { mdata.double_sided = true; }

        {
            auto base = find_texture_type(
                m, { aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE });

            if (base) {
                mdata.pbr_info->base_color_texture.emplace(noo::TextureRef {
                    .source             = base,
                    .transform          = glm::mat3(1),
                    .texture_coord_slot = 0,
                });
            }
        }

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
                                                int             id,
                                                ImportOptions const& options) {
    auto new_model = std::make_shared<Model>();
    new_model->id  = id;

    Importer imp {
        .scene     = scene,
        .doc       = doc,
        .root      = collective_root,
        .model_ref = new_model,
        .thing     = *new_model,
        .options   = options,
    };

    imp.process_import_tree(*(scene.mRootNode), collective_root);

    return new_model;
}

bool needs_gltf_sampler_hack(QString path) {
    auto check_json = [](QByteArray array) {
        auto doc = QJsonDocument::fromJson(array).object();

        auto samplers = doc["samplers"].toArray();

        for (auto const& sampler : samplers) {
            auto so = sampler.toObject();
            // check for nearest in any filter slot
            if (so["magFilter"].toInt() == 9728) return true;
            if (so["minFilter"].toInt() == 9728) return true;
        }

        return false;
    };

    qDebug() << Q_FUNC_INFO << path;
    // THIS IS HORRIBLE AND ONLY HERE TO FIX THE FACT THAT ASSIMP HAS NO SAMPLER
    // CONCEPT.

    if (!path.endsWith(".glb") and !path.endsWith(".gltf")) return false;

    // hacks for GLTF
    QFile file(path);
    if (!file.open(QFile::ReadOnly)) return false;

    std::array<uint32_t, 5> header_first_chunk;
    file.read((char*)header_first_chunk.data(), sizeof(header_first_chunk));

    // check if its really a binary gltf
    if (header_first_chunk[0] != 0x46546C67) {
        // assume just json

        file.seek(0);

        return check_json(file.readAll());
    }


    // first chunk has to be json

    auto chunk_len  = header_first_chunk[3];
    auto chunk_type = header_first_chunk[4];

    if (chunk_type != 0x4E4F534A) return false;

    auto json_payload = file.read(chunk_len);

    return check_json(json_payload);
}


std::variant<ModelPtr, QString> make_thing(int                  id,
                                           QString              path,
                                           noo::DocumentTPtrRef doc,
                                           noo::ObjectTPtr      collective_root,
                                           ImportOptions        options) {

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

    options.force_samplers_to_nearest = needs_gltf_sampler_hack(path);

    if (options.force_samplers_to_nearest) {
        qDebug() << "Enabling sampler hack";
    }


    return import_ai_scene(*scene, doc, collective_root, id, options);
}

// =============================================================================

void Playground::add_model(QString path, ImportOptions const& options) {
    qInfo() << "Loading" << path;
    auto result =
        make_thing(m_id_counter, path, m_doc, m_collective_root, options);

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

    auto double_sided = QCommandLineOption(
        "double-sided",
        "Force all geometry to be double-sided (no backface culling)");

    parser.addOption(double_sided);

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

    ImportOptions options {
        .double_sided = parser.isSet(double_sided),
    };

    auto start_time = std::chrono::high_resolution_clock::now();

    {
        noo::ObjectData obdata = {
            .name = "Scene Root",
            .tags = QStringList() << noo::names::tag_user_hidden,
        };

        m_collective_root = noo::create_object(m_doc, obdata);
    }

    for (auto const& fname : args) {
        add_model(fname, options);
    }

    auto end_time = std::chrono::high_resolution_clock::now();

    qInfo() << "Done loading models:"
            << std::chrono::duration<double>(end_time - start_time).count()
            << "seconds";

    update_root_tf();
}

Playground::~Playground() { }
