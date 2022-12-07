#include "utils/gltf/common.h"
#include "utils/gltf/dme.h"
#include "utils/gltf/dmat.h"

#define _USE_MATH_DEFINES
#include <math.h>
#include <spdlog/spdlog.h>

#include "bone.h"
#include "glm/matrix.hpp"
#include "glm/gtx/quaternion.hpp"
#include "half.hpp"
#include "jenkins.h"
#include "ps2_bone_map.h"
#include "utils/materials_3.h"

// Here it is
#include "utils/sign.h"

#include "utils/textures.h"
#include "utils.h"

namespace logger = spdlog;
using half_float::half;

using namespace warpgate;

std::vector<Parameter::Semantic> semantics = {
    Parameter::Semantic::BASE_COLOR,
    Parameter::Semantic::NORMAL_MAP,
    Parameter::Semantic::SPECULAR,
    Parameter::Semantic::DETAIL_SELECT,
    Parameter::Semantic::DETAIL_CUBE,
    Parameter::Semantic::OVERLAY0,
    Parameter::Semantic::OVERLAY1,
    Parameter::Semantic::BASE_CAMO,
};

int utils::gltf::dmat::add_material_to_gltf(
    tinygltf::Model &gltf, 
    const DMAT &dmat, 
    uint32_t material_index, 
    bool export_textures,
    std::unordered_map<uint32_t, uint32_t> &texture_indices,
    std::unordered_map<uint32_t, std::vector<uint32_t>> &material_indices,
    utils::tsqueue<std::pair<std::string, Parameter::Semantic>> &image_queue,
    std::filesystem::path output_directory,
    std::string dme_name
) {
    tinygltf::Material material;
    if(export_textures) {
        build_material(gltf, material, dmat, material_index, texture_indices, image_queue, output_directory);
    } else {
        material.pbrMetallicRoughness.baseColorFactor = { 0.133, 0.545, 0.133, 1.0 }; // Forest Green
    }
    std::unordered_map<uint32_t, std::vector<uint32_t>>::iterator value;
    uint32_t material_namehash = dmat.material(material_index)->definition();
    if((value = material_indices.find(material_namehash)) != material_indices.end()) {
        for(uint32_t index : value->second) {
            if(gltf.materials.at(index) == material) {
                return index;
            }
        }
    } else {
        material_indices[material_namehash] = {};
    }
    material_indices[material_namehash].push_back((uint32_t)gltf.materials.size());
    material.doubleSided = true;
    material.name = dme_name + "::" + utils::materials3::materials.at("materialDefinitions").at(std::to_string(material_namehash)).at("name").get<std::string>();
    int to_return = (int)gltf.materials.size();
    gltf.materials.push_back(material);
    return to_return;
}

int utils::gltf::add_texture_to_gltf(
    tinygltf::Model &gltf, 
    std::filesystem::path texture_path, 
    std::filesystem::path output_directory, 
    std::optional<std::string> label
) {
    int index = (int)gltf.textures.size();
    tinygltf::Texture tex;
    tex.source = (int)gltf.images.size();
    tex.name = label ? *label : texture_path.filename().string();
    tex.sampler = 0;
    tinygltf::Image img;
    img.uri = texture_path.lexically_relative(output_directory).string();
    
    gltf.textures.push_back(tex);
    gltf.images.push_back(img);
    return index;
}

int utils::gltf::dme::add_mesh_to_gltf(tinygltf::Model &gltf, const DME &dme, uint32_t index, uint32_t material_index) {
    int texcoord = 0;
    int color = 0;
    tinygltf::Mesh gltf_mesh;
    tinygltf::Primitive primitive;
    std::shared_ptr<const Mesh> mesh = dme.mesh(index);
    std::vector<uint32_t> offsets((std::size_t)mesh->vertex_stream_count(), 0);
    nlohmann::json input_layout = utils::materials3::get_input_layout(dme.dmat()->material(index)->definition());
    std::string layout_name = input_layout.at("name").get<std::string>();
    logger::info("Using input layout {}", layout_name);
    bool rigid = utils::uppercase(layout_name).find("RIGID") != std::string::npos;
    
    std::vector<tinygltf::Buffer> buffers;
    for(uint32_t j = 0; j < mesh->vertex_stream_count(); j++) {
        std::span<uint8_t> vertex_stream = mesh->vertex_stream(j);
        tinygltf::Buffer buffer;
        logger::info("Expanding vertex stream {}", j);
        buffer.data = expand_vertex_stream(input_layout, vertex_stream, j, rigid, dme);
        buffers.push_back(buffer);
    }

    for(nlohmann::json entry : input_layout.at("entries")) {
        std::string type = entry.at("type").get<std::string>();
        std::string usage = entry.at("usage").get<std::string>();
        int stream = entry.at("stream").get<int>();
        if(usage == "Binormal") {
            offsets.at(stream) += utils::materials3::sizes.at(type);
            continue;
        }
        logger::info("Adding accessor for {} {} data", type, usage);
        tinygltf::Accessor accessor;
        accessor.bufferView = (int)gltf.bufferViews.size();
        accessor.byteOffset = 0;
        accessor.componentType = utils::materials3::component_types.at(type);
        accessor.type = utils::materials3::types.at(type);
        accessor.count = mesh->vertex_count();

        tinygltf::BufferView bufferview;
        bufferview.buffer = (int)gltf.buffers.size() + stream;
        bufferview.byteLength = buffers.at(stream).data.size() - offsets.at(stream);
        bufferview.byteStride = input_layout.at("sizes").at(std::to_string(stream)).get<uint32_t>();
        bufferview.target = TINYGLTF_TARGET_ARRAY_BUFFER;
        bufferview.byteOffset = offsets.at(stream);
        std::string attribute = utils::materials3::usages.at(usage);
        if(usage == "Texcoord") {
            attribute += std::to_string(texcoord);
            texcoord++;
        } else if (usage == "Color") {
            attribute += std::to_string(color);
            color++;
            accessor.normalized = true;
        } else if(usage == "Position") {
            AABB aabb = dme.aabb();
            accessor.minValues = {aabb.min.x, aabb.min.y, aabb.min.z};
            accessor.maxValues = {aabb.max.x, aabb.max.y, aabb.max.z};
        } else if(usage == "Tangent") {
            accessor.normalized = true;
        }
        primitive.attributes[attribute] = (int)gltf.accessors.size();
        gltf.accessors.push_back(accessor);
        gltf.bufferViews.push_back(bufferview);

        offsets.at(stream) += utils::materials3::sizes.at(type);
    }

    gltf.buffers.insert(gltf.buffers.end(), buffers.begin(), buffers.end());

    std::span<uint8_t> indices = mesh->index_data();
    tinygltf::Accessor accessor;
    accessor.bufferView = (int)gltf.bufferViews.size();
    accessor.byteOffset = 0;
    accessor.componentType = mesh->index_size() == 2 ? TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT : TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
    accessor.type = TINYGLTF_TYPE_SCALAR;
    accessor.count = mesh->index_count();

    tinygltf::BufferView bufferview;
    bufferview.buffer = (int)gltf.buffers.size();
    bufferview.byteLength = indices.size();
    bufferview.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
    bufferview.byteOffset = 0;

    tinygltf::Buffer buffer;
    buffer.data = std::vector<uint8_t>(indices.begin(), indices.end());

    primitive.indices = (int)gltf.accessors.size();
    primitive.mode = TINYGLTF_MODE_TRIANGLES;
    primitive.material = material_index;
    gltf_mesh.primitives.push_back(primitive);

    gltf.accessors.push_back(accessor);
    gltf.bufferViews.push_back(bufferview);
    gltf.buffers.push_back(buffer);

    gltf.scenes.at(gltf.defaultScene).nodes.push_back((int)gltf.nodes.size());

    int node_index = (int)gltf.nodes.size();

    tinygltf::Node node;
    node.mesh = (int)gltf.meshes.size();
    gltf.nodes.push_back(node);
    
    gltf_mesh.name = dme.get_name() + " mesh " + std::to_string(index);
    gltf.meshes.push_back(gltf_mesh);

    return node_index;
}

void utils::gltf::dme::add_skeleton_to_gltf(tinygltf::Model &gltf, const DME &dme, std::vector<int> mesh_nodes) {
    for(int node_index : mesh_nodes) {
        gltf.nodes.at(node_index).skin = (int)gltf.skins.size();
    }

    tinygltf::Buffer bone_buffer;
    tinygltf::Skin skin;
    skin.name = dme.get_name();
    skin.inverseBindMatrices = (int)gltf.accessors.size();

    std::unordered_map<uint32_t, size_t> skeleton_map;
    for(uint32_t bone_index = 0; bone_index < dme.bone_count(); bone_index++) {
        tinygltf::Node bone_node;
        Bone bone = dme.bone(bone_index);
        PackedMat4 packed_inv = bone.inverse_bind_matrix;
        uint32_t namehash = bone.namehash;
        glm::mat4 inverse_bind_matrix(glm::mat4x3(
            packed_inv[0][0], packed_inv[0][1], packed_inv[0][2],
            packed_inv[1][0], packed_inv[1][1], packed_inv[1][2],
            packed_inv[2][0], packed_inv[2][1], packed_inv[2][2],
            packed_inv[3][0], packed_inv[3][1], packed_inv[3][2]
        ));
        glm::mat4 bind_matrix = glm::inverse(inverse_bind_matrix);
        glm::mat4 rotated = glm::rotate(inverse_bind_matrix, (float)M_PI, glm::vec3(0, 1, 0));
        std::span<float> inverse_matrix_data(&inverse_bind_matrix[0].x, 16);
        std::span<float> matrix_data(&bind_matrix[0].x, 16);
        //bone_node.matrix = std::vector<double>(matrix_data.begin(), matrix_data.end());
        bone_node.translation = {bind_matrix[3].x, bind_matrix[3].y, bind_matrix[3].z};
        bind_matrix[3].x = 0;
        bind_matrix[3].y = 0;
        bind_matrix[3].z = 0;
        //bone_node.scale = {glm::length(bind_matrix[0]), glm::length(bind_matrix[1]), glm::length(bind_matrix[2])};
        bind_matrix[0] /= glm::length(bind_matrix[0]);
        bind_matrix[1] /= glm::length(bind_matrix[1]);
        bind_matrix[2] /= glm::length(bind_matrix[2]);
        glm::quat quat(bind_matrix);
        bone_node.rotation = {quat.x, quat.y, quat.z, quat.w};
        std::unordered_map<uint32_t, std::string>::iterator name_iter;
        if((name_iter = utils::bone_hashmap.find(namehash)) != utils::bone_hashmap.end()) {
            bone_node.name = name_iter->second;
        }
        
        skeleton_map[bone.namehash] = gltf.nodes.size();
        skin.joints.push_back((int)gltf.nodes.size());

        bone_buffer.data.insert(
            bone_buffer.data.end(), 
            reinterpret_cast<uint8_t*>(inverse_matrix_data.data()), 
            reinterpret_cast<uint8_t*>(inverse_matrix_data.data()) + inverse_matrix_data.size_bytes()
        );

        gltf.nodes.push_back(bone_node);
    }

    for(auto iter = skeleton_map.begin(); iter != skeleton_map.end(); iter++) {
        uint32_t curr_hash = iter->first;
        size_t curr_index = iter->second;
        uint32_t parent = 0;
        std::unordered_map<uint32_t, uint32_t>::iterator parent_iter;
        if((parent_iter = utils::bone_hierarchy.find(curr_hash)) != utils::bone_hierarchy.end()) {
            parent = parent_iter->second;
        }
        std::unordered_map<uint32_t, size_t>::iterator parent_index;
        while(parent != 0 && (parent_index = skeleton_map.find(parent)) == skeleton_map.end()) {
            parent = utils::bone_hierarchy.at(parent);
        }
        if(parent != 0) {
            // Bone has parent in the skeleton, add it to its parent's children
            gltf.nodes.at(parent_index->second).children.push_back((int)curr_index);
        } else if(parent == 0) {
            // Bone is the root of the skeleton, add it as the skin's skeleton
            skin.skeleton = (int)curr_index;
            //     and make sure it is in the scene
            gltf.scenes.at(gltf.defaultScene).nodes.push_back((int)curr_index);
        }
    }

    update_bone_transforms(gltf, skin.skeleton);

    tinygltf::Accessor accessor;
    accessor.bufferView = (int)gltf.bufferViews.size();
    accessor.byteOffset = 0;
    accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    accessor.type = TINYGLTF_TYPE_MAT4;
    accessor.count = dme.bone_count();

    tinygltf::BufferView bufferview;
    bufferview.buffer = (int)gltf.buffers.size();
    bufferview.byteLength = bone_buffer.data.size();
    bufferview.byteOffset = 0;
    
    gltf.accessors.push_back(accessor);
    gltf.buffers.push_back(bone_buffer);
    gltf.bufferViews.push_back(bufferview);
    gltf.skins.push_back(skin);
}

tinygltf::Model utils::gltf::dme::build_gltf_from_dme(
    const DME &dme, 
    utils::tsqueue<std::pair<std::string, Parameter::Semantic>> &image_queue, 
    std::filesystem::path output_directory, 
    bool export_textures, 
    bool include_skeleton
) {
    tinygltf::Model gltf;
    tinygltf::Sampler sampler;
    sampler.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
    sampler.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
    sampler.wrapS = TINYGLTF_TEXTURE_WRAP_REPEAT;
    sampler.wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
    gltf.samplers.push_back(sampler);
    
    gltf.defaultScene = (int)gltf.scenes.size();
    gltf.scenes.push_back({});

    std::unordered_map<uint32_t, uint32_t> texture_indices;
    std::unordered_map<uint32_t, std::vector<uint32_t>> material_indices;
    std::vector<int> mesh_nodes;

    for(uint32_t i = 0; i < dme.mesh_count(); i++) {
        int material_index = dmat::add_material_to_gltf(gltf, *dme.dmat(), i, export_textures, texture_indices, material_indices, image_queue, output_directory, dme.get_name());
        int node_index = add_mesh_to_gltf(gltf, dme, i, material_index);
        mesh_nodes.push_back(node_index);
        
        logger::info("Added mesh {} to gltf", i);
    }

    if(dme.bone_count() > 0 && include_skeleton) {
        add_skeleton_to_gltf(gltf, dme, mesh_nodes);
    }
    
    gltf.asset.version = "2.0";
    gltf.asset.generator = "DME Converter (C++) " + std::string(CPPDMOD_VERSION) + " via tinygltf";
    return gltf;
}

void utils::gltf::dmat::build_material(
    tinygltf::Model &gltf, 
    tinygltf::Material &material,
    const DMAT &dmat, 
    uint32_t i, 
    std::unordered_map<uint32_t, uint32_t> &texture_indices,
    utils::tsqueue<std::pair<std::string, Parameter::Semantic>> &image_queue,
    std::filesystem::path output_directory
) {
    std::optional<tinygltf::TextureInfo> info;
    std::optional<std::pair<tinygltf::TextureInfo, tinygltf::TextureInfo>> info_pair;
    std::optional<std::string> texture_name, label = {};
    std::filesystem::path temp;
    for(Parameter::Semantic semantic : semantics) {
        switch(semantic) {
        case Parameter::Semantic::NORMAL_MAP:
            info = load_texture_info(gltf, dmat, i, texture_indices, image_queue, output_directory, semantic);
            if(!info)
                break;
            material.normalTexture.index = info->index;
            break;
        case Parameter::Semantic::BASE_COLOR:
            info = load_texture_info(gltf, dmat, i, texture_indices, image_queue, output_directory, semantic);
            if(!info)
                break;
            material.pbrMetallicRoughness.baseColorTexture = *info;
            break;
        case Parameter::Semantic::SPECULAR:
            info_pair = load_specular_info(
                gltf, dmat, i, texture_indices, image_queue, output_directory, semantic
            );
            if(!info_pair)
                break;
            material.pbrMetallicRoughness.metallicRoughnessTexture = info_pair->first;
            material.emissiveTexture = info_pair->second;
            material.emissiveFactor = {1.0, 1.0, 1.0};
            break;
        default:
            // Just export the texture
            label = Parameter::semantic_name(semantic);
            texture_name = dmat.material(i)->texture(semantic);
            if(texture_name) {
                uint32_t hash = jenkins::oaat(*texture_name);
                if(texture_indices.find(hash) != texture_indices.end()) {
                    break;
                }
                texture_indices[hash] = (uint32_t)gltf.textures.size();
                image_queue.enqueue({*texture_name, semantic});
                if (semantic != Parameter::Semantic::DETAIL_CUBE) {
                    add_texture_to_gltf(gltf, (output_directory / "textures" / *texture_name).replace_extension(".png"), output_directory, label);
                } else {
                    temp = std::filesystem::path(*texture_name);
                    for(std::string face : utils::materials3::detailcube_faces) {
                        add_texture_to_gltf(
                            gltf, 
                            (output_directory / "textures" / (temp.stem().string() + "_" + face)).replace_extension(".png"),
                            output_directory,
                            *label + " " + face
                        );
                    }
                }
            }
            break;
        }
    }
}

std::optional<tinygltf::TextureInfo> utils::gltf::dmat::load_texture_info(
    tinygltf::Model &gltf,
    const DMAT &dmat, 
    uint32_t i, 
    std::unordered_map<uint32_t, uint32_t> &texture_indices, 
    utils::tsqueue<std::pair<std::string, Parameter::Semantic>> &image_queue,
    std::filesystem::path output_directory,
    Parameter::Semantic semantic
) {
    std::optional<std::string> texture_name = dmat.material(i)->texture(semantic);
    if(!texture_name) {
        return {};
    }
    tinygltf::TextureInfo info;
    std::string original_name;
    uint32_t hash = jenkins::oaat(*texture_name);
    std::unordered_map<uint32_t, uint32_t>::iterator value;
    if((value = texture_indices.find(hash)) == texture_indices.end()) {
        image_queue.enqueue({*texture_name, semantic});
        
        std::filesystem::path texture_path(*texture_name);
        texture_path.replace_extension(".png");
        texture_path = output_directory / "textures" / texture_path;
        
        texture_indices[hash] = (uint32_t)gltf.textures.size();
        info.index = add_texture_to_gltf(gltf, texture_path, output_directory);
    } else {
        info.index = value->second;
    }
    return info;
}

std::optional<std::pair<tinygltf::TextureInfo, tinygltf::TextureInfo>> utils::gltf::dmat::load_specular_info(
    tinygltf::Model &gltf,
    const DMAT &dmat, 
    uint32_t i, 
    std::unordered_map<uint32_t, uint32_t> &texture_indices, 
    utils::tsqueue<std::pair<std::string, Parameter::Semantic>> &image_queue,
    std::filesystem::path output_directory,
    Parameter::Semantic semantic
) {
    tinygltf::TextureInfo metallic_roughness_info, emissive_info;
    std::optional<std::string> texture_name = dmat.material(i)->texture(semantic);
    if(!texture_name) {
        return {};
    }
    uint32_t hash = jenkins::oaat(*texture_name);
    std::unordered_map<uint32_t, uint32_t>::iterator value;
    if((value = texture_indices.find(hash)) == texture_indices.end()) {
        image_queue.enqueue({*texture_name, semantic});
        std::string metallic_roughness_name = utils::textures::relabel_texture(*texture_name, "MR");

        std::filesystem::path metallic_roughness_path(metallic_roughness_name);
        metallic_roughness_path.replace_extension(".png");
        metallic_roughness_path = output_directory / "textures" / metallic_roughness_path;
        
        texture_indices[hash] = (uint32_t)gltf.textures.size();
        metallic_roughness_info.index = add_texture_to_gltf(gltf, metallic_roughness_path, output_directory);
        
        std::string emissive_name = utils::textures::relabel_texture(*texture_name, "E");
        std::filesystem::path emissive_path = metallic_roughness_path.parent_path() / emissive_name;
        emissive_path.replace_extension(".png");

        hash = jenkins::oaat(emissive_name);
        texture_indices[hash] = (uint32_t)gltf.textures.size();
        emissive_info.index = add_texture_to_gltf(gltf, emissive_path, output_directory);
    } else {
        metallic_roughness_info.index = value->second;
        std::string emissive_name = utils::textures::relabel_texture(*texture_name, "E");
        hash = jenkins::oaat(emissive_name);
        emissive_info.index = texture_indices.at(hash);
    }
    return std::make_pair(metallic_roughness_info, emissive_info);
}

std::vector<uint8_t> utils::gltf::dme::expand_vertex_stream(
    nlohmann::json &layout, 
    std::span<uint8_t> data, 
    uint32_t stream, 
    bool is_rigid, 
    const DME &dme
) {
    VertexStream vertices(data);
    logger::debug("{}['{}']", layout.at("sizes").dump(), std::to_string(stream));
    uint32_t stride = layout.at("sizes")
                            .at(std::to_string(stream))
                            .get<uint32_t>();
    logger::info("Data stride: {}", stride);
    std::vector<std::pair<uint32_t, bool>> offsets;
    bool conversion_required = false;
    int tangent_index = -1;
    int binormal_index = -1;
    int blend_indices_index = -1;
    int blend_weights_index = -1;
    int vert_index_offset = 0;
    bool has_normals = false, bone_remapping = false, weight_conversion = false;

    for(int i = 0; i < layout.at("entries").size(); i++) {
        nlohmann::json &entry = layout.at("entries").at(i);
        if(entry.at("stream").get<uint32_t>() != stream) {
            logger::debug("Skipping entry...");
            if(entry.at("stream").get<uint32_t>() < stream)
                vert_index_offset++;
            continue;
        }
        logger::debug("{}", entry.dump());
        std::string type = entry.at("type").get<std::string>();
        std::string usage = entry.at("usage").get<std::string>();
        bool needs_conversion = type == "Float16_2" || type == "float16_2";
        offsets.push_back({
            utils::materials3::sizes.at(type), 
            needs_conversion
        });
        if(needs_conversion) {
            conversion_required = true;
            entry.at("type") = "Float2";
            layout.at("sizes").at(std::to_string(stream)) = layout.at("sizes").at(std::to_string(stream)).get<uint32_t>() + 4;
        }
        
        if(usage == "Normal") {
            has_normals = true;
        } else if(usage == "Binormal") {
            binormal_index = i;
        } else if(usage == "Tangent") {
            tangent_index = i;
        } else if (usage == "BlendIndices") {
            bone_remapping = true;
            blend_indices_index = i;
        } else if (usage == "BlendWeight" && type == "ubyte4n") {
            weight_conversion = true;
            blend_weights_index = i;
            entry.at("type") = "Float4";
            layout.at("sizes").at(std::to_string(stream)) = layout.at("sizes").at(std::to_string(stream)).get<uint32_t>() + 12;
        }
    }
    
    std::string binormal_type, tangent_type;
    if(binormal_index != -1)
        binormal_type = layout.at("entries").at(binormal_index).at("type");
    if(tangent_index != -1)
        tangent_type = layout.at("entries").at(tangent_index).at("type");
    
    
    bool calculate_normals = !has_normals && binormal_index != -1 && tangent_index != -1;
    bool add_rigid_bones = is_rigid && binormal_type == "ubyte4n";

    if(!conversion_required && !calculate_normals && !add_rigid_bones && !bone_remapping && !weight_conversion) {
        logger::info("No conversion required!");
        return std::vector<uint8_t>(vertices.buf_.begin(), vertices.buf_.end());
    }
    
    if(calculate_normals) {
        logger::info("Calculating normals from tangents and binormals");
        layout.at("sizes").at(std::to_string(stream)) = layout.at("sizes").at(std::to_string(stream)).get<uint32_t>() + 12;
        layout.at("entries") += nlohmann::json::parse("{\"stream\":"+std::to_string(stream)+",\"type\":\"Float3\",\"usage\":\"Normal\",\"usageIndex\":0}");
    }

    if(add_rigid_bones) {
        logger::info("Adding rigid bone weights");
        layout.at("sizes").at(std::to_string(stream)) = layout.at("sizes").at(std::to_string(stream)).get<uint32_t>() + 20;
        layout.at("entries") += nlohmann::json::parse("{\"stream\":"+std::to_string(stream)+",\"type\":\"D3dcolor\",\"usage\":\"BlendIndices\",\"usageIndex\":0}");
        layout.at("entries") += nlohmann::json::parse("{\"stream\":"+std::to_string(stream)+",\"type\":\"Float4\",\"usage\":\"BlendWeight\",\"usageIndex\":0}");
    }

    logger::info("Converting {} entries", std::count_if(offsets.begin(), offsets.end(), [](auto pair) { return pair.second; }));
    std::vector<uint8_t> output;
    for(uint32_t vertex_offset = 0; vertex_offset < vertices.size(); vertex_offset += stride) {
        uint32_t entry_offset = 0;
        float binormal[3] = {0, 0, 0};
        float tangent[3] = {0, 0, 0};
        float sign = 0;
        float normal[3] = {0, 0, 0};
        uint16_t rigid_joint_index = 0;

        float converter[2] = {0, 0};
        for(auto iter = offsets.begin(); iter != offsets.end(); iter++) {
            int index = (int)(iter - offsets.begin());
            if(iter->second) {
                converter[0] = (float)(half)vertices.get<half>(vertex_offset + entry_offset);
                converter[1] = (float)(half)vertices.get<half>(vertex_offset + entry_offset + 2);
                output.insert(output.end(), reinterpret_cast<uint8_t*>(converter), reinterpret_cast<uint8_t*>(converter) + 8);
            } else {
                std::span<uint8_t> to_add = vertices.buf_.subspan(vertex_offset + entry_offset, iter->first);
                if(index == blend_indices_index - vert_index_offset) {
                    for(uint32_t bone_index = 0; bone_index < to_add.size(); bone_index++) {
                        to_add[bone_index] = (uint8_t)dme.map_bone(to_add[bone_index]);
                    }
                }

                float weights[4];
                if(index == blend_weights_index - vert_index_offset) {
                    for(uint32_t weight_index = 0; weight_index < to_add.size(); weight_index++) {
                        weights[weight_index] = (float)(to_add[weight_index]) / 255.0f;
                    }
                    to_add = std::span<uint8_t>(reinterpret_cast<uint8_t*>(weights), sizeof(weights));
                }

                output.insert(output.end(), to_add.begin(), to_add.end());
            }

            if(index == binormal_index - vert_index_offset) {
                if(calculate_normals) {
                    utils::load_vector(binormal_type, vertex_offset, entry_offset, vertices, binormal);
                }
                if(is_rigid && binormal_type == "ubyte4n") {
                    rigid_joint_index = dme.map_bone(vertices.get<uint8_t>(vertex_offset + entry_offset + 3));
                }
            }

            if(calculate_normals && index == tangent_index - vert_index_offset) {
                utils::load_vector(tangent_type, vertex_offset, entry_offset, vertices, tangent);
                if(tangent_type == "ubyte4n") {
                    sign = vertices.get<uint8_t>(vertex_offset + entry_offset + 3) / 255.0f * 2 - 1;
                } else {
                    sign = -1;
                }
            }

            entry_offset += iter->first;
        }

        if(calculate_normals) {
            sign /= std::fabsf(sign);
            utils::normalize(binormal);
            utils::normalize(tangent);
            logger::debug("Tangent {}:  ({: 0.2f} {: 0.2f} {: 0.2f})", tangent_index, tangent[0], tangent[1], tangent[2]);
            logger::debug("Binormal {}: ({: 0.2f} {: 0.2f} {: 0.2f})", binormal_index, binormal[0], binormal[1], binormal[2]);
            normal[0] = binormal[1] * tangent[2] - binormal[2] * tangent[1];
            normal[1] = binormal[2] * tangent[0] - binormal[0] * tangent[2];
            normal[2] = binormal[0] * tangent[1] - binormal[1] * tangent[0];
            utils::normalize(normal);
            normal[0] *= sign;
            normal[1] *= sign;
            normal[2] *= sign;
            logger::debug("Normal:     ({: 0.2f} {: 0.2f} {: 0.2f})", normal[0], normal[1], normal[2], sign);
            logger::debug("Entry offset/stride: {} / {}", entry_offset, stride);
            output.insert(output.end(), reinterpret_cast<uint8_t*>(normal), reinterpret_cast<uint8_t*>(normal) + 12);
        }

        if(add_rigid_bones) {
            uint8_t blend_indices[4] = {(uint8_t)rigid_joint_index, 0, 0, 0};
            float blend_weights[4] = {1, 0, 0, 0};
            output.insert(output.end(), blend_indices, blend_indices + 4);
            output.insert(output.end(), reinterpret_cast<uint8_t*>(blend_weights), reinterpret_cast<uint8_t*>(blend_weights) + 16);
        }
    }
    return output;
}

void utils::gltf::update_bone_transforms(tinygltf::Model &gltf, int skeleton_root) {
    for(int child : gltf.nodes.at(skeleton_root).children) {
        update_bone_transforms(gltf, child);
        glm::dvec3 parent_translation(
            gltf.nodes.at(skeleton_root).translation[0], 
            gltf.nodes.at(skeleton_root).translation[1], 
            gltf.nodes.at(skeleton_root).translation[2]
        );
        glm::dquat parent_rotation(
            gltf.nodes.at(skeleton_root).rotation[3],
            gltf.nodes.at(skeleton_root).rotation[0], 
            gltf.nodes.at(skeleton_root).rotation[1], 
            gltf.nodes.at(skeleton_root).rotation[2]
        );
        
        glm::dvec3 child_translation = glm::dvec3(
            gltf.nodes.at(child).translation[0], 
            gltf.nodes.at(child).translation[1], 
            gltf.nodes.at(child).translation[2]
        );
        glm::dquat child_rotation(
            gltf.nodes.at(child).rotation[3],
            gltf.nodes.at(child).rotation[0], 
            gltf.nodes.at(child).rotation[1], 
            gltf.nodes.at(child).rotation[2]
        );

        child_translation -= parent_translation;
        
        child_rotation = glm::inverse(parent_rotation) * child_rotation;
        child_translation = utils::sign(parent_rotation) * glm::inverse(parent_rotation) * child_translation;


        gltf.nodes.at(child).translation = {
            child_translation.x, 
            child_translation.y, 
            child_translation.z
        };

        gltf.nodes.at(child).rotation = {
            child_rotation.x,
            child_rotation.y,
            child_rotation.z,
            child_rotation.w
        };
    }
}