#include <fstream>
#include <filesystem>
#include <functional>
#include <cmath>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <unordered_set>

#define GLM_FORCE_XYZW_ONLY
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>
#include <synthium/synthium.h>

#include "argparse/argparse.hpp"
#include "mrn_loader.h"
#include "tiny_gltf.h"
#include "json.hpp"
#include "version.h"

namespace logger = spdlog;
using namespace warpgate;

void build_argument_parser(argparse::ArgumentParser &parser, int &log_level) {
    parser.add_description("C++ MRN to GLTF2 animation conversion tool");
    parser.add_argument("input_file");
    parser.add_argument("--output-file", "-o");
    parser.add_argument("--skeleton", "-s")
        .help("Choose the skeleton to export")
        .default_value("");
    parser.add_argument("--animations", "-a")
        .help("Choose the animation(s) to export")
        .nargs(argparse::nargs_pattern::at_least_one);
    parser.add_argument("--format", "-f")
        .help("Select the output file format {glb, gltf}")
        .action([](const std::string& value) {
            static const std::vector<std::string> choices = { "gltf", "glb" };
            if (std::find(choices.begin(), choices.end(), value) != choices.end()) {
                return value;
            }
            return std::string{ "" };
        });
    parser.add_argument("--remap")
        .help("Optional JSON skeleton remap file");
    
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
    
    parser.add_argument("--assets-directory", "-d")
        .help("The directory where the game's assets are stored")
#ifdef _WIN32
        .default_value(std::string("C:/Users/Public/Daybreak Game Company/Installed Games/Planetside 2 Test/Resources/Assets/"));
#else
        .default_value(std::string("/mnt/c/Users/Public/Daybreak Game Company/Installed Games/Planetside 2 Test/Resources/Assets/"));
#endif   
    parser.add_argument("--rigify", "-r")
        .help("Export bones named to match bones generated by Rigify (for humanoid rigs)")
        .default_value(false)
        .implicit_value(true)
        .nargs(0);
    
}

std::string uppercase(const std::string input) {
    std::string temp = input;
    std::transform(temp.begin(), temp.end(), temp.begin(), [](const char value) -> char {
        if(value >= 'a' && value <= 'z') {
            return value - (char)('a' - 'A');
        }
        return value;
    });
    return temp;
}

void add_skeleton_to_gltf(tinygltf::Model &gltf, std::shared_ptr<mrn::Skeleton> skeleton, std::string skeleton_name, nlohmann::json bone_map = {}) {
    logger::info("Exporting skeleton {}...", skeleton_name);
    
    std::vector<int> joint_indices;
    tinygltf::Buffer matrices;
    tinygltf::Skin skin;
    
    if(bone_map.size() != 0) {
        logger::info("Remapping skeleton using provided json...");
        std::vector<mrn::Bone> remapped;
        remapped.resize(skeleton->bones.size());
        std::function<uint32_t(uint32_t)> remap_chain = [&](uint32_t root){
            uint32_t end_index = bone_map[skeleton->bones[root].name]["end"];
            remapped[end_index] = skeleton->bones[root];
            remapped[end_index].index = end_index;
            for(uint32_t i = 0; i < skeleton->bones[root].children.size(); i++) {
                remapped[bone_map[skeleton->bones[root].name]["end"]].children[i] = remap_chain(skeleton->bones[root].children[i]);
            }
            return end_index;
        };
        skeleton->bones = remapped;
    }

    for(uint32_t i = 0; i < skeleton->bones.size(); i++) {
        joint_indices.push_back(gltf.nodes.size());
        glm::vec3 translation = skeleton->bones[i].position;
        glm::quat rotation = skeleton->bones[i].rotation;
        tinygltf::Node node;
        node.name = uppercase(skeleton->bones[i].name);
        node.translation = {translation.x, translation.y, translation.z};
        node.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
        node.children = {skeleton->bones[i].children.begin(), skeleton->bones[i].children.end()};

        gltf.nodes.push_back(node);
        glm::mat4 inverse_bind_matrix = glm::inverse(skeleton->bones[i].global_transform);
        matrices.data.insert(
            matrices.data.end(), 
            (uint8_t*)glm::value_ptr(inverse_bind_matrix), 
            (uint8_t*)glm::value_ptr(inverse_bind_matrix) + sizeof(inverse_bind_matrix)
        );
    }

    skin.name = skeleton_name;
    skin.inverseBindMatrices = gltf.accessors.size();
    skin.joints = joint_indices;

    gltf.skins.push_back(skin);

    tinygltf::Accessor accessor;
    accessor.bufferView = gltf.bufferViews.size();
    accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    accessor.count = gltf.nodes.size();
    accessor.type = TINYGLTF_TYPE_MAT4;

    gltf.accessors.push_back(accessor);

    tinygltf::BufferView bufferView;
    bufferView.buffer = gltf.buffers.size();
    bufferView.byteOffset = 0;
    bufferView.byteLength = matrices.data.size();
    
    gltf.bufferViews.push_back(bufferView);

    matrices.uri = skeleton_name + ".bin";
    gltf.buffers.push_back(matrices);
}

void add_animation_to_gltf(tinygltf::Model &gltf, std::shared_ptr<mrn::SkeletonData> skeleton, std::shared_ptr<mrn::NSAFile> animation, std::string name) {
    tinygltf::Animation gltf_animation;
    gltf_animation.name = name;
    
    size_t offset = 0;
    uint32_t bone_offset = skeleton->bone_count() - animation->bone_count();
    std::vector<float> sample_times;
    for(uint32_t i = 0; i < animation->root_segment()->sample_count(); i++) {
        sample_times.push_back(((float)i) / animation->sample_rate());
    }

    animation->dequantize();

    tinygltf::Buffer animation_buffer;
    animation_buffer.uri = name + ".bin";
    animation_buffer.data.insert(
        animation_buffer.data.end(),
        (uint8_t*)sample_times.data(),
        (uint8_t*)sample_times.data() + sizeof(float) * sample_times.size()
    );

    int time_accessor = gltf.accessors.size();
    tinygltf::Accessor accessor;
    accessor.bufferView = gltf.bufferViews.size();
    accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    accessor.count = sample_times.size();
    accessor.type = TINYGLTF_TYPE_SCALAR;
    accessor.minValues = {sample_times[0]};
    accessor.maxValues = {sample_times[sample_times.size() - 1]};

    gltf.accessors.push_back(accessor);

    tinygltf::BufferView bufferView;
    bufferView.buffer = gltf.buffers.size();
    bufferView.byteOffset = offset;
    bufferView.byteLength = sizeof(float) * sample_times.size();

    gltf.bufferViews.push_back(bufferView);

    offset += sizeof(float) * sample_times.size();

    std::vector<float> static_sample_time = {0.0f};
    animation_buffer.data.insert(
        animation_buffer.data.end(),
        (uint8_t*)static_sample_time.data(),
        (uint8_t*)static_sample_time.data() + sizeof(float) * static_sample_time.size()
    );

    int static_time_accessor = gltf.accessors.size();
    accessor.bufferView = gltf.bufferViews.size();
    accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    accessor.count = static_sample_time.size();
    accessor.type = TINYGLTF_TYPE_SCALAR;
    accessor.minValues = {static_sample_time[0]};
    accessor.maxValues = {static_sample_time[static_sample_time.size() - 1]};

    gltf.accessors.push_back(accessor);

    bufferView.buffer = gltf.buffers.size();
    bufferView.byteOffset = offset;
    bufferView.byteLength = sizeof(float) * static_sample_time.size();

    gltf.bufferViews.push_back(bufferView);

    offset += sizeof(float) * static_sample_time.size();

    std::vector<glm::vec3> root_translation = animation->root_translation();
    if(root_translation.size() != 0) {
        tinygltf::AnimationChannel channel;
        channel.extras_json_string = "{'name': '" + name + " root_translation'}";
        channel.sampler = gltf_animation.samplers.size();
        channel.target_node = 0;
        channel.target_path = "translation";

        gltf_animation.channels.push_back(channel);

        int data_accessor = gltf.accessors.size();
        
        accessor.bufferView = gltf.bufferViews.size();
        accessor.type = TINYGLTF_TYPE_VEC3;
        accessor.count = root_translation.size();
        accessor.minValues = {};
        accessor.maxValues = {};

        gltf.accessors.push_back(accessor);

        bufferView.byteOffset = offset;
        bufferView.byteLength = sizeof(glm::vec3) * root_translation.size();

        gltf.bufferViews.push_back(bufferView);

        animation_buffer.data.insert(
            animation_buffer.data.end(), 
            (uint8_t*)root_translation.data(), 
            (uint8_t*)root_translation.data() + sizeof(glm::vec3) * root_translation.size()
        );

        offset += sizeof(glm::vec3) * root_translation.size();

        tinygltf::AnimationSampler sampler;
        sampler.input = time_accessor;
        sampler.output = data_accessor;

        gltf_animation.samplers.push_back(sampler);
    }

    std::vector<glm::quat> root_rotation = animation->root_rotation();
    if(root_rotation.size() != 0) {
        tinygltf::AnimationChannel channel;
        channel.extras_json_string = "{'name': '" + name + " root_rotation'}";
        channel.sampler = gltf_animation.samplers.size();
        channel.target_node = 0;
        channel.target_path = "rotation";

        gltf_animation.channels.push_back(channel);

        int data_accessor = gltf.accessors.size();
        
        accessor.bufferView = gltf.bufferViews.size();
        accessor.type = TINYGLTF_TYPE_VEC4;
        accessor.count = root_rotation.size();
        accessor.minValues = {};
        accessor.maxValues = {};

        gltf.accessors.push_back(accessor);

        bufferView.byteOffset = offset;
        bufferView.byteLength = sizeof(glm::quat) * root_rotation.size();

        gltf.bufferViews.push_back(bufferView);

        animation_buffer.data.insert(
            animation_buffer.data.end(), 
            (uint8_t*)root_rotation.data(), 
            (uint8_t*)root_rotation.data() + sizeof(glm::quat) * root_rotation.size()
        );

        offset += sizeof(glm::quat) * root_rotation.size();

        tinygltf::AnimationSampler sampler;
        sampler.input = time_accessor;
        sampler.output = data_accessor;

        gltf_animation.samplers.push_back(sampler);
    }

    std::vector<glm::vec3> static_translation = animation->static_translation();
    if(static_translation.size() != 0) {
        for(uint32_t i = 0; i < animation->static_translation_bone_indices().size(); i++) {
            uint32_t bone = animation->static_translation_bone_indices()[i];
            tinygltf::AnimationChannel channel;
            channel.extras_json_string = "{'name': '" + name + " static_translation'}";
            channel.sampler = gltf_animation.samplers.size();
            channel.target_node = bone + bone_offset;
            channel.target_path = "translation";

            gltf_animation.channels.push_back(channel);
           
            int data_accessor = gltf.accessors.size();
            
            accessor.bufferView = gltf.bufferViews.size();
            accessor.type = TINYGLTF_TYPE_VEC3;
            accessor.count = 1;
            accessor.minValues = {};
            accessor.maxValues = {};

            gltf.accessors.push_back(accessor);

            bufferView.byteOffset = offset;
            bufferView.byteLength = sizeof(glm::vec3);

            gltf.bufferViews.push_back(bufferView);

            animation_buffer.data.insert(
                animation_buffer.data.end(), 
                (uint8_t*)glm::value_ptr(static_translation[i]), 
                (uint8_t*)glm::value_ptr(static_translation[i]) + sizeof(glm::vec3)
            );

            offset += sizeof(glm::vec3);

            tinygltf::AnimationSampler sampler;
            sampler.input = static_time_accessor;
            sampler.output = data_accessor;

            gltf_animation.samplers.push_back(sampler);
        }
    }

    std::vector<glm::quat> static_rotation = animation->static_rotation();
    if(static_rotation.size() != 0) {
        for(uint32_t i = 0; i < animation->static_rotation_bone_indices().size(); i++) {
            uint32_t bone = animation->static_rotation_bone_indices()[i];
            tinygltf::AnimationChannel channel;
            channel.extras_json_string = "{'name': '" + name + " static_rotation'}";
            channel.sampler = gltf_animation.samplers.size();
            channel.target_node = bone + bone_offset;
            channel.target_path = "rotation";

            gltf_animation.channels.push_back(channel);
           
            int data_accessor = gltf.accessors.size();
            
            accessor.bufferView = gltf.bufferViews.size();
            accessor.type = TINYGLTF_TYPE_VEC4;
            accessor.count = 1;
            accessor.minValues = {};
            accessor.maxValues = {};

            gltf.accessors.push_back(accessor);

            bufferView.byteOffset = offset;
            bufferView.byteLength = sizeof(glm::quat);

            gltf.bufferViews.push_back(bufferView);

            animation_buffer.data.insert(
                animation_buffer.data.end(), 
                (uint8_t*)glm::value_ptr(static_rotation[i]), 
                (uint8_t*)glm::value_ptr(static_rotation[i]) + sizeof(glm::quat)
            );

            offset += sizeof(glm::quat);

            tinygltf::AnimationSampler sampler;
            sampler.input = static_time_accessor;
            sampler.output = data_accessor;

            gltf_animation.samplers.push_back(sampler);
        }
    }

    std::vector<std::vector<glm::vec3>> dynamic_translation_samples = animation->dynamic_translation();
    if(dynamic_translation_samples.size() != 0) {
        for(uint32_t i = 0; i < animation->dynamic_translation_bone_indices().size(); i++) {
            uint32_t bone = animation->dynamic_translation_bone_indices()[i];
            tinygltf::AnimationChannel channel;
            channel.extras_json_string = "{'name': '" + name + " dynamic_translation'}";
            channel.sampler = gltf_animation.samplers.size();
            channel.target_node = bone + bone_offset;
            channel.target_path = "translation";

            gltf_animation.channels.push_back(channel);

            std::vector<glm::vec3> dynamic_translation;
            for(uint32_t sample = 0; sample < dynamic_translation_samples.size(); sample++) {
                dynamic_translation.push_back(dynamic_translation_samples[sample][i]);
            }

            int data_accessor = gltf.accessors.size();
            
            accessor.bufferView = gltf.bufferViews.size();
            accessor.type = TINYGLTF_TYPE_VEC3;
            accessor.count = dynamic_translation.size();
            accessor.minValues = {};
            accessor.maxValues = {};

            gltf.accessors.push_back(accessor);

            bufferView.byteOffset = offset;
            bufferView.byteLength = sizeof(glm::vec3) * dynamic_translation.size();

            gltf.bufferViews.push_back(bufferView);

            animation_buffer.data.insert(
                animation_buffer.data.end(), 
                (uint8_t*)dynamic_translation.data(), 
                (uint8_t*)dynamic_translation.data() + sizeof(glm::vec3) * dynamic_translation.size()
            );

            offset += sizeof(glm::vec3) * dynamic_translation.size();

            tinygltf::AnimationSampler sampler;
            sampler.input = time_accessor;
            sampler.output = data_accessor;

            gltf_animation.samplers.push_back(sampler);
        }
    }

    std::vector<std::vector<glm::quat>> dynamic_rotation_samples = animation->dynamic_rotation();
    if(dynamic_rotation_samples.size() != 0) {
        for(uint32_t i = 0; i < animation->dynamic_rotation_bone_indices().size(); i++) {
            uint32_t bone = animation->dynamic_rotation_bone_indices()[i];
            tinygltf::AnimationChannel channel;
            channel.extras_json_string = "{'name': '" + name + " dynamic_rotation'}";
            channel.sampler = gltf_animation.samplers.size();
            channel.target_node = bone + bone_offset;
            channel.target_path = "rotation";

            gltf_animation.channels.push_back(channel);

            std::vector<glm::quat> dynamic_rotation;
            for(uint32_t sample = 0; sample < dynamic_rotation_samples.size(); sample++) {
                dynamic_rotation.push_back(dynamic_rotation_samples[sample][i]);
            }

            int data_accessor = gltf.accessors.size();
            
            accessor.bufferView = gltf.bufferViews.size();
            accessor.type = TINYGLTF_TYPE_VEC4;
            accessor.count = dynamic_rotation.size();
            accessor.minValues = {};
            accessor.maxValues = {};

            gltf.accessors.push_back(accessor);

            bufferView.byteOffset = offset;
            bufferView.byteLength = sizeof(glm::quat) * dynamic_rotation.size();

            gltf.bufferViews.push_back(bufferView);

            animation_buffer.data.insert(
                animation_buffer.data.end(), 
                (uint8_t*)dynamic_rotation.data(), 
                (uint8_t*)dynamic_rotation.data() + sizeof(glm::quat) * dynamic_rotation.size()
            );

            offset += sizeof(glm::quat) * dynamic_rotation.size();

            tinygltf::AnimationSampler sampler;
            sampler.input = time_accessor;
            sampler.output = data_accessor;

            gltf_animation.samplers.push_back(sampler);
        }
    }

    gltf.animations.push_back(gltf_animation);
    gltf.buffers.push_back(animation_buffer);
}

int main(int argc, const char* argv[]) {
    argparse::ArgumentParser parser("dme_converter", WARPGATE_VERSION);
    int log_level = logger::level::warn;

    build_argument_parser(parser, log_level);

    try {
        parser.parse_args(argc, argv);
    } catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << parser;
        std::exit(1);
    }
    logger::set_level(logger::level::level_enum(log_level));

    std::string input_str = parser.get<std::string>("input_file");
    
    logger::info("Converting file {} using mrn_converter {}", input_str, WARPGATE_VERSION);
    std::string path = parser.get<std::string>("--assets-directory");
    std::filesystem::path server(path);
    std::vector<std::filesystem::path> assets;
    for(int i = 0; i < 24; i++) {
        assets.push_back(server / ("assets_x64_" + std::to_string(i) + ".pack2"));
    }
    
    logger::info("Loading packs...");
    synthium::Manager manager(assets);
    logger::info("Manager loaded.");

    std::filesystem::path input_filename(input_str);
    std::unique_ptr<uint8_t[]> data;
    std::vector<uint8_t> data_vector;
    std::span<uint8_t> data_span;
    if(manager.contains(input_str)) {
        logger::debug("Loading '{}' from manager...", input_str);
        int retries = 3;
        std::shared_ptr<synthium::Asset2> asset = manager.get(input_str);
        //manager.deallocate(asset->uncompressed_size());
        while(data_vector.size() == 0 && retries > 0) {
            try {
                data_vector = asset->get_data();
                data_span = std::span<uint8_t>(data_vector.data(), data_vector.size());
                logger::debug("Loaded '{}' from manager.", input_str);
            } catch(std::bad_alloc) {
                logger::warn("Failed to load asset, deallocating some packs");
                manager.deallocate(asset->uncompressed_size());
            } catch(std::exception &err) {
                logger::error("Failed to load '{}' from manager: {}", input_str, err.what());
                std::exit(1);
            }
            retries--;
        }
    } else {
        logger::debug("Loading '{}' from filesystem...", input_str);
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
        logger::debug("Loaded '{}' from filesystem.", input_str);
    }


    std::filesystem::path output_filename, output_directory;
    std::string format = "";
    if(parser.is_used("--output-file")) {
        output_filename = std::filesystem::weakly_canonical(parser.get<std::string>("--output-file"));
        if(output_filename.has_parent_path()) {
            output_directory = output_filename.parent_path();
        }

        try {
            if(!std::filesystem::exists(output_filename.parent_path())) {
                logger::debug("Creating directories '{}'...", (output_directory).string());
                std::filesystem::create_directories(output_directory);
                logger::debug("Created directories '{}'.", (output_directory).string());
            }
        } catch (std::filesystem::filesystem_error& err) {
            logger::error("Failed to create directory {}: {}", err.path1().string(), err.what());
            std::exit(3);
        }

        format = parser.get<std::string>("--format");
    }

    bool rigify_skeleton = parser.get<bool>("--rigify");
    std::string skeleton_name = parser.get<std::string>("--skeleton");

    mrn::MRN mrn(data_span, input_filename.filename().string());

    std::vector<std::string> skeleton_names = mrn.skeleton_names()->skeleton_names()->strings();
    if(!parser.is_used("--output-file")) {
        logger::level::level_enum old = logger::get_level();
        logger::set_level(logger::level::level_enum::info);

        logger::info("Available skeletons:");
        for(uint32_t i = 0; i < skeleton_names.size(); i++) {
            logger::info("{:>{}}", skeleton_names[i], skeleton_names[i].size() + 4);
        }

        logger::info("Available animations:");
        std::vector<std::string> animation_names = mrn.file_names()->files()->animation_names()->strings();
        for(uint32_t i = 0; i < animation_names.size(); i++) {
            logger::info("{:>{}}", animation_names[i], animation_names[i].size() + 4);
        }

        logger::set_level(old);
        return 0;
    }

    auto skeleton_it = std::find(skeleton_names.begin(), skeleton_names.end(), skeleton_name);
    if(skeleton_it == skeleton_names.end()) {
        logger::error("Skeleton '{}' not found.");
        return 1;
    }

    uint32_t skeleton_index = skeleton_it - skeleton_names.begin();
    tinygltf::Model gltf;
    nlohmann::json remap;

    if(parser.is_used("--remap")) {
        std::ifstream remap_file(parser.get<std::string>("--remap"));
        if(remap_file.fail()) {
            logger::error("Could not open {}", parser.get<std::string>("--remap"));
            std::exit(1);
        }
        remap = nlohmann::json::parse(remap_file);
    }

    std::shared_ptr<mrn::SkeletonData> skeleton_data = static_pointer_cast<mrn::SkeletonPacket>(mrn[mrn.skeleton_indices()[skeleton_index]])->skeleton_data();

    add_skeleton_to_gltf(
        gltf, 
        skeleton_data->skeleton(),
        skeleton_name,
        remap
    );

    std::unordered_set<std::string> exported_animations;
    for(std::string animation : parser.get<std::vector<std::string>>("--animations")) {
        std::regex anim_regex(animation, std::regex_constants::ECMAScript);
        std::vector<std::string> animation_names = mrn.file_names()->files()->animation_names()->strings();
        for(uint32_t i = 0; i < animation_names.size(); i++) {
            if(
                !exported_animations.contains(animation_names[i]) 
                && std::regex_match(animation_names[i], anim_regex)
            ) {
                logger::info("{}: Exporting animation {}...", i, animation_names[i]);
                std::shared_ptr<mrn::NSAFile> nsa_file = static_pointer_cast<mrn::NSAFilePacket>(mrn[i])->animation();
                add_animation_to_gltf(gltf, skeleton_data, nsa_file, animation_names[i]);
                exported_animations.insert(animation_names[i]);
            }
        }
    }

    logger::info("Writing GLTF2 file {}...", output_filename.filename().string());
    tinygltf::TinyGLTF writer;
    writer.WriteGltfSceneToFile(&gltf, output_filename.string(), false, format == "glb", format == "gltf", format == "glb");
    logger::info("Done.");
    return 0;
}