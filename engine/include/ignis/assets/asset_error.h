#pragma once

#include <string>

namespace ignis {

// Classification of an asset load failure, preserved by the loader so the
// renderer (the layer with rendering context) can decide recovery explicitly
// instead of the failure being concealed inside a Result error string.
enum class AssetErrorKind {
    Io,               // file/entry could not be read
    Decode,           // bytes present but undecodable
    InvalidFormat,    // structurally invalid asset
    MissingDependency // a referenced sub-asset (e.g. a texture image) failed
};

struct AssetError {
    AssetErrorKind kind = AssetErrorKind::Io;
    std::string message;      // original loader reason
    std::string source_path;  // image uri or virtual path that failed
    int image_index = -1;     // index into Model::textures (-1 = not an image)
};

} // namespace ignis
