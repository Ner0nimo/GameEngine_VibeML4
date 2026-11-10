#include "ignis/assets/asset_manager.h"

#include "ignis/core/log.h"
#include "texture_decode.h"

#include <cgltf.h>

#include <cstring>
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

} // namespace

std::shared_ptr<Model> AssetManager::load_model_impl(const std::string& path) {
    auto bytes_r = read_file(path);
    if (!bytes_r.ok()) {
        IGNIS_ERROR("load_model: {}", bytes_r.error());
        return nullptr;
    }
    const std::vector<u8>& bytes = bytes_r.value();

    cgltf_options options{};
    std::memset(&options, 0, sizeof(options));
    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse(&options, bytes.data(), bytes.size(), &data);
    if (result != cgltf_result_success) {
        IGNIS_ERROR("load_model: cgltf_parse failed on '{}' ({})", path, static_cast<int>(result));
        return nullptr;
    }
    result = cgltf_load_buffers(&options, data, path.c_str());
    if (result != cgltf_result_success) {
        IGNIS_ERROR("load_model: cgltf_load_buffers failed on '{}' ({})", path, static_cast<int>(result));
        cgltf_free(data);
        return nullptr;
    }
    result = cgltf_validate(data);
    if (result != cgltf_result_success) {
        IGNIS_WARN("load_model: cgltf_validate reported issues on '{}' ({})", path, static_cast<int>(result));
    }

    auto model = std::make_shared<Model>();
    model->mesh = std::make_shared<Mesh>();
    model->mesh->name = path;

    // Textures first so material slots can reference indices.
    for (size_t i = 0; i < data->images_count; ++i) {
        const cgltf_image& img = data->images[i];
        std::shared_ptr<Texture> tex;
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
            tex = decode_texture_memory(virtual_path, img_bytes, false);
        } else if (img.uri) {
            // External file: resolve relative to the gltf directory.
            std::string dir = path;
            const auto pos = dir.find_last_of("/\\");
            dir = (pos == std::string::npos) ? "" : dir.substr(0, pos + 1);
            tex = load_texture(dir + img.uri);
        }
        model->textures.push_back(tex ? tex : std::make_shared<Texture>());
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

    cgltf_free(data);
    return model;
}

std::shared_ptr<Model> AssetManager::load_model(const std::string& path) {
    auto it = cache_.find(path);
    if (it != cache_.end()) return std::static_pointer_cast<Model>(it->second);
    auto model = load_model_impl(path);
    if (model) cache_[path] = model;
    return model;
}

} // namespace ignis
