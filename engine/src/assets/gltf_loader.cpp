#include "ignis/assets/asset_manager.h"

#include "texture_decode.h"

#include <cgltf.h>

#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <stdexcept>

namespace ignis {

namespace {

math::Vec3 read_vec3(const float* p) { return {p[0], p[1], p[2]}; }
math::Vec4 read_vec4(const float* p) { return {p[0], p[1], p[2], p[3]}; }

// Read an accessor into a flat float buffer.
std::vector<float> read_accessor_floats(const cgltf_accessor* acc) {
    const size_t count = acc->count;
    const size_t comps = cgltf_num_components(acc->type);
    std::vector<float> out(count * comps);
    if (cgltf_accessor_unpack_floats(acc, out.data(), out.size()) != count * comps) {
        throw std::runtime_error("cgltf_accessor_unpack_floats failed");
    }
    return out;
}

// Builds the engine Model from a parsed, buffer-loaded, validated cgltf
// document. May throw (read_accessor_floats); load_model_impl contains that.
// A referenced image that fails to load is recorded in Model::texture_errors
// with a null texture slot (structured, never concealed); load_model() turns
// texture errors into a failure, load_model_lenient() keeps the partial model
// for the renderer's recovery policy (issue #114).
Result<std::shared_ptr<Model>> build_model(const cgltf_data* data, const std::string& path,
                                           AssetManager& assets) {
    using ModelResult = Result<std::shared_ptr<Model>>;
    auto model = std::make_shared<Model>();
    model->mesh = std::make_shared<Mesh>();
    model->mesh->name = path;

    // Textures first so material slots can reference indices. A failed image is
    // recorded as a structured AssetError and leaves a null slot — the model
    // itself still builds; load_model() turns texture errors into a failure,
    // load_model_lenient() exposes the partial model for renderer recovery.
    for (size_t i = 0; i < data->images_count; ++i) {
        const cgltf_image& img = data->images[i];
        Result<std::shared_ptr<Texture>> tex_r = Result<std::shared_ptr<Texture>>::failure(
            "image has neither bufferView nor uri");
        AssetError err;
        err.image_index = static_cast<int>(i);
        if (img.buffer_view && img.buffer_view->buffer && img.buffer_view->buffer->data) {
            const u8* base = static_cast<const u8*>(img.buffer_view->buffer->data);
            const size_t offset = img.buffer_view->offset;
            const size_t size = img.buffer_view->size;
            // Decode via the texture pipeline by extension hint from mime type.
            const char* mime = img.mime_type ? img.mime_type : "";
            const std::string ext = std::strstr(mime, "png") ? ".png" :
                                    std::strstr(mime, "jpeg") ? ".jpg" : ".bin";
            const std::string virtual_path = path + "#image" + std::to_string(i) + ext;
            std::vector<u8> img_bytes(base + offset, base + offset + size);
            tex_r = decode_texture_memory(virtual_path, img_bytes, false);
            if (!tex_r.ok()) {
                err.kind = AssetErrorKind::Decode;
                err.message = tex_r.error();
                err.source_path = virtual_path;
            }
        } else if (img.uri) {
            // External file: resolve relative to the gltf directory.
            std::string dir = path;
            const auto pos = dir.find_last_of("/\\");
            dir = (pos == std::string::npos) ? "" : dir.substr(0, pos + 1);
            const std::string img_path = dir + img.uri;
            tex_r = assets.load_texture(img_path);
            if (!tex_r.ok()) {
                err.source_path = img_path;
                // Classify Io vs decode: re-read to see if the file exists.
                auto fr = assets.read_file(img_path);
                if (fr.ok()) {
                    err.kind = AssetErrorKind::Decode;
                    err.message = "texture decode failed for '" + img_path + "': " + tex_r.error();
                } else {
                    err.kind = AssetErrorKind::MissingDependency;
                    err.message = fr.error();
                }
            }
        } else {
            err.kind = AssetErrorKind::InvalidFormat;
            err.message = "image has neither bufferView nor uri";
        }
        if (!tex_r.ok()) {
            model->texture_errors.push_back(std::move(err));
            model->textures.push_back(nullptr);
        } else {
            model->textures.push_back(std::move(tex_r.value()));
        }
    }

    // Materials.
    for (size_t i = 0; i < data->materials_count; ++i) {
        const cgltf_material& gm = data->materials[i];
        Material m;
        m.name = gm.name ? gm.name : ("material_" + std::to_string(i));
        const cgltf_pbr_metallic_roughness& pbr = gm.pbr_metallic_roughness;
        m.base_color = read_vec4(pbr.base_color_factor); // array, always valid (default white)
        m.metallic = pbr.metallic_factor;
        m.roughness = pbr.roughness_factor;
        m.double_sided = gm.double_sided;
        if (pbr.base_color_texture.texture && pbr.base_color_texture.texture->image) {
            for (size_t k = 0; k < data->images_count; ++k) {
                if (&data->images[k] == pbr.base_color_texture.texture->image) {
                    m.base_color_texture = static_cast<int>(k);
                    break;
                }
            }
        }
        if (pbr.metallic_roughness_texture.texture && pbr.metallic_roughness_texture.texture->image) {
            for (size_t k = 0; k < data->images_count; ++k) {
                if (&data->images[k] == pbr.metallic_roughness_texture.texture->image) {
                    m.metallic_roughness_texture = static_cast<int>(k);
                    break;
                }
            }
        }
        if (gm.normal_texture.texture && gm.normal_texture.texture->image) {
            for (size_t k = 0; k < data->images_count; ++k) {
                if (&data->images[k] == gm.normal_texture.texture->image) {
                    m.normal_texture = static_cast<int>(k);
                    break;
                }
            }
        }
        if (gm.occlusion_texture.texture && gm.occlusion_texture.texture->image) {
            for (size_t k = 0; k < data->images_count; ++k) {
                if (&data->images[k] == gm.occlusion_texture.texture->image) {
                    m.occlusion_texture = static_cast<int>(k);
                    break;
                }
            }
        }
        if (gm.emissive_texture.texture && gm.emissive_texture.texture->image) {
            for (size_t k = 0; k < data->images_count; ++k) {
                if (&data->images[k] == gm.emissive_texture.texture->image) {
                    m.emissive_texture = static_cast<int>(k);
                    break;
                }
            }
        }
        m.emissive = read_vec3(gm.emissive_factor); // array, always valid (default black)
        model->materials.push_back(std::move(m));
    }

    // Meshes: merge all primitives of all meshes into one Mesh.
    for (size_t mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& gmesh = data->meshes[mi];
        for (size_t pi = 0; pi < gmesh.primitives_count; ++pi) {
            const cgltf_primitive& prim = gmesh.primitives[pi];
            MeshPrimitive out;
            out.material_index = prim.material ? static_cast<u32>(
                static_cast<size_t>(prim.material - data->materials)) : 0;

            for (size_t ai = 0; ai < prim.attributes_count; ++ai) {
                const cgltf_attribute& attr = prim.attributes[ai];
                const cgltf_accessor* acc = attr.data;
                if (!acc) continue;
                switch (attr.type) {
                    case cgltf_attribute_type_position: {
                        auto v = read_accessor_floats(acc);
                        for (size_t k = 0; k + 2 < v.size(); k += 3)
                            out.positions.push_back({v[k], v[k + 1], v[k + 2]});
                        break;
                    }
                    case cgltf_attribute_type_normal: {
                        auto v = read_accessor_floats(acc);
                        for (size_t k = 0; k + 2 < v.size(); k += 3)
                            out.normals.push_back({v[k], v[k + 1], v[k + 2]});
                        break;
                    }
                    case cgltf_attribute_type_texcoord: {
                        if (attr.index == 0) {
                            auto v = read_accessor_floats(acc);
                            for (size_t k = 0; k + 1 < v.size(); k += 2)
                                out.texcoords.push_back({v[k], v[k + 1]});
                        }
                        break;
                    }
                    case cgltf_attribute_type_tangent: {
                        auto v = read_accessor_floats(acc);
                        for (size_t k = 0; k + 3 < v.size(); k += 4)
                            out.tangents.push_back({v[k], v[k + 1], v[k + 2], v[k + 3]});
                        break;
                    }
                    default: break;
                }
            }

            if (prim.indices) {
                const cgltf_accessor* idx = prim.indices;
                const size_t count = idx->count;
                if (idx->component_type == cgltf_component_type_r_16u) {
                    std::vector<u16> buf(count);
                    cgltf_accessor_unpack_indices(idx, buf.data(), sizeof(u16), count);
                    out.indices.assign(buf.begin(), buf.end());
                } else if (idx->component_type == cgltf_component_type_r_32u) {
                    std::vector<u32> buf(count);
                    cgltf_accessor_unpack_indices(idx, buf.data(), sizeof(u32), count);
                    out.indices = std::move(buf);
                } else {
                    std::vector<u8> buf(count);
                    cgltf_accessor_unpack_indices(idx, buf.data(), sizeof(u8), count);
                    out.indices.assign(buf.begin(), buf.end());
                }
            } else {
                // Non-indexed primitive: sequential indices.
                const size_t n = out.positions.size();
                out.indices.resize(n);
                for (size_t k = 0; k < n; ++k) out.indices[k] = static_cast<u32>(k);
            }

            if (!out.positions.empty()) model->mesh->primitives.push_back(std::move(out));
        }
    }

    return ModelResult::success(std::move(model));
}

} // namespace

Result<std::shared_ptr<Model>> AssetManager::load_model_impl(const std::string& path) {
    using ModelResult = Result<std::shared_ptr<Model>>;
    // The whole loading lifecycle (read, parse, buffer IO, validate, build) is
    // inside one exception boundary: per the AssetManager contract, loaders
    // never throw — every failure is returned as a Result carrying the reason.
    try {
        auto bytes_r = read_file(path);
        if (!bytes_r.ok())
            return ModelResult::failure(std::format("load_model: {}", bytes_r.error()));
        const std::vector<u8>& bytes = bytes_r.value();

        // Resource limits, applied before any allocation driven by declared
        // (attacker-controlled) sizes inside the document.
        constexpr size_t kMaxModelBytes = 256ull << 20;      // glTF/GLB document
        constexpr cgltf_size kMaxBuffers = 1024;
        constexpr cgltf_size kMaxImages = 1024;
        constexpr cgltf_size kMaxBufferViews = 65536;
        constexpr cgltf_size kMaxAccessors = 65536;
        constexpr cgltf_size kMaxMeshes = 4096;
        constexpr cgltf_size kMaxSingleBufferBytes = 256ull << 20;
        constexpr cgltf_size kMaxTotalBufferBytes = 512ull << 20;
        if (bytes.size() > kMaxModelBytes)
            return ModelResult::failure(
                std::format("load_model: '{}' too large ({} bytes)", path, bytes.size()));

        cgltf_options options{};
        std::memset(&options, 0, sizeof(options));
        cgltf_data* raw = nullptr;
        cgltf_result result = cgltf_parse(&options, bytes.data(), bytes.size(), &raw);
        if (result != cgltf_result_success)
            return ModelResult::failure(std::format("load_model: cgltf_parse failed on '{}' ({})",
                                                    path, static_cast<int>(result)));
        // RAII: the cgltf document is freed exactly once on every exit path.
        std::unique_ptr<cgltf_data, void (*)(cgltf_data*)> data(raw, &cgltf_free);

        if (data->buffers_count > kMaxBuffers || data->images_count > kMaxImages ||
            data->buffer_views_count > kMaxBufferViews || data->accessors_count > kMaxAccessors ||
            data->meshes_count > kMaxMeshes)
            return ModelResult::failure(
                std::format("load_model: too many objects in '{}' (buffers={} images={} views={} accessors={} meshes={})",
                            path, data->buffers_count, data->images_count, data->buffer_views_count,
                            data->accessors_count, data->meshes_count));
        cgltf_size total_buffer_bytes = 0;
        for (size_t i = 0; i < data->buffers_count; ++i) {
            const cgltf_size bsize = data->buffers[i].size;
            if (bsize > kMaxSingleBufferBytes)
                return ModelResult::failure(
                    std::format("load_model: buffer {} of '{}' claims {} bytes (max {})",
                                i, path, bsize, kMaxSingleBufferBytes));
            total_buffer_bytes += bsize;
            if (total_buffer_bytes > kMaxTotalBufferBytes)
                return ModelResult::failure(
                    std::format("load_model: aggregate buffer size of '{}' exceeds {} bytes",
                                path, kMaxTotalBufferBytes));
        }

        // Non-editor access modes must not let cgltf open host files for external
        // buffer URIs (that would bypass the pak/project-root boundary): resolve
        // them through read_file and preset the buffer data, so cgltf_load_buffers
        // skips its own FILE* loading for those buffers.
        if (mode_ != AssetAccessMode::EditorFilesystem) {
            std::string dir = path;
            const auto pos = dir.find_last_of("/\\");
            dir = (pos == std::string::npos) ? "" : dir.substr(0, pos + 1);
            for (size_t i = 0; i < data->buffers_count; ++i) {
                cgltf_buffer& buf = data->buffers[i];
                if (buf.data != nullptr || buf.uri == nullptr ||
                    std::strncmp(buf.uri, "data:", 5) == 0)
                    continue;
                std::string uri = buf.uri;
                static_cast<void>(cgltf_decode_uri(uri.data()));
                auto buf_bytes = read_file(dir + uri);
                if (!buf_bytes.ok())
                    return ModelResult::failure(
                        std::format("load_model: buffer '{}' rejected on '{}': {}", uri, path,
                                    buf_bytes.error()));
                void* copy = std::malloc(buf_bytes.value().size());
                if (copy == nullptr)
                    return ModelResult::failure(std::format(
                        "load_model: out of memory loading buffer '{}' on '{}'", uri, path));
                std::memcpy(copy, buf_bytes.value().data(), buf_bytes.value().size());
                buf.data = copy;
                buf.size = static_cast<cgltf_size>(buf_bytes.value().size());
                buf.data_free_method = cgltf_data_free_method_memory_free;
            }
        }

        result = cgltf_load_buffers(&options, data.get(), path.c_str());
        if (result != cgltf_result_success)
            return ModelResult::failure(
                std::format("load_model: cgltf_load_buffers failed on '{}' ({})",
                            path, static_cast<int>(result)));
        // Reject models that fail validation: cgltf_accessor_unpack_floats performs
        // unchecked reads, so out-of-range buffer views/accessors must never reach it.
        result = cgltf_validate(data.get());
        if (result != cgltf_result_success)
            return ModelResult::failure(
                std::format("load_model: cgltf_validate failed on '{}' ({})",
                            path, static_cast<int>(result)));

        return build_model(data.get(), path, *this);
    } catch (const std::exception& e) {
        return ModelResult::failure(
            std::format("load_model: failed to load '{}': {}", path, e.what()));
    }
}

Result<std::shared_ptr<Model>> AssetManager::load_model_lenient(const std::string& path) {
    auto it = cache_.find(path);
    if (it != cache_.end())
        return Result<std::shared_ptr<Model>>::success(
            std::static_pointer_cast<Model>(it->second));
    auto model_r = load_model_impl(path);
    if (model_r.ok()) cache_[path] = model_r.value();
    return model_r;
}

Result<std::shared_ptr<Model>> AssetManager::load_model(const std::string& path) {
    auto model_r = load_model_lenient(path);
    if (model_r.ok() && !model_r.value()->texture_errors.empty()) {
        // Strict contract: a model with failed texture images is a failure for
        // general callers; the renderer uses load_model_lenient for recovery.
        std::string msg = std::format("load_model: '{}' failed image(s):", path);
        for (const AssetError& e : model_r.value()->texture_errors)
            msg += std::format(" [image {} '{}': {}]", e.image_index, e.source_path, e.message);
        return Result<std::shared_ptr<Model>>::failure(std::move(msg));
    }
    return model_r;
}

} // namespace ignis
