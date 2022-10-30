#pragma once

#include <assimp/BaseImporter.h>
#include <assimp/scene.h>

#include <QString>

class XDMFAssimpImporter : public Assimp::BaseImporter {
public:
    XDMFAssimpImporter();
    virtual ~XDMFAssimpImporter();

public:
    bool CanRead(std::string const& pFile,
                 Assimp::IOSystem*  pIOHandler,
                 bool               checkSig) const;

    aiImporterDesc const* GetInfo() const;

protected:
    void InternReadFile(std::string const& pFile,
                        aiScene*           pScene,
                        Assimp::IOSystem*  pIOHandler);
};
