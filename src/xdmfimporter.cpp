#include "xdmfimporter.h"

#include <assimp/Importer.hpp>
#include <assimp/cimport.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <QDir>
#include <QDirIterator>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>

#include <QDebug>

#include <span>

using ReturnType = std::optional<QString>;

struct MappedFile {
    enum PType {
        Float32,
        Float64,
        Int32,
        Int64,
    };

    QFile                    file;
    std::span<unsigned char> bytes;
    PType                    type = PType::Float32;

    void reset_span(size_t count) {
        size_t bcount = count;

        switch (type) {
        case Float32:
        case Int32: bcount *= 4; break;
        case Float64:
        case Int64: bcount *= 8; break;
        }

        assert(bcount < bytes.size());

        bytes = bytes.subspan(0, bcount);
    }

    MappedFile(QString path, size_t offset, size_t span = 0) : file(path) {
        if (!file.open(QFile::ReadOnly)) return;

        qDebug() << "Mapping" << path << file.size();

        offset = std::min<size_t>(offset, file.size());

        if (span == 0) { span = file.size() - offset; }

        auto mapped_bytes = file.map(offset, span);

        if (!mapped_bytes) return;

        bytes = std::span<unsigned char>(mapped_bytes, span);
    }
};

class XDMFImporter {
    QString m_file_path;
    QDir    m_directory;

    aiScene* m_scene;

    QString resolve_path(QString path);

    std::shared_ptr<MappedFile> get_data(QDomElement element);

    std::shared_ptr<MappedFile> consume_conn(QDomElement element);
    std::shared_ptr<MappedFile> consume_geom(QDomElement element);

    void consume_grid(QDomElement element);
    void consume_domain(QDomElement element);

public:
    XDMFImporter(QString file_path, aiScene* scene);

    ReturnType parse(QFile& file);
};


XDMFImporter::XDMFImporter(QString file_path, aiScene* scene)
    : m_file_path(file_path), m_scene(scene) {
    QFileInfo info(file_path);

    Q_ASSERT(info.exists());

    m_directory = info.absoluteDir();
}

QString XDMFImporter::resolve_path(QString path) {
    path = path.trimmed();

    if (QFileInfo::exists(path)) return path;

    auto fname = QFileInfo(path).fileName();

    qInfo() << "Unable to find" << path << "as absolute path, looking for"
            << fname;

    auto filter = QStringList() << fname;

    QDirIterator iterator(m_directory.path(),
                          filter,
                          QDir::AllEntries | QDir::NoSymLinks |
                              QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);

    QStringList candidates;

    while (iterator.hasNext()) {
        auto possible = iterator.next();
        auto info     = QFileInfo(possible);

        if (info.isFile()) candidates << possible;
    }

    if (candidates.isEmpty()) {
        qCritical() << "Unable to find path. Bailing.";
        return QString();
    }

    return candidates.at(0);
}

MappedFile::PType convert_data_type(QString format, int precision) {
    if (format == "Float") {
        if (precision == 4) return MappedFile::Float32;
        if (precision == 8) return MappedFile::Float64;
        return MappedFile::Float32;
    } else if (format == "Int") {
        if (precision == 4) return MappedFile::Int32;
        if (precision == 8) return MappedFile::Int64;
        return MappedFile::Int32;
    }
    qWarning() << "Unsupported format, expect badness:" << format << precision;
    return MappedFile::Float32;
}

std::shared_ptr<MappedFile> XDMFImporter::get_data(QDomElement element) {
    auto format    = element.attribute("Format");
    auto precision = element.attribute("Precision", "-1").toLong();
    auto data_type = element.attribute("DataType");
    auto seek      = element.attribute("Seek", "0").toLong();
    auto dims      = element.attribute("Dimensions", "0").toLong();

    qInfo() << "Fetching data with format" << format << "precision" << precision
            << "data type" << data_type << "seek" << seek << "dims" << dims;

    if (format != "Binary") return {};

    auto data_file_path = resolve_path(element.text());

    if (data_file_path.isEmpty()) return {};

    auto ret = std::make_shared<MappedFile>(data_file_path, seek);

    if (ret->bytes.empty()) return {};

    ret->type = convert_data_type(data_type, precision);
    ret->reset_span(dims);
    qDebug() << "Mapped:" << ret->bytes.size() << "as" << ret->type;

    return ret;
}

std::shared_ptr<MappedFile> XDMFImporter::consume_conn(QDomElement element) {
    if (element.attribute("TopologyType") != "Triangle") {
        qCritical() << "Topology type is not supported (Triangles only)";
        return {};
    }

    bool ok = false;

    size_t conn_count = element.attribute("NumberOfElements").toULong(&ok);

    qDebug() << "CONN COUNT" << conn_count;

    if (!ok) {
        qCritical() << "Missing number of topology elements";
        return {};
    }

    auto data_elem = element.elementsByTagName("DataItem").at(0).toElement();

    if (data_elem.isNull() or data_elem.attribute("Name") != "Conn") {
        qCritical() << "Missing connectivity data";
        return {};
    }

    auto data = get_data(data_elem);

    if (!data) {
        qCritical() << "Missing connectivity data file";
        return {};
    }

    return data;
}

std::shared_ptr<MappedFile> XDMFImporter::consume_geom(QDomElement element) {
    if (element.attribute("GeometryType") != "XYZ") {
        qCritical() << "Unknown geometry type";
        return {};
    }

    auto node_list = element.elementsByTagName("DataItem");
    for (auto i = 0; i < node_list.count(); i++) {
        auto node_elem = node_list.item(i).toElement();

        if (node_elem.attribute("Name") != "Coord") { continue; }

        auto data = get_data(node_elem);

        return data;
    }

    return {};
}

template <class T, class U>
std::span<T> span_as(std::span<U> other) {
    static_assert(std::is_trivial_v<T> and std::is_trivial_v<U>);

    return { reinterpret_cast<T*>(other.data()), other.size() / sizeof(T) };
}

template <class Function>
auto interpret_mapped(MappedFile& file, Function&& f) {
    switch (file.type) {
    case MappedFile::Float32: return f(span_as<float>(file.bytes));
    case MappedFile::Float64: return f(span_as<double>(file.bytes));
    case MappedFile::Int32: return f(span_as<int32_t>(file.bytes));
    case MappedFile::Int64: return f(span_as<int64_t>(file.bytes));
    }
}

template <class T>
constexpr int vector_length() {
    static_assert("nope");
    return 0;
}

template <>
constexpr int vector_length<aiVector3D>() {
    return 3;
}

template <class T>
std::pair<std::unique_ptr<T[]>, size_t> pack_to(MappedFile& file) {

    qDebug() << Q_FUNC_INFO << file.type;

    if constexpr (std::is_fundamental_v<T>) {
        // one to one, just copy
        return interpret_mapped(file, [](auto span) {
            std::unique_ptr<T[]> ret = std::make_unique<T[]>(span.size());

            std::copy(span.begin(), span.end(), ret.get());

            return std::pair<std::unique_ptr<T[]>, size_t>(std::move(ret),
                                                           span.size());
        });
    } else {
        // GLM type
        return interpret_mapped(file, [](auto span) {
            auto vlen = vector_length<T>();

            auto count = span.size() / vlen;

            std::unique_ptr<T[]> ret = std::make_unique<T[]>(count);

            qDebug() << Q_FUNC_INFO << "Vector Type:" << count << vlen;

            for (size_t v_i = 0; v_i < count; v_i++) {
                size_t place = v_i * vlen;
                T      t;
                for (int i = 0; i < vlen; i++) {
                    t[i] = span[place + i];
                }
                // qDebug() << t.x << t.y << t.z;
                ret[v_i] = t;
            }

            return std::pair<std::unique_ptr<T[]>, size_t>(std::move(ret),
                                                           count);
        });
    }
}

void XDMFImporter::consume_grid(QDomElement element) {
    qDebug() << "Loading Grid...";

    auto time_element = element.elementsByTagName("Time").at(0).toElement();

    auto topology_element =
        element.elementsByTagName("Topology").at(0).toElement();

    auto geometry_element =
        element.elementsByTagName("Geometry").at(0).toElement();

    if (time_element.isNull() or topology_element.isNull() or
        geometry_element.isNull()) {
        qWarning() << "Missing key elements from XDMF";
        return;
    }

    qInfo() << "Importing XDMF at time" << time_element.attribute("Value");

    auto conn_data = consume_conn(topology_element);
    auto geom_data = consume_geom(geometry_element);


    if (!conn_data or !geom_data) {
        qCritical() << "Unable to import, bailing";
        return;
    }

    // interpret data

    auto [positions, positions_size] = pack_to<aiVector3D>(*geom_data);
    auto [indices, indices_size]     = pack_to<uint32_t>(*conn_data);

    //    auto source = noo::MeshSource {
    //        .material     = material,
    //        .positions    = positions,
    //        .indices      = std::as_bytes(std::span(indices)),
    //        .index_format = noo::Format::U32,
    //        .type         = noo::MeshSource::TRIANGLE,
    //    };


    // create model
    auto& model = *m_scene;

    model.mRootNode = new aiNode;

    model.mMaterials    = new aiMaterial*[1];
    model.mNumMaterials = 1;

    auto new_mat        = new aiMaterial;
    model.mMaterials[0] = new_mat;

    model.mMeshes    = new aiMesh*[1];
    model.mNumMeshes = 1;

    auto new_mesh = new aiMesh;

    model.mMeshes[0] = new_mesh;

    new_mesh->mMaterialIndex = 0;

    model.mRootNode->mMeshes    = new unsigned int[1];
    model.mRootNode->mMeshes[0] = 0;
    model.mRootNode->mNumMeshes = 1;


    new_mesh->mVertices    = positions.release();
    new_mesh->mNumVertices = positions_size;

    new_mesh->mName = "imported";


    auto num_tris       = indices_size / 3;
    new_mesh->mNumFaces = num_tris;
    new_mesh->mFaces    = new aiFace[num_tris];

    size_t cursor = 0;

    for (size_t i = 0; i < num_tris; i++) {
        auto& f       = new_mesh->mFaces[i];
        f.mNumIndices = 3;

        f.mIndices = new unsigned int[3];

        f.mIndices[0] = indices[cursor];
        cursor++;
        f.mIndices[1] = indices[cursor];
        cursor++;
        f.mIndices[2] = indices[cursor];
        cursor++;
    }
}

void XDMFImporter::consume_domain(QDomElement element) {
    qDebug() << "Loading Domain...";

    auto node = element.firstChildElement("Grid");

    while (!node.isNull()) {
        consume_grid(node.toElement());
        node = node.nextSiblingElement("Grid");
    }
}

ReturnType XDMFImporter::parse(QFile& file) {
    QDomDocument document("XDMFDocument");

    if (!document.setContent(&file)) { return "Unable to read XML document"; }

    auto doc_elem = document.documentElement();

    auto node = doc_elem.firstChild();

    while (!node.isNull()) {
        auto element = node.toElement();

        if (!element.isNull()) {
            if (element.tagName() == "Domain") { consume_domain(element); }
        }

        node = node.nextSibling();
    }

    return std::nullopt;
}

XDMFAssimpImporter::XDMFAssimpImporter()  = default;
XDMFAssimpImporter::~XDMFAssimpImporter() = default;

bool XDMFAssimpImporter::CanRead(std::string const& pFile,
                                 Assimp::IOSystem*  pIOHandler,
                                 bool               checkSig) const {
    return QString::fromStdString(pFile).endsWith(".xmf");
}

aiImporterDesc const* XDMFAssimpImporter::GetInfo() const {
    static aiImporterDesc const description = {
        .mName           = "Rich XMF Importer",
        .mAuthor         = "It doesn't matter",
        .mMaintainer     = "",
        .mComments       = "None",
        .mFlags          = 0,
        .mMinMajor       = 0,
        .mMinMinor       = 0,
        .mMaxMajor       = 0,
        .mMaxMinor       = 0,
        .mFileExtensions = "xmf",
    };

    return &description;
}

void XDMFAssimpImporter::InternReadFile(std::string const& pFile,
                                        aiScene*           pScene,
                                        Assimp::IOSystem*  pIOHandler) {
    qDebug() << "Loading XMF...";
    auto  file_path = QString::fromStdString(pFile);
    QFile file(file_path);

    if (!file.open(QFile::ReadOnly)) {
        throw DeadlyExportError("Unreadable file");
    }

    XDMFImporter importer(file_path, pScene);

    auto ret = importer.parse(file);

    if (ret) { throw DeadlyExportError(ret.value().toStdString()); }
}
