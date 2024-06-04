#include <chrono>
#include <fstream>
#include <filesystem>
#include <memory>
#include <thread>

#define _USE_MATH_DEFINES
#include <math.h>

#include "argparse/argparse.hpp"
#include "cnk_loader.h"
#include "dme_loader.h"
#include "zone_loader.h"
#include "utils/gltf/chunk.h"
#include "utils/gltf/dme.h"
#include "utils/adr.h"
#include "utils/materials_3.h"
#include "utils/textures.h"
#include "utils/tsqueue.h"
#include "synthium/synthium.h"
#include "tiny_gltf.h"
#include "version.h"

#include <glm/gtx/euler_angles.hpp>
#include <glob/glob.h>
#include <spdlog/spdlog.h>

using namespace std::chrono_literals;

namespace logger = spdlog;


void process_images(
    synthium::Manager& manager,
    warpgate::utils::tsqueue<
        std::tuple<
            std::string, 
            std::shared_ptr<uint8_t[]>, uint32_t, 
            std::shared_ptr<uint8_t[]>, uint32_t
        >
    >& chunk_queue,
    warpgate::utils::tsqueue<std::pair<std::string, warpgate::Semantic>> &dme_queue,
    std::filesystem::path output_directory
) {
    logger::debug("Got output directory {}", output_directory.string());
    while(!chunk_queue.is_closed() || !dme_queue.is_closed()) {
        auto chunk_value = chunk_queue.try_dequeue_for(1ms);
        if(chunk_value) {
            auto[texture_basename, cnx_data, cnx_length, sny_data, sny_length] = *chunk_value;
            std::span<uint8_t> cnx_map(cnx_data.get(), cnx_length);
            std::span<uint8_t> sny_map(sny_data.get(), sny_length);
            warpgate::utils::textures::process_cnx_sny(texture_basename, cnx_map, sny_map, output_directory);
        }

        auto dme_value = dme_queue.try_dequeue_for(1ms);
        if(dme_value) {
            std::string albedo_name;
            size_t index;
            auto[texture_name, semantic] = *dme_value;
            std::shared_ptr<synthium::Asset2> asset, asset2;
            switch (semantic)
            {
            case warpgate::Semantic::Diffuse:
            case warpgate::Semantic::BaseDiffuse:
            case warpgate::Semantic::baseDiffuse:
            case warpgate::Semantic::diffuseTexture:
            case warpgate::Semantic::DiffuseB:
            case warpgate::Semantic::HoloTexture:
            case warpgate::Semantic::DecalTint:
            case warpgate::Semantic::TilingTint:
            case warpgate::Semantic::DetailMask:
            case warpgate::Semantic::detailMaskTexture:
            case warpgate::Semantic::DetailMaskMap:
            case warpgate::Semantic::Overlay:
            case warpgate::Semantic::Overlay1:
            case warpgate::Semantic::Overlay2:
            case warpgate::Semantic::Overlay3:
            case warpgate::Semantic::Overlay4:
                asset = manager.get(texture_name);
                if(asset) {
                    warpgate::utils::textures::save_texture(texture_name, asset->get_data(), output_directory);
                }
                break;
            case warpgate::Semantic::Bump:
            case warpgate::Semantic::BumpMap:
            case warpgate::Semantic::BumpMap1:
            case warpgate::Semantic::BumpMap2:
            case warpgate::Semantic::BumpMap3:
            case warpgate::Semantic::bumpMap:
                asset = manager.get(texture_name);
                if(asset) {
                    warpgate::utils::textures::process_normalmap(texture_name, asset->get_data(), output_directory);
                }
                break;
            case warpgate::Semantic::Spec:
            case warpgate::Semantic::SpecMap:
            case warpgate::Semantic::SpecGlow:
            case warpgate::Semantic::SpecB:
                albedo_name = texture_name;
                index = albedo_name.find_last_of('_');
                albedo_name[index + 1] = 'C';
                asset = manager.get(texture_name);
                asset2 = manager.get(albedo_name);
                if(asset && asset2) {
                    warpgate::utils::textures::process_specular(texture_name, asset->get_data(), asset2->get_data(), output_directory);
                }
                break;
            case warpgate::Semantic::detailBump:
            case warpgate::Semantic::DetailBump:
                asset = manager.get(texture_name);
                if(asset) {
                    warpgate::utils::textures::process_detailcube(texture_name, asset->get_data(), output_directory);
                }
                break;
            default:
                logger::warn("Skipping unimplemented semantic: {}", texture_name);
                break;
            }
        }
    }
    logger::info("Both queues closed, stopping thread");
}

void build_argument_parser(argparse::ArgumentParser &parser, int &log_level) {
    parser.add_description("C++ Forgelight Chunk to GLTF2 model conversion tool");
    parser.add_argument("input_file");
    parser.add_argument("output_file");
    parser.add_argument("--format", "-f")
        .help("Select the output file format {glb, gltf}")
        .required()
        .action([](const std::string& value) {
            static const std::vector<std::string> choices = { "gltf", "glb" };
            if (std::find(choices.begin(), choices.end(), value) != choices.end()) {
                return value;
            }
            return std::string{ "" };
        });
    
    parser.add_argument("--verbose", "-v")
        .help("Increase log level. May be specified multiple times")
        .action([&](const auto &){ 
            if(log_level > 0) {
                log_level--; 
            }
        })
        .append()
        .nargs(0)
        .default_value(false)
        .implicit_value(true);
    
    parser.add_argument("--no-textures", "-i")
        .help("Exclude the textures from the output")
        .default_value(false)
        .implicit_value(true)
        .nargs(0);

    parser.add_argument("--assets-directory", "-d")
        .help("The directory where the game's assets are stored")
#ifdef _WIN32
        .default_value(std::string("C:/Users/Public/Daybreak Game Company/Installed Games/Planetside 2 Test/Resources/Assets/"));
#else
        .default_value(std::string("/mnt/c/Users/Public/Daybreak Game Company/Installed Games/Planetside 2 Test/Resources/Assets/"));
#endif
    
    parser.add_argument("--threads", "-t")
        .help("The number of threads to use for image processing")
        .default_value(4u)
        .scan<'u', uint32_t>();

    parser.add_argument("--aabb")
        .help("An axis aligned bounding box to constrain which assets are exported. (xmin zmin xmax zmax)")
        .nargs(4)
        //.default_value(std::vector<double>{0.0, 0.0, 0.0, 0.0})
        .scan<'g', double>();
}

int main(int argc, char* argv[]) {
    try {
        argparse::ArgumentParser parser("zone_converter", WARPGATE_VERSION);
        int log_level = logger::level::warn;

        build_argument_parser(parser, log_level);

        try {
            parser.parse_args(argc, argv);
        } catch (const std::runtime_error& err) {
            std::cerr << err.what() << std::endl;
            std::cerr << parser;
            std::exit(1);
        }

        std::optional<warpgate::utils::AABB> aabb = {};
        if(auto aabb_value = parser.present<std::vector<double>>("--aabb")) {
            aabb = warpgate::utils::AABB({(*aabb_value)[0], 0.0, (*aabb_value)[1], 1.0}, {(*aabb_value)[2], 1024.0, (*aabb_value)[3], 1.0});
        }

        logger::set_level(logger::level::level_enum(log_level));

        std::string input_str = parser.get<std::string>("input_file");
        
        logger::info("Converting file {} using zone_converter {}", input_str, WARPGATE_VERSION);

        std::string continent_name = std::filesystem::path(input_str).stem().string();

        std::string path = parser.get<std::string>("--assets-directory");
        std::filesystem::path server(path);
        std::vector<std::filesystem::path> packs = glob::glob({
            (server / (continent_name + "_x64_*.pack2")).string(),
            (server / "assets_x64_*.pack2").string()
        });

        logger::info("Loading {} packs...", packs.size());
        synthium::Manager manager(packs);
        logger::info("Manager loaded.");

        logger::info("Loading materials.json");
        warpgate::utils::materials3::init_materials();
        logger::info("Loaded materials.json");

        std::filesystem::path input_filename(input_str);
        std::unique_ptr<uint8_t[]> data;
        std::vector<uint8_t> data_vector, chunk1_data_vector;
        std::span<uint8_t> data_span, chunk1_data_span;
        if(manager.contains(input_str)) {
            data_vector = manager.get(input_str)->get_data();
            data_span = std::span<uint8_t>(data_vector.data(), data_vector.size());
        } else {
            std::ifstream input(input_filename, std::ios::binary | std::ios::ate);
            if(input.fail()) {
                logger::error("Failed to open file '{}'", input_filename.string());
                std::exit(2);
            }
            size_t length = input.tellg();
            input.seekg(0);
            data = std::make_unique<uint8_t[]>(length);
            input.read((char*)data.get(), length);
            input.close();
            data_span = std::span<uint8_t>(data.get(), length);
        }

        std::filesystem::path output_filename(parser.get<std::string>("output_file"));
        output_filename = std::filesystem::weakly_canonical(output_filename);
        std::filesystem::path output_directory;
        if(output_filename.has_parent_path()) {
            output_directory = output_filename.parent_path();
        }

        try {
            if(!std::filesystem::exists(output_filename.parent_path())) {
                std::filesystem::create_directories(output_directory / "textures");
            }
        } catch (std::filesystem::filesystem_error& err) {
            logger::error("Failed to create directory {}: {}", err.path1().string(), err.what());
            std::exit(3);
        }

        std::string format = parser.get<std::string>("--format");
        bool export_textures = !parser.get<bool>("--no-textures");
        uint32_t image_processor_thread_count = parser.get<uint32_t>("--threads");
        // hmm
        warpgate::utils::tsqueue<
            std::tuple<
                std::string, 
                std::shared_ptr<uint8_t[]>, uint32_t, 
                std::shared_ptr<uint8_t[]>, uint32_t
            >
        > chunk_image_queue;

        warpgate::utils::tsqueue<std::pair<std::string, warpgate::Semantic>> dme_image_queue;

        std::vector<std::thread> image_processor_pool;
        if(export_textures) {
            logger::info("Using {} image processing thread{}", image_processor_thread_count, image_processor_thread_count == 1 ? "" : "s");
            for(uint32_t i = 0; i < image_processor_thread_count; i++) {
                image_processor_pool.push_back(std::thread{
                    process_images,
                    std::ref(manager),
                    std::ref(chunk_image_queue), 
                    std::ref(dme_image_queue),
                    output_directory
                });
            }
        } else {
            logger::info("Not exporting textures by user request.");
        }

        logger::info("Parsing zone...");
        warpgate::zone::Zone continent(data_span);
        logger::info("Parsed zone.");

        if(continent.version() > 3) {
            logger::error("Not currently set up for exporting ZONE version > 3 (got version {})", continent.version());
            return 1;
        }

        tinygltf::Model gltf;
        tinygltf::Sampler dme_sampler, chunk_sampler;
        int dme_sampler_index = (int)gltf.samplers.size();
        dme_sampler.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
        dme_sampler.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
        dme_sampler.wrapS = TINYGLTF_TEXTURE_WRAP_REPEAT;
        dme_sampler.wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
        gltf.samplers.push_back(dme_sampler);

        int chunk_sampler_index = (int)gltf.samplers.size();
        chunk_sampler.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
        chunk_sampler.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
        chunk_sampler.wrapS = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
        chunk_sampler.wrapT = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
        gltf.samplers.push_back(chunk_sampler);
        
        gltf.defaultScene = (int)gltf.scenes.size();
        gltf.scenes.push_back({});

        std::unordered_map<uint32_t, uint32_t> texture_indices;
        std::unordered_map<uint32_t, std::vector<uint32_t>> material_indices;

        warpgate::zone::ZoneHeader header = continent.header();

        int terrain_parent_index = (int)gltf.nodes.size();
        tinygltf::Node terrain_parent;
        terrain_parent.name = "Terrain";
        gltf.nodes.push_back(terrain_parent);
        double min_z = header.chunk_info.start_y * 64.0;
        double max_z = ((int)(header.chunk_info.start_y + header.chunk_info.count_y)) * 64.0;
        std::vector<std::pair<int, int>> chunk_indices;
        for(uint32_t x = 0; x < header.chunk_info.count_x; x += 4) {
            if(aabb) {
                double curr_x = ((int)(header.chunk_info.start_x + x)) * 64.0, next_x = ((int)(header.chunk_info.start_x + x + 4)) * 64.0;
                warpgate::utils::AABB strip_aabb({min_z, 0.0, curr_x, 1.0}, {max_z, 1024.0, next_x, 1.0});
                if(!aabb->overlaps(strip_aabb)) {
                    logger::debug("Skipping x={}", (int)(header.chunk_info.start_x + x));
                    continue;
                }
            }
            for(uint32_t y = 0; y < header.chunk_info.count_y; y += 4) {
                if(aabb) {
                    glm::dvec4 minimum{((int)(header.chunk_info.start_y + y)) * 64.0, 0.0, ((int)(header.chunk_info.start_x + x)) * 64.0, 1.0};
                    glm::dvec4 maximum = minimum + glm::dvec4{256.0, 1024.0, 256.0, 0.0};
                    warpgate::utils::AABB strip_aabb(minimum, maximum);
                    if(!aabb->overlaps(strip_aabb)) {
                        logger::debug("Skipping z={}", (int)(header.chunk_info.start_y + y));
                        continue;
                    }
                }
                chunk_indices.push_back({(int)(header.chunk_info.start_x + x), (int)(header.chunk_info.start_y + y)});
            }
        }
        logger::info("Adding {} chunks...", chunk_indices.size());
        for(auto[x, z] : chunk_indices) {
            std::string chunk_stem = continent_name + "_" + std::to_string(x) + "_" + std::to_string(z);
            std::unique_ptr<uint8_t[]> decompressed_cnk0_data, decompressed_cnk1_data;
            size_t cnk0_length, cnk1_length;
            {
                std::vector<uint8_t> chunk0_data = manager.get(std::filesystem::path(chunk_stem).replace_extension(".cnk0").string())->get_data();
                warpgate::chunk::Chunk compressed_chunk0(chunk0_data);
                decompressed_cnk0_data = std::move(compressed_chunk0.decompress());
                cnk0_length = compressed_chunk0.decompressed_size();
            }
            {
                std::vector<uint8_t> chunk1_data = manager.get(std::filesystem::path(chunk_stem).replace_extension(".cnk1").string())->get_data();
                warpgate::chunk::Chunk compressed_chunk1(chunk1_data);
                decompressed_cnk1_data = std::move(compressed_chunk1.decompress());
                cnk1_length = compressed_chunk1.decompressed_size();
            }

            warpgate::chunk::CNK0 cnk0({decompressed_cnk0_data.get(), cnk0_length});
            warpgate::chunk::CNK1 cnk1({decompressed_cnk1_data.get(), cnk1_length});
            int chunk_index = warpgate::utils::gltf::chunk::add_chunks_to_gltf(
                gltf, cnk0, cnk1, chunk_image_queue, output_directory,
                chunk_stem, chunk_sampler_index, export_textures);
            std::vector<double> translation = {z * 64.0, 0.0, x * 64.0};
            // if(aabb) {
            //     translation[0] -= aabb->midpoint().x;
            //     translation[2] -= aabb->midpoint().z;
            // }
            gltf.nodes.at(chunk_index).translation = translation;
            gltf.nodes.at(terrain_parent_index).children.push_back(chunk_index);
        }

        int object_parent_index = (int)gltf.nodes.size();
        tinygltf::Node object_parent;
        object_parent.name = "Objects";
        gltf.nodes.push_back(object_parent);
        glm::dmat4 gltf_conversion = glm::transpose(glm::dmat4{
            glm::dvec4{1.0, 0.0, 0.0, 0.0},
            glm::dvec4{0.0, 1.0, 0.0, 0.0},
            glm::dvec4{0.0, 0.0, 1.0, 0.0},
            glm::dvec4{0.0, 0.0, 0.0, 1.0},
        });

        uint32_t objects_count = continent.objects_count();
        for(uint32_t i = 0; i < objects_count; i++) {
            std::shared_ptr<warpgate::zone::RuntimeObject> object = continent.object(i);
            logger::info("Loading {}", object->actor_file());
            std::vector<uint8_t> adr_data = manager.get(object->actor_file())->get_data();
            warpgate::utils::ADR adr(adr_data);
            std::optional<std::string> dme_name = adr.base_model();
            if(!dme_name) {
                logger::warn("ADR {} did not have a model file?", object->actor_file());
                continue;
            }
            std::vector<uint8_t> dme_data = manager.get(*dme_name)->get_data();
            warpgate::DME dme(dme_data, std::filesystem::path(object->actor_file()).stem().string());
            
            warpgate::AABB aabb_data = dme.aabb();
            warpgate::utils::AABB dme_aabb(aabb_data.min.x, aabb_data.min.y, aabb_data.min.z, aabb_data.max.x, aabb_data.max.y, aabb_data.max.z);
            std::vector<uint32_t> instances_to_add;
            uint32_t instance_count = object->instance_count();
            for(uint32_t j = 0; j < instance_count; j++) {
                if(aabb && !aabb->overlaps(dme_aabb * object->instance(j).transform() /*(translation * rotation * scale)*/)) {
                    continue;
                }
                instances_to_add.push_back(j);
            }
            if(instances_to_add.size() == 0) {
                continue;
            }
            logger::info("Adding {} instances of {}", instances_to_add.size(), object->actor_file());
            int object_index = warpgate::utils::gltf::dme::add_dme_to_gltf(gltf, dme, dme_image_queue, output_directory, texture_indices, material_indices, dme_sampler_index, export_textures, false, false);
            gltf.nodes.at(object_parent_index).children.push_back(object_index);
            for(auto it = instances_to_add.begin(); it != instances_to_add.end(); it++) {
                glm::dvec4 translation = ((warpgate::zone::Float4)object->instance(*it).translation()).vector() * gltf_conversion;
                glm::dvec4 rot = ((warpgate::zone::Float4)object->instance(*it).rotation()).vector();
                glm::dquat rotation = glm::dquat(glm::eulerAngleYXZ(rot[0], rot[1], rot[2]));
                glm::dvec4 scale = ((warpgate::zone::Float4)object->instance(*it).scale()).vector() * gltf_conversion;
                tinygltf::Node parent;
                int parent_index = (int)gltf.nodes.size();
                parent.name = dme.get_name() + "_" + std::to_string(*it);
                parent.translation = {translation.x, translation.y, translation.z};
                parent.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
                parent.scale = {scale.x, scale.y, scale.z};

                if(it == instances_to_add.begin()) {
                    gltf.nodes.at(object_index).name = parent.name;
                    gltf.nodes.at(object_index).translation = parent.translation;
                    gltf.nodes.at(object_index).rotation = parent.rotation;
                    gltf.nodes.at(object_index).scale = parent.scale;
                    continue;
                }

                gltf.nodes.push_back(parent);
                if(gltf.nodes.at(object_index).children.size() > 0) {
                    for(int child : gltf.nodes.at(object_index).children) {
                        tinygltf::Node child_node;
                        child_node.mesh = gltf.nodes.at(child).mesh;
                        gltf.nodes.at(parent_index).children.push_back((int)gltf.nodes.size());
                        gltf.nodes.push_back(child_node);
                    }
                } else {
                    gltf.nodes.at(parent_index).mesh = gltf.nodes.at(object_index).mesh;
                }
                gltf.nodes.at(object_parent_index).children.push_back(parent_index);
            }
        }

        uint32_t lights_count = continent.lights_count();
        std::unordered_map<uint64_t, uint32_t> light_index_map;
        tinygltf::Node light_parent;
        light_parent.name = "Lights";
        uint32_t light_parent_index = (uint32_t)gltf.nodes.size();
        gltf.nodes.push_back(light_parent);
        logger::info("Checking {} lights...", lights_count);
        for(uint32_t i = 0; i < lights_count; i++) {
            warpgate::zone::Float4 translation = continent.light(i)->translation();
            if(aabb && !aabb->contains({translation.x, translation.y, translation.z})) {
                continue;
            }
            warpgate::zone::Color4ARGB color = continent.light(i)->color();
            warpgate::zone::LightType type = continent.light(i)->type();
            glm::vec4 rot = ((warpgate::zone::Float4)continent.light(i)->rotation()).vector();
            glm::dquat rotation(glm::eulerAngleYXZ(rot[0], rot[1], rot[2]));
            float intensity = ((warpgate::zone::Float2)continent.light(i)->unk_floats()).x;
            uint64_t light_hash = color.r | color.g << 8 | color.b << 16 | ((uint32_t)type & 0xFF) << 24 | ((uint64_t)(*(reinterpret_cast<uint32_t*>(&intensity)))) << 32;
            if(light_index_map.find(light_hash) == light_index_map.end()) {
                light_index_map[light_hash] = (uint32_t)gltf.lights.size();
                tinygltf::Light light;
                light.color = {(double)color.r / 255.0, (double)color.g / 255.0, (double)color.b / 255.0};
                light.type = type == warpgate::zone::LightType::Point ? "point" : "spot";
                light.intensity = intensity * 1000;
                if(type == warpgate::zone::LightType::Spot) {
                    light.spot = {};
                }
                gltf.lights.push_back(light);
            }
            tinygltf::Node light_node;
            light_node.name = continent.light(i)->name();
            light_node.translation = {translation.x, translation.y, translation.z};
            light_node.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
            light_node.scale = {1.0, 1.0, -1.0};
            light_node.extensions["KHR_lights_punctual"] = tinygltf::Value(tinygltf::Value::Object());
            light_node.extensions["KHR_lights_punctual"].Get<tinygltf::Value::Object>()["light"] = tinygltf::Value((int)light_index_map.at(light_hash));
            gltf.nodes.at(light_parent_index).children.push_back((int)gltf.nodes.size());
            gltf.nodes.push_back(light_node);
        }
        logger::info("Added {} lights.", gltf.nodes.at(light_parent_index).children.size());
        
        gltf.asset.version = "2.0";
        gltf.asset.generator = "warpgate " + std::string(WARPGATE_VERSION) + " via tinygltf";

        logger::info("Writing GLTF2 file {}...", output_filename.filename().string());
        tinygltf::TinyGLTF writer;
        writer.WriteGltfSceneToFile(&gltf, output_filename.string(), false, format == "glb", format == "gltf", format == "glb");
        
        chunk_image_queue.close();
        dme_image_queue.close();
        logger::info("Joining image processing thread{}...", image_processor_pool.size() == 1 ? "" : "s");
        for(uint32_t i = 0; i < image_processor_pool.size(); i++) {
            image_processor_pool.at(i).join();
        }
        logger::info("Done.");
    } catch(std::exception &err) {
        logger::error("Caught {}", err.what());
        std::exit(1);
    }
    return 0;
}