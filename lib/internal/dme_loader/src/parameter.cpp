#include "parameter.h"
#include <string>

using namespace warpgate;

Parameter::Parameter(std::span<uint8_t> subspan): buf_(subspan) {
    buf_ = buf_.first(16 + length());
}

Parameter::ref<Semantic> Parameter::semantic_hash() const {
    return get<Semantic>(0);
}

Parameter::ref<Parameter::D3DXParamClass> Parameter::_class() const {
    return get<D3DXParamClass>(4);
}

Parameter::ref<Parameter::D3DXParamType> Parameter::type() const {
    return get<D3DXParamType>(8);
}

Parameter::ref<uint32_t> Parameter::length() const {
    return get<uint32_t>(12);
}

std::span<uint8_t> Parameter::data() const {
    return buf_.subspan(16, length());
}

uint32_t Parameter::data_offset() const {
    return 16;
}

std::string Parameter::semantic_texture_type(int32_t semantic) {
    switch(Semantic(semantic)) {
    case Semantic::Diffuse:
    case Semantic::BaseDiffuse:
    case Semantic::baseDiffuse:
    case Semantic::diffuseTexture:
    case Semantic::DiffuseB:
        return "Diffuse";
    case Semantic::HoloTexture:
        return "Emissive";
    case Semantic::Bump:
    case Semantic::BumpMap:
        return "Normal";
    case Semantic::BlendMask:
        return "Blend Mask";
    case Semantic::Spec:
    case Semantic::SpecMap:
    case Semantic::SpecGlow:
    case Semantic::SpecB:
        return "Specular";
    case Semantic::detailBump:
    case Semantic::DetailBump:
        return "Detail Cube";
    case Semantic::DetailMask:
        return "Detail Select";
    case Semantic::Overlay:
    case Semantic::Overlay1:
    case Semantic::Overlay2:
    case Semantic::Overlay3:
    case Semantic::Overlay4:
    case Semantic::TilingOverlay:
        return "Overlay";
    case Semantic::DecalTint:
        return "Decal";
    case Semantic::TilingTint:
        return "Tiling Tint";
    default:
        return "Unknown (" + std::to_string(semantic) + ")";
    }
}

std::string Parameter::semantic_texture_type(Semantic semantic) {
    return semantic_texture_type((int32_t)semantic);
}

Parameter::WarpgateSemantic Parameter::texture_common_semantic(int32_t semantic) {
    switch(Semantic(semantic)) {
    case Semantic::Color:
    case Semantic::Color1:
    case Semantic::Color2:
    case Semantic::Diffuse:
    case Semantic::BaseDiffuse:
    case Semantic::baseDiffuse:
    case Semantic::diffuseTexture:
    case Semantic::DiffuseB:
        return Parameter::WarpgateSemantic::DIFFUSE;
    case Semantic::HoloTexture:
        return Parameter::WarpgateSemantic::EMISSIVE;
    case Semantic::Bump:
    case Semantic::BumpMap:
        return Parameter::WarpgateSemantic::NORMAL;
    case Semantic::BlendMask:
        return Parameter::WarpgateSemantic::BLENDMASK;
    case Semantic::Spec:
    case Semantic::SpecMap:
    case Semantic::SpecGlow:
    case Semantic::SpecB:
        return Parameter::WarpgateSemantic::SPECULAR;
    case Semantic::detailBump:
    case Semantic::DetailBump:
        return Parameter::WarpgateSemantic::DETAILCUBE;
    case Semantic::DetailMask:
        return Parameter::WarpgateSemantic::DETAILMASK;
    case Semantic::Overlay:
    case Semantic::Overlay1:
    case Semantic::Overlay2:
    case Semantic::Overlay3:
    case Semantic::Overlay4:
    case Semantic::TilingOverlay:
        return Parameter::WarpgateSemantic::OVERLAY;
    case Semantic::DecalTint:
        return Parameter::WarpgateSemantic::DECAL;
    case Semantic::TilingTint:
        return Parameter::WarpgateSemantic::TILINGTINT;
    default:
        return Parameter::WarpgateSemantic::UNKNOWN;
    }
}

Parameter::WarpgateSemantic Parameter::texture_common_semantic(Semantic semantic) {
    return texture_common_semantic((int32_t)semantic);
}