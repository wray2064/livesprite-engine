// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors
// ls_serialize.cpp — the canonical LiveSprite document format.
//
// The serialized form is the source of truth for a sprite. Three rules hold it
// together:
//   1. Round-trip is lossless: every operation parameter is written and read.
//   2. Fields written by a newer engine survive a load/save cycle here, so an
//      older build cannot silently strip a newer build's data.
//   3. Output is deterministic: sorted keys, fixed number formatting.
//
// Entity ids are file-local. Loading remaps them onto freshly minted ids in the
// receiving context, so two documents can be loaded side by side.

#include "ls_internal.h"
#include "ls_json.h"
#include "ls_reflect.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <type_traits>

namespace ls {
namespace {

constexpr const char* kDocumentTag = "livesprite/document";
constexpr const char* kSpriteTag   = "livesprite/sprite";

// ---------------------------------------------------------------------------
// Value encoding
// ---------------------------------------------------------------------------

json::Value enc(bool value)               { return json::Value{value}; }
json::Value enc(float value)              { return json::Value{static_cast<double>(value)}; }
json::Value enc(double value)             { return json::Value{value}; }
json::Value enc(int32_t value)            { return json::Value{static_cast<double>(value)}; }
json::Value enc(uint32_t value)           { return json::Value{static_cast<double>(value)}; }
json::Value enc(uint64_t value)           { return json::Value{static_cast<double>(value)}; }
json::Value enc(int64_t value)            { return json::Value{static_cast<double>(value)}; }
json::Value enc(const std::string& value) { return json::Value{value}; }

json::Value enc(Vec2f value) {
    json::Value out = json::Value::array();
    out.push(enc(value.x));
    out.push(enc(value.y));
    return out;
}

json::Value enc(Color value) {
    json::Value out = json::Value::array();
    out.push(enc(static_cast<uint32_t>(value.r)));
    out.push(enc(static_cast<uint32_t>(value.g)));
    out.push(enc(static_cast<uint32_t>(value.b)));
    out.push(enc(static_cast<uint32_t>(value.a)));
    return out;
}

json::Value enc(const Mat3f& value) {
    json::Value out = json::Value::array();
    for (float cell : value.m) {
        out.push(enc(cell));
    }
    return out;
}

template<typename Tag>
json::Value enc(TypedId<Tag> value) { return enc(value.value); }

template<typename E, typename std::enable_if<std::is_enum<E>::value, int>::type = 0>
json::Value enc(E value) { return enc(static_cast<uint32_t>(value)); }

// Declared before the template that calls them, and that is not tidiness.
//
// enc(const std::vector<T>&) below calls enc on the element. That call is
// dependent, so a conforming compiler resolves it in two phases: names not
// found by argument-dependent lookup must be visible where the template is
// *defined*, not merely where it is instantiated. These overloads live in an
// anonymous namespace, so ADL on ls::RampStop never reaches them -- and
// without these declarations the definitions further down are invisible.
//
// MSVC accepted it without /permissive-, which is why this survived every
// local build and failed on the first GCC and Clang run.
json::Value enc(const CurveDesc::Segment& segment);
json::Value enc(const RampStop& stop);
json::Value enc(const PluginValue& value);
json::Value enc(const PluginParams& params);
json::Value enc(const IntervalSet& set);

template<typename T>
json::Value enc(const std::vector<T>& values) {
    json::Value out = json::Value::array();
    for (const T& value : values) {
        out.push(enc(value));
    }
    return out;
}

json::Value enc(const CurveDesc::Segment& segment) {
    json::Value out = json::Value::array();
    out.push(enc(segment.p0));
    out.push(enc(segment.cp0));
    out.push(enc(segment.cp1));
    out.push(enc(segment.p1));
    return out;
}

json::Value enc(const RampStop& stop) {
    json::Value out = json::Value::object();
    out["position"] = enc(stop.position);
    out["color"] = enc(stop.color);
    return out;
}

json::Value enc(const PluginValue& value) {
    json::Value out = json::Value::object();
    if (const bool* asBool = std::get_if<bool>(&value)) {
        out["t"] = enc(std::string("b"));  out["v"] = enc(*asBool);
    } else if (const int64_t* asInt = std::get_if<int64_t>(&value)) {
        out["t"] = enc(std::string("i"));  out["v"] = enc(*asInt);
    } else if (const double* asFloat = std::get_if<double>(&value)) {
        out["t"] = enc(std::string("f"));  out["v"] = enc(*asFloat);
    } else if (const std::string* asText = std::get_if<std::string>(&value)) {
        out["t"] = enc(std::string("s"));  out["v"] = enc(*asText);
    } else if (const Vec2f* asVec = std::get_if<Vec2f>(&value)) {
        out["t"] = enc(std::string("v2")); out["v"] = enc(*asVec);
    } else if (const Color* asColor = std::get_if<Color>(&value)) {
        out["t"] = enc(std::string("c"));  out["v"] = enc(*asColor);
    } else if (const uint64_t* asId = std::get_if<uint64_t>(&value)) {
        out["t"] = enc(std::string("id")); out["v"] = enc(*asId);
    }
    return out;
}

json::Value enc(const PluginParams& params) {
    json::Value out = json::Value::object();
    for (const auto& [name, value] : params) {
        out[name] = enc(value);
    }
    return out;
}

json::Value enc(const IntervalSet& set) {
    // Flat triples keep interval sets compact: [y, x0, x1, y, x0, x1, ...]
    json::Value out = json::Value::array();
    for (const Interval& interval : set.intervals) {
        out.push(enc(interval.y));
        out.push(enc(interval.x0));
        out.push(enc(interval.x1));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Value decoding. An id remap table is applied to every typed id so that a
// loaded document lives alongside whatever the context already holds.
// ---------------------------------------------------------------------------

struct DecodeContext {
    const std::map<uint64_t, uint64_t>* remap = nullptr;

    uint64_t mapId(uint64_t fileId) const {
        if (fileId == 0 || remap == nullptr) {
            return fileId;
        }
        auto it = remap->find(fileId);
        return it == remap->end() ? 0 : it->second;
    }
};

void dec(const json::Value& value, const DecodeContext&, bool& out)     { out = value.asBool(out); }
void dec(const json::Value& value, const DecodeContext&, float& out)    { out = static_cast<float>(value.asNumber(out)); }
void dec(const json::Value& value, const DecodeContext&, uint32_t& out) { out = static_cast<uint32_t>(value.asNumber(out)); }
void dec(const json::Value& value, const DecodeContext&, int64_t& out)  { out = static_cast<int64_t>(value.asNumber(static_cast<double>(out))); }
void dec(const json::Value& value, const DecodeContext&, std::string& out) {
    if (value.isString()) {
        out = value.asString();
    }
}

void dec(const json::Value& value, const DecodeContext& ctx, Vec2f& out) {
    if (value.isArray() && value.items().size() >= 2) {
        dec(value.items()[0], ctx, out.x);
        dec(value.items()[1], ctx, out.y);
    }
}

void dec(const json::Value& value, const DecodeContext&, Color& out) {
    if (value.isArray() && value.items().size() >= 4) {
        out.r = static_cast<uint8_t>(value.items()[0].asNumber());
        out.g = static_cast<uint8_t>(value.items()[1].asNumber());
        out.b = static_cast<uint8_t>(value.items()[2].asNumber());
        out.a = static_cast<uint8_t>(value.items()[3].asNumber());
    }
}

void dec(const json::Value& value, const DecodeContext&, Mat3f& out) {
    if (value.isArray() && value.items().size() >= 9) {
        for (size_t i = 0; i < 9; ++i) {
            out.m[i] = static_cast<float>(value.items()[i].asNumber());
        }
    }
}

template<typename Tag>
void dec(const json::Value& value, const DecodeContext& ctx, TypedId<Tag>& out) {
    out.value = ctx.mapId(static_cast<uint64_t>(value.asNumber(0.0)));
}

template<typename E, typename std::enable_if<std::is_enum<E>::value, int>::type = 0>
void dec(const json::Value& value, const DecodeContext&, E& out) {
    out = static_cast<E>(static_cast<uint32_t>(value.asNumber(static_cast<double>(out))));
}

// Declared ahead of the template for the same reason as the enc overloads.
void dec(const json::Value& value, const DecodeContext& ctx, CurveDesc::Segment& out);
void dec(const json::Value& value, const DecodeContext& ctx, RampStop& out);
void dec(const json::Value& value, const DecodeContext& ctx, PluginValue& out);
void dec(const json::Value& value, const DecodeContext& ctx, PluginParams& out);
void dec(const json::Value& value, const DecodeContext&, IntervalSet& out);

template<typename T>
void dec(const json::Value& value, const DecodeContext& ctx, std::vector<T>& out) {
    if (!value.isArray()) {
        return;
    }
    out.clear();
    out.reserve(value.items().size());
    for (const json::Value& item : value.items()) {
        T decoded {};
        dec(item, ctx, decoded);
        out.push_back(std::move(decoded));
    }
}

void dec(const json::Value& value, const DecodeContext& ctx, CurveDesc::Segment& out) {
    if (value.isArray() && value.items().size() >= 4) {
        dec(value.items()[0], ctx, out.p0);
        dec(value.items()[1], ctx, out.cp0);
        dec(value.items()[2], ctx, out.cp1);
        dec(value.items()[3], ctx, out.p1);
    }
}

void dec(const json::Value& value, const DecodeContext& ctx, RampStop& out) {
    if (const json::Value* position = value.find("position")) { dec(*position, ctx, out.position); }
    if (const json::Value* color = value.find("color"))       { dec(*color, ctx, out.color); }
}

void dec(const json::Value& value, const DecodeContext& ctx, PluginValue& out) {
    const json::Value* tag = value.find("t");
    const json::Value* payload = value.find("v");
    if (tag == nullptr || payload == nullptr) {
        return;
    }
    const std::string& type = tag->asString();
    if (type == "b")       { out = payload->asBool(); }
    else if (type == "i")  { int64_t v = 0; dec(*payload, ctx, v); out = v; }
    else if (type == "f")  { out = payload->asNumber(); }
    else if (type == "s")  { out = payload->asString(); }
    else if (type == "v2") { Vec2f v; dec(*payload, ctx, v); out = v; }
    else if (type == "c")  { Color v; dec(*payload, ctx, v); out = v; }
    else if (type == "id") {
        // Plugin ids reference engine entities, so they remap like typed ids.
        out = ctx.mapId(static_cast<uint64_t>(payload->asNumber(0.0)));
    }
}

void dec(const json::Value& value, const DecodeContext& ctx, PluginParams& out) {
    if (!value.isObject()) {
        return;
    }
    out.clear();
    for (const auto& [name, member] : value.members()) {
        PluginValue decoded;
        dec(member, ctx, decoded);
        out[name] = std::move(decoded);
    }
}

void dec(const json::Value& value, const DecodeContext&, IntervalSet& out) {
    out.clear();
    if (!value.isArray()) {
        return;
    }
    const std::vector<json::Value>& items = value.items();
    for (size_t i = 0; i + 2 < items.size(); i += 3) {
        Interval interval;
        interval.y  = static_cast<int32_t>(items[i].asNumber());
        interval.x0 = static_cast<int32_t>(items[i + 1].asNumber());
        interval.x1 = static_cast<int32_t>(items[i + 2].asNumber());
        out.intervals.push_back(interval);
    }
}

// ---------------------------------------------------------------------------
// Archives: one field map serves both directions.
// ---------------------------------------------------------------------------

struct Writer {
    json::Value* obj = nullptr;
    template<typename T>
    void field(const char* name, const T& value) { (*obj)[name] = enc(value); }
};

struct Reader {
    const json::Value* obj = nullptr;
    DecodeContext ctx;
    std::set<std::string>* consumed = nullptr;

    template<typename T>
    void field(const char* name, T& value) {
        if (consumed != nullptr) {
            consumed->insert(name);
        }
        if (const json::Value* member = obj->find(name)) {
            dec(*member, ctx, value);
        }
    }
};

// Keys the archive did not claim came from a newer engine. They are kept
// verbatim so a save from this build does not drop them.
std::string collectUnknown(const json::Value& obj, const std::set<std::string>& consumed) {
    json::Value extras = json::Value::object();
    bool any = false;
    for (const auto& [key, member] : obj.members()) {
        if (consumed.count(key) != 0) {
            continue;
        }
        extras[key] = member;
        any = true;
    }
    return any ? json::dump(extras) : std::string();
}

void mergeUnknown(json::Value& obj, const std::string& stored) {
    if (stored.empty()) {
        return;
    }
    json::Value extras;
    if (!json::parse(stored, extras) || !extras.isObject()) {
        return;
    }
    for (const auto& [key, member] : extras.members()) {
        if (!obj.has(key)) {
            obj[key] = member;
        }
    }
}

json::Value writeOperation(const Operation& operation) {
    json::Value obj = json::Value::object();
    obj["type"] = enc(std::string(operationTypeName(operation)));
    std::visit([&obj](const auto& concrete) {
        using Op = std::decay_t<decltype(concrete)>;
        Op copy = concrete;
        Writer writer{&obj};
        reflect::mapOp(writer, copy);
    }, operation);
    return obj;
}

// Construct the operation named by "type", read its fields, and report which
// keys the schema claimed.
bool readOperation(const json::Value& obj, const DecodeContext& ctx,
                   Operation& out, std::set<std::string>& consumed) {
    const json::Value* type = obj.find("type");
    if (type == nullptr || !type->isString()) {
        return false;
    }
    consumed.insert("type");
    const std::string& name = type->asString();

    // The type roll call lives in ls_operations.h; expanding it here means a
    // new operation cannot be added and silently left unreadable.
    auto tryType = [&](auto prototype) {
        using Op = decltype(prototype);
        if (name != operationTypeName(Operation{Op{}})) {
            return false;
        }
        Op op{};
        Reader reader{&obj, ctx, &consumed};
        reflect::mapOp(reader, op);
        out = op;
        return true;
    };

#define LS_OP_TRY(T) if (tryType(T{})) { return true; }
    LS_OPERATION_TYPES(LS_OP_TRY)
#undef LS_OP_TRY
    return false;
}

json::Value writeGeometry(const GeometryData& data) {
    json::Value obj = json::Value::object();
    std::visit([&obj](const auto& shape) {
        using Shape = std::decay_t<decltype(shape)>;
        Shape copy = shape;
        Writer writer{&obj};
        if constexpr (std::is_same_v<Shape, PointDesc>)         { obj["shape"] = enc(std::string("point")); }
        else if constexpr (std::is_same_v<Shape, LineDesc>)     { obj["shape"] = enc(std::string("line")); }
        else if constexpr (std::is_same_v<Shape, PolylineDesc>) { obj["shape"] = enc(std::string("polyline")); }
        else if constexpr (std::is_same_v<Shape, RectDesc>)     { obj["shape"] = enc(std::string("rect")); }
        else if constexpr (std::is_same_v<Shape, EllipseDesc>)  { obj["shape"] = enc(std::string("ellipse")); }
        else if constexpr (std::is_same_v<Shape, CircleDesc>)   { obj["shape"] = enc(std::string("circle")); }
        else if constexpr (std::is_same_v<Shape, PolygonDesc>)  { obj["shape"] = enc(std::string("polygon")); }
        else                                                    { obj["shape"] = enc(std::string("curve")); }
        reflect::mapShape(writer, copy);
    }, data.shape);
    return obj;
}

bool readGeometryShape(const json::Value& obj, const DecodeContext& ctx,
                       GeometryShape& out, std::set<std::string>& consumed) {
    const json::Value* shape = obj.find("shape");
    if (shape == nullptr || !shape->isString()) {
        return false;
    }
    consumed.insert("shape");
    const std::string& name = shape->asString();
    Reader reader{&obj, ctx, &consumed};

    if (name == "point")    { PointDesc d{};    reflect::mapShape(reader, d); out = d; return true; }
    if (name == "line")     { LineDesc d{};     reflect::mapShape(reader, d); out = d; return true; }
    if (name == "polyline") { PolylineDesc d{}; reflect::mapShape(reader, d); out = d; return true; }
    if (name == "rect")     { RectDesc d{};     reflect::mapShape(reader, d); out = d; return true; }
    if (name == "ellipse")  { EllipseDesc d{};  reflect::mapShape(reader, d); out = d; return true; }
    if (name == "circle")   { CircleDesc d{};   reflect::mapShape(reader, d); out = d; return true; }
    if (name == "polygon")  { PolygonDesc d{};  reflect::mapShape(reader, d); out = d; return true; }
    if (name == "curve")    { CurveDesc d{};    reflect::mapShape(reader, d); out = d; return true; }
    return false;
}

// --- version migration -----------------------------------------------------
//
// A step rewrites a document from one major version to the next. Steps chain,
// so a v1 file reaches v3 by running v1->v2 then v2->v3, and a version with no
// route to the current one is refused rather than half read.
//
// The table is empty because no breaking version has shipped. Adding one means
// writing the rewrite and listing it here; the walk below needs no changes.
struct MigrationStep {
    uint32_t fromMajor = 0;
    uint32_t toMajor = 0;
    bool (*apply)(json::Value& root) = nullptr;
};

const std::vector<MigrationStep>& migrationSteps() {
    static const std::vector<MigrationStep> steps;
    return steps;
}

inline uint32_t majorOf(uint32_t version) { return version >> 16; }

// Walk the chain from one major version to another, rewriting as it goes.
LSError migrateJson(json::Value& root, uint32_t from, uint32_t to) {
    uint32_t current = majorOf(from);
    const uint32_t target = majorOf(to);
    if (current == target) {
        return LSError::None;
    }
    if (current > target) {
        // Downgrading would have to discard whatever the newer version added.
        return LSError::VersionMigrationFailed;
    }

    for (int guard = 0; guard < 64 && current != target; ++guard) {
        const MigrationStep* next = nullptr;
        for (const MigrationStep& step : migrationSteps()) {
            if (step.fromMajor == current) {
                next = &step;
                break;
            }
        }
        if (next == nullptr || next->apply == nullptr) {
            return LSError::VersionMigrationFailed;
        }
        if (!next->apply(root)) {
            return LSError::VersionMigrationFailed;
        }
        current = next->toMajor;
    }

    return current == target ? LSError::None : LSError::VersionMigrationFailed;
}

std::vector<uint8_t> toBytes(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::string fromBytes(const std::vector<uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

} // namespace

// ---------------------------------------------------------------------------
// Document serialization
// ---------------------------------------------------------------------------

namespace {

json::Value writeSprite(const LSContext::Impl& impl, SpriteId spriteId, const SpriteData& sprite);

json::Value writeLayer(const LSContext::Impl& impl, LayerId layerId, const LayerData& layer) {
    json::Value obj = json::Value::object();
    obj["id"] = enc(layerId);
    obj["name"] = enc(layer.desc.name);
    obj["layerType"] = enc(layer.desc.type);
    obj["opacity"] = enc(layer.desc.opacity);
    obj["blend"] = enc(layer.desc.blend);
    obj["visible"] = enc(layer.desc.visible);
    obj["mask"] = enc(layer.mask);
    obj["clipBase"] = enc(layer.clipBase);
    obj["parent"] = enc(layer.parent);

    json::Value operations = json::Value::array();
    for (OperationId opId : layer.operations) {
        const OperationData* data = impl.findOperation(opId);
        if (data == nullptr) {
            continue;
        }
        json::Value opObj = writeOperation(data->op);
        opObj["id"] = enc(opId);
        opObj["boundary"] = enc(data->assignedBoundary);
        auto unknown = impl.unknownFields.find(opId.value);
        if (unknown != impl.unknownFields.end()) {
            mergeUnknown(opObj, unknown->second);
        }
        operations.push(std::move(opObj));
    }
    obj["operations"] = std::move(operations);

    auto unknown = impl.unknownFields.find(layerId.value);
    if (unknown != impl.unknownFields.end()) {
        mergeUnknown(obj, unknown->second);
    }
    return obj;
}

json::Value writeSprite(const LSContext::Impl& impl, SpriteId spriteId, const SpriteData& sprite) {
    json::Value obj = json::Value::object();
    obj["id"] = enc(spriteId);
    obj["palette"] = enc(sprite.palette);
    obj["pivot"] = enc(sprite.pivot);
    obj["transform"] = enc(sprite.transform);
    if (sprite.attached) {
        json::Value attachment = json::Value::object();
        attachment["socket"] = enc(sprite.attachment.socket);
        attachment["childPivot"] = enc(sprite.attachment.childPivot);
        attachment["localOffset"] = enc(sprite.attachment.localOffset);
        attachment["behindParent"] = enc(sprite.attachment.behindParent);
        obj["attachment"] = std::move(attachment);
    }

    json::Value pivots = json::Value::array();
    for (PivotId pivotId : sprite.pivots) {
        const PivotData* data = impl.findPivot(pivotId);
        if (data == nullptr) {
            continue;
        }
        json::Value pivotObj = json::Value::object();
        pivotObj["id"] = enc(pivotId);
        pivotObj["position"] = enc(data->position);
        pivotObj["name"] = enc(data->name);
        pivots.push(std::move(pivotObj));
    }
    obj["pivots"] = std::move(pivots);

    json::Value sockets = json::Value::array();
    for (SocketId socketId : sprite.sockets) {
        const SocketData* data = impl.findSocket(socketId);
        if (data == nullptr) {
            continue;
        }
        json::Value socketObj = json::Value::object();
        socketObj["id"] = enc(socketId);
        socketObj["name"] = enc(data->desc.name);
        socketObj["position"] = enc(data->desc.position);
        socketObj["angle"] = enc(data->desc.angle);
        socketObj["scale"] = enc(data->desc.scale);
        sockets.push(std::move(socketObj));
    }
    obj["sockets"] = std::move(sockets);

    json::Value boundaries = json::Value::array();
    for (BoundaryId boundaryId : sprite.boundaries) {
        const BoundaryData* data = impl.findBoundary(boundaryId);
        if (data == nullptr) {
            continue;
        }
        json::Value boundaryObj = json::Value::object();
        boundaryObj["id"] = enc(boundaryId);
        boundaryObj["name"] = enc(data->desc.name);
        boundaryObj["shape"] = enc(data->desc.shape);
        boundaryObj["falloff"] = enc(data->desc.falloff);
        boundaryObj["falloffWidth"] = enc(data->desc.falloffWidth);
        boundaries.push(std::move(boundaryObj));
    }
    obj["boundaries"] = std::move(boundaries);

    json::Value groups = json::Value::array();
    for (GroupId groupId : sprite.groups) {
        const GroupData* data = impl.findGroup(groupId);
        if (data == nullptr) {
            continue;
        }
        json::Value groupObj = json::Value::object();
        groupObj["id"] = enc(groupId);
        groupObj["name"] = enc(data->desc.name);
        groupObj["opacity"] = enc(data->desc.opacity);
        groupObj["blend"] = enc(data->desc.blend);
        groupObj["visible"] = enc(data->desc.visible);
        groupObj["layers"] = enc(data->layers);
        groups.push(std::move(groupObj));
    }
    obj["groups"] = std::move(groups);

    json::Value layers = json::Value::array();
    for (LayerId layerId : sprite.layers) {
        const LayerData* data = impl.findLayer(layerId);
        if (data != nullptr) {
            layers.push(writeLayer(impl, layerId, *data));
        }
    }
    obj["layers"] = std::move(layers);

    auto unknown = impl.unknownFields.find(spriteId.value);
    if (unknown != impl.unknownFields.end()) {
        mergeUnknown(obj, unknown->second);
    }
    return obj;
}

json::Value writeDocumentBody(const LSContext::Impl& impl, DocumentId docId,
                              const DocumentData& doc, const std::vector<SpriteId>& sprites) {
    json::Value root = json::Value::object();
    root["format"] = enc(std::string(kDocumentTag));
    root["engineVersion"] = enc(LS_ENGINE_VERSION);

    json::Value document = json::Value::object();
    document["id"] = enc(docId);
    document["name"] = enc(doc.name);
    document["canvasWidth"] = enc(doc.canvasWidth);
    document["canvasHeight"] = enc(doc.canvasHeight);
    document["palette"] = enc(doc.palette);
    root["document"] = std::move(document);

    json::Value geometry = json::Value::array();
    for (GeometryId geometryId : doc.geometry) {
        const GeometryData* data = impl.findGeometry(geometryId);
        if (data == nullptr) {
            continue;
        }
        json::Value obj = writeGeometry(*data);
        obj["id"] = enc(geometryId);
        auto unknown = impl.unknownFields.find(geometryId.value);
        if (unknown != impl.unknownFields.end()) {
            mergeUnknown(obj, unknown->second);
        }
        geometry.push(std::move(obj));
    }
    root["geometry"] = std::move(geometry);

    json::Value regions = json::Value::array();
    for (RegionId regionId : doc.regions) {
        const RegionData* data = impl.findRegion(regionId);
        if (data == nullptr) {
            continue;
        }
        json::Value obj = json::Value::object();
        obj["id"] = enc(regionId);
        obj["source"] = enc(data->source);
        obj["coverage"] = enc(data->coverage);
        obj["boundary"] = enc(data->boundary);
        obj["role"] = enc(data->role);
        regions.push(std::move(obj));
    }
    root["regions"] = std::move(regions);

    json::Value palettes = json::Value::array();
    for (PaletteId paletteId : doc.palettes) {
        const PaletteData* data = impl.findPalette(paletteId);
        if (data == nullptr) {
            continue;
        }
        json::Value obj = json::Value::object();
        obj["id"] = enc(paletteId);
        obj["name"] = enc(data->name);
        json::Value entries = json::Value::array();
        for (const auto& [role, color] : data->colors) {
            json::Value entry = json::Value::object();
            entry["role"] = enc(role);
            entry["color"] = enc(color);
            auto label = data->labels.find(role);
            if (label != data->labels.end()) {
                entry["label"] = enc(label->second);
            }
            entries.push(std::move(entry));
        }
        obj["entries"] = std::move(entries);
        palettes.push(std::move(obj));
    }
    root["palettes"] = std::move(palettes);

    json::Value ramps = json::Value::array();
    for (RampId rampId : doc.ramps) {
        const RampData* data = impl.findRamp(rampId);
        if (data == nullptr) {
            continue;
        }
        json::Value obj = json::Value::object();
        obj["id"] = enc(rampId);
        obj["name"] = enc(data->desc.name);
        obj["stops"] = enc(data->desc.stops);
        obj["interpolate"] = enc(data->desc.interpolate);
        ramps.push(std::move(obj));
    }
    root["ramps"] = std::move(ramps);

    json::Value patterns = json::Value::array();
    for (PatternId patternId : doc.patterns) {
        const PatternData* data = impl.findPattern(patternId);
        if (data == nullptr) {
            continue;
        }
        json::Value obj = json::Value::object();
        obj["id"] = enc(patternId);
        obj["name"] = enc(data->desc.name);
        obj["tileWidth"] = enc(data->desc.tileWidth);
        obj["tileHeight"] = enc(data->desc.tileHeight);
        obj["levels"] = enc(data->desc.levels);
        obj["phase"] = enc(data->desc.phase);
        obj["density"] = enc(data->desc.density);
        obj["coordinateSpace"] = enc(data->desc.coordinateSpace);
        json::Value mask = json::Value::array();
        for (uint8_t cell : data->desc.mask) {
            mask.push(enc(static_cast<uint32_t>(cell)));
        }
        obj["mask"] = std::move(mask);
        if (!data->desc.colors.empty()) {
            obj["colors"] = enc(data->desc.colors);
        }
        patterns.push(std::move(obj));
    }
    root["patterns"] = std::move(patterns);

    json::Value spriteArray = json::Value::array();
    for (SpriteId spriteId : sprites) {
        const SpriteData* data = impl.findSprite(spriteId);
        if (data != nullptr) {
            spriteArray.push(writeSprite(impl, spriteId, *data));
        }
    }
    root["sprites"] = std::move(spriteArray);

    // App metadata for everything in this document. Values are opaque: the
    // engine writes back exactly what it was handed.
    json::Value metadata = json::Value::object();
    auto includeMetadata = [&](uint64_t entityId) {
        auto entry = impl.metadata.find(entityId);
        if (entry == impl.metadata.end() || entry->second.empty()) {
            return;
        }
        json::Value entity = json::Value::object();
        for (const auto& [key, value] : entry->second) {
            entity[key] = enc(value);
        }
        metadata[std::to_string(entityId)] = std::move(entity);
    };

    includeMetadata(docId.value);
    for (GeometryId id : doc.geometry)  { includeMetadata(id.value); }
    for (RegionId id : doc.regions)     { includeMetadata(id.value); }
    for (PaletteId id : doc.palettes)   { includeMetadata(id.value); }
    for (RampId id : doc.ramps)         { includeMetadata(id.value); }
    for (PatternId id : doc.patterns)   { includeMetadata(id.value); }
    for (SpriteId spriteId : sprites) {
        includeMetadata(spriteId.value);
        const SpriteData* sprite = impl.findSprite(spriteId);
        if (sprite == nullptr) {
            continue;
        }
        for (SocketId id : sprite->sockets)      { includeMetadata(id.value); }
        for (BoundaryId id : sprite->boundaries) { includeMetadata(id.value); }
        for (PivotId id : sprite->pivots)        { includeMetadata(id.value); }
        for (GroupId id : sprite->groups)        { includeMetadata(id.value); }
        for (LayerId layerId : sprite->layers) {
            includeMetadata(layerId.value);
            const LayerData* layer = impl.findLayer(layerId);
            if (layer == nullptr) {
                continue;
            }
            for (OperationId id : layer->operations) {
                includeMetadata(id.value);
            }
        }
    }
    if (!metadata.members().empty()) {
        root["metadata"] = std::move(metadata);
    }

    auto unknown = impl.unknownFields.find(docId.value);
    if (unknown != impl.unknownFields.end()) {
        mergeUnknown(root, unknown->second);
    }
    return root;
}

// Collect every id the file mentions as an entity so references can be remapped
// before any of them are decoded.
void collectIds(const json::Value& root, std::vector<uint64_t>& out) {
    auto pushId = [&out](const json::Value& obj) {
        if (const json::Value* id = obj.find("id")) {
            out.push_back(static_cast<uint64_t>(id->asNumber()));
        }
    };

    if (const json::Value* document = root.find("document")) {
        pushId(*document);
    }
    for (const char* key : {"geometry", "regions", "palettes", "ramps", "patterns"}) {
        if (const json::Value* list = root.find(key)) {
            for (const json::Value& item : list->items()) {
                pushId(item);
            }
        }
    }
    if (const json::Value* sprites = root.find("sprites")) {
        for (const json::Value& sprite : sprites->items()) {
            pushId(sprite);
            for (const char* key : {"pivots", "sockets", "boundaries", "groups"}) {
                if (const json::Value* list = sprite.find(key)) {
                    for (const json::Value& item : list->items()) {
                        pushId(item);
                    }
                }
            }
            if (const json::Value* layers = sprite.find("layers")) {
                for (const json::Value& layer : layers->items()) {
                    pushId(layer);
                    if (const json::Value* operations = layer.find("operations")) {
                        for (const json::Value& operation : operations->items()) {
                            pushId(operation);
                        }
                    }
                }
            }
        }
    }
}

} // namespace

Result<SerializedData> LSContext::serializeDocument(DocumentId doc) const {
    const DocumentData* document = impl_->findDocument(doc);
    if (document == nullptr) {
        return Result<SerializedData>::err(LSError::InvalidId);
    }

    const json::Value root = writeDocumentBody(*impl_, doc, *document, document->sprites);
    SerializedData data;
    data.bytes = toBytes(json::dump(root));
    data.engineVersion = LS_ENGINE_VERSION;
    data.formatTag = kDocumentTag;
    return Result<SerializedData>::ok(std::move(data));
}

Result<SerializedData> LSContext::serializeSprite(SpriteId id) const {
    const SpriteData* sprite = impl_->findSprite(id);
    if (sprite == nullptr) {
        return Result<SerializedData>::err(LSError::InvalidId);
    }
    const DocumentData* document = impl_->findDocument(sprite->document);
    if (document == nullptr) {
        return Result<SerializedData>::err(LSError::InvalidId);
    }

    // A sprite travels with the document resources it references, so it can be
    // loaded into another document without losing its regions or palette.
    json::Value root = writeDocumentBody(*impl_, sprite->document, *document, { id });
    root["format"] = enc(std::string(kSpriteTag));

    SerializedData data;
    data.bytes = toBytes(json::dump(root));
    data.engineVersion = LS_ENGINE_VERSION;
    data.formatTag = kSpriteTag;
    return Result<SerializedData>::ok(std::move(data));
}

namespace {

// Shared loader for documents and sprites. Returns the new document id and the
// ids of the sprites it created, in file order.
Result<DocumentId> loadDocument(LSContext::Impl& impl, const SerializedData& data,
                                std::vector<SpriteId>* createdSprites,
                                bool preserveIds = false) {
    if ((LS_ENGINE_VERSION >> 16) < (data.engineVersion >> 16)) {
        // A newer major version can change the meaning of existing fields.
        return Result<DocumentId>::err(LSError::VersionMismatch);
    }

    json::Value root;
    if (!json::parse(fromBytes(data.bytes), root) || !root.isObject()) {
        return Result<DocumentId>::err(LSError::DeserializationFailure);
    }

    // An older major is brought forward through the migration chain first, so
    // the reader below only ever sees the current schema.
    const LSError migrated = migrateJson(root, data.engineVersion, LS_ENGINE_VERSION);
    if (migrated != LSError::None) {
        return Result<DocumentId>::err(migrated);
    }
    const json::Value* documentObj = root.find("document");
    if (documentObj == nullptr || !documentObj->isObject()) {
        return Result<DocumentId>::err(LSError::DeserializationFailure);
    }

    std::vector<uint64_t> fileIds;
    collectIds(root, fileIds);
    std::map<uint64_t, uint64_t> remap;
    for (uint64_t fileId : fileIds) {
        if (fileId == 0 || remap.find(fileId) != remap.end()) {
            continue;
        }
        // Restoring a snapshot puts the ids back as they were, so every handle
        // the caller is holding still points at the same thing. Loading a file
        // mints fresh ones, so two documents can be open at once.
        remap[fileId] = preserveIds ? fileId : impl.nextId++;
        if (preserveIds) {
            impl.nextId = std::max(impl.nextId, fileId + 1);
        }
    }
    DecodeContext ctx;
    ctx.remap = &remap;

    auto mapped = [&remap](const json::Value& obj) -> uint64_t {
        const json::Value* id = obj.find("id");
        if (id == nullptr) {
            return 0;
        }
        auto it = remap.find(static_cast<uint64_t>(id->asNumber()));
        return it == remap.end() ? 0 : it->second;
    };

    const uint64_t docId = mapped(*documentObj);
    if (docId == 0) {
        return Result<DocumentId>::err(LSError::DeserializationFailure);
    }

    DocumentData document;
    {
        std::set<std::string> consumed {"id"};
        Reader reader{documentObj, ctx, &consumed};
        reader.field("name", document.name);
        reader.field("canvasWidth", document.canvasWidth);
        reader.field("canvasHeight", document.canvasHeight);
        reader.field("palette", document.palette);
        const std::string unknown = collectUnknown(*documentObj, consumed);
        if (!unknown.empty()) {
            impl.unknownFields[docId] = unknown;
        }
    }
    // This is the path that matters: the canvas size came out of a file, and the
    // file came from whoever sent it. A document declaring 65535 x 65535 would
    // otherwise have the reader hand back something no compile can survive.
    //
    // Checked against this context's own limits, so an application willing to
    // work larger can also open larger files, and one that has tightened them
    // will not be handed a document it cannot display.
    if (!canvasSizeIsUsable(document.canvasWidth, document.canvasHeight,
                            impl.canvasLimits)) {
        return Result<DocumentId>::err(LSError::DeserializationFailure);
    }

    // Top-level keys the schema does not know are kept against the document.
    {
        std::set<std::string> consumed {
            "format", "engineVersion", "document", "geometry", "regions",
            "palettes", "ramps", "patterns", "sprites", "metadata"
        };
        const std::string unknown = collectUnknown(root, consumed);
        if (!unknown.empty()) {
            json::Value merged = json::Value::object();
            auto existing = impl.unknownFields.find(docId);
            if (existing != impl.unknownFields.end()) {
                json::parse(existing->second, merged);
            }
            json::Value extras;
            if (json::parse(unknown, extras) && extras.isObject()) {
                for (const auto& [key, member] : extras.members()) {
                    merged[key] = member;
                }
            }
            impl.unknownFields[docId] = json::dump(merged);
        }
    }

    impl.documents.emplace(docId, document);
    impl.documentOrder.push_back(DocumentId{docId});
    DocumentData& stored = impl.documents.at(docId);

    // --- geometry ---------------------------------------------------------
    if (const json::Value* list = root.find("geometry")) {
        for (const json::Value& item : list->items()) {
            const uint64_t id = mapped(item);
            if (id == 0) {
                continue;
            }
            GeometryData geometry;
            geometry.document = DocumentId{docId};
            std::set<std::string> consumed {"id"};
            if (!readGeometryShape(item, ctx, geometry.shape, consumed)) {
                return Result<DocumentId>::err(LSError::DeserializationFailure);
            }
            const std::string unknown = collectUnknown(item, consumed);
            if (!unknown.empty()) {
                impl.unknownFields[id] = unknown;
            }
            impl.geometry.emplace(id, std::move(geometry));
            stored.geometry.push_back(GeometryId{id});
        }
    }

    // --- regions ----------------------------------------------------------
    if (const json::Value* list = root.find("regions")) {
        for (const json::Value& item : list->items()) {
            const uint64_t id = mapped(item);
            if (id == 0) {
                continue;
            }
            RegionData region;
            region.document = DocumentId{docId};
            std::set<std::string> consumed {"id"};
            Reader reader{&item, ctx, &consumed};
            reader.field("source", region.source);
            reader.field("coverage", region.coverage);
            reader.field("boundary", region.boundary);
            reader.field("role", region.role);
            const std::string unknown = collectUnknown(item, consumed);
            if (!unknown.empty()) {
                impl.unknownFields[id] = unknown;
            }
            if (region.source.valid()) {
                impl.addDependencyEdge(region.source.value, id);
            }
            impl.regions.emplace(id, std::move(region));
            stored.regions.push_back(RegionId{id});
        }
    }

    // --- palettes ---------------------------------------------------------
    if (const json::Value* list = root.find("palettes")) {
        for (const json::Value& item : list->items()) {
            const uint64_t id = mapped(item);
            if (id == 0) {
                continue;
            }
            PaletteData palette;
            palette.document = DocumentId{docId};
            if (const json::Value* name = item.find("name")) {
                palette.name = name->asString();
            }
            if (const json::Value* entries = item.find("entries")) {
                for (const json::Value& entry : entries->items()) {
                    ColorRole role = kColorRoleNone;
                    Color color;
                    if (const json::Value* roleValue = entry.find("role")) {
                        dec(*roleValue, ctx, role);
                    }
                    if (const json::Value* colorValue = entry.find("color")) {
                        dec(*colorValue, ctx, color);
                    }
                    if (role == kColorRoleNone) {
                        continue;
                    }
                    palette.colors[role] = color;
                    if (const json::Value* label = entry.find("label")) {
                        palette.labels[role] = label->asString();
                    }
                }
            }
            impl.palettes.emplace(id, std::move(palette));
            stored.palettes.push_back(PaletteId{id});
        }
    }

    // --- ramps ------------------------------------------------------------
    if (const json::Value* list = root.find("ramps")) {
        for (const json::Value& item : list->items()) {
            const uint64_t id = mapped(item);
            if (id == 0) {
                continue;
            }
            RampData ramp;
            ramp.document = DocumentId{docId};
            std::set<std::string> consumed {"id"};
            Reader reader{&item, ctx, &consumed};
            reader.field("name", ramp.desc.name);
            reader.field("stops", ramp.desc.stops);
            reader.field("interpolate", ramp.desc.interpolate);
            impl.ramps.emplace(id, std::move(ramp));
            stored.ramps.push_back(RampId{id});
        }
    }

    // --- patterns ---------------------------------------------------------
    if (const json::Value* list = root.find("patterns")) {
        for (const json::Value& item : list->items()) {
            const uint64_t id = mapped(item);
            if (id == 0) {
                continue;
            }
            PatternData pattern;
            pattern.document = DocumentId{docId};
            std::set<std::string> consumed {"id"};
            Reader reader{&item, ctx, &consumed};
            reader.field("name", pattern.desc.name);
            reader.field("tileWidth", pattern.desc.tileWidth);
            reader.field("tileHeight", pattern.desc.tileHeight);
            reader.field("levels", pattern.desc.levels);
            reader.field("phase", pattern.desc.phase);
            reader.field("density", pattern.desc.density);
            reader.field("coordinateSpace", pattern.desc.coordinateSpace);
            std::vector<uint32_t> mask;
            reader.field("mask", mask);
            pattern.desc.mask.clear();
            pattern.desc.mask.reserve(mask.size());
            for (uint32_t cell : mask) {
                pattern.desc.mask.push_back(static_cast<uint8_t>(cell));
            }
            reader.field("colors", pattern.desc.colors);
            impl.patterns.emplace(id, std::move(pattern));
            stored.patterns.push_back(PatternId{id});
        }
    }

    // --- sprites ----------------------------------------------------------
    if (const json::Value* sprites = root.find("sprites")) {
        for (const json::Value& spriteObj : sprites->items()) {
            const uint64_t spriteId = mapped(spriteObj);
            if (spriteId == 0) {
                continue;
            }
            SpriteData sprite;
            sprite.document = DocumentId{docId};
            std::set<std::string> consumed {
                "id", "pivots", "sockets", "boundaries", "groups", "layers", "attachment"
            };
            {
                Reader reader{&spriteObj, ctx, &consumed};
                reader.field("palette", sprite.palette);
                reader.field("pivot", sprite.pivot);
                reader.field("transform", sprite.transform);
            }
            if (const json::Value* attachment = spriteObj.find("attachment")) {
                Reader reader{attachment, ctx, nullptr};
                reader.field("socket", sprite.attachment.socket);
                reader.field("childPivot", sprite.attachment.childPivot);
                reader.field("localOffset", sprite.attachment.localOffset);
                reader.field("behindParent", sprite.attachment.behindParent);
                sprite.attached = sprite.attachment.socket.valid();
            }

            if (const json::Value* list = spriteObj.find("pivots")) {
                for (const json::Value& item : list->items()) {
                    const uint64_t id = mapped(item);
                    if (id == 0) {
                        continue;
                    }
                    PivotData pivot;
                    pivot.sprite = SpriteId{spriteId};
                    if (const json::Value* position = item.find("position")) {
                        dec(*position, ctx, pivot.position);
                    }
                    if (const json::Value* pivotName = item.find("name")) {
                        pivot.name = pivotName->asString();
                    }
                    impl.pivots.emplace(id, pivot);
                    sprite.pivots.push_back(PivotId{id});
                }
            }
            if (const json::Value* list = spriteObj.find("sockets")) {
                for (const json::Value& item : list->items()) {
                    const uint64_t id = mapped(item);
                    if (id == 0) {
                        continue;
                    }
                    SocketData socket;
                    socket.sprite = SpriteId{spriteId};
                    std::set<std::string> socketConsumed {"id"};
                    Reader reader{&item, ctx, &socketConsumed};
                    reader.field("name", socket.desc.name);
                    reader.field("position", socket.desc.position);
                    reader.field("angle", socket.desc.angle);
                    reader.field("scale", socket.desc.scale);
                    impl.sockets.emplace(id, std::move(socket));
                    sprite.sockets.push_back(SocketId{id});
                }
            }
            if (const json::Value* list = spriteObj.find("boundaries")) {
                for (const json::Value& item : list->items()) {
                    const uint64_t id = mapped(item);
                    if (id == 0) {
                        continue;
                    }
                    BoundaryData boundary;
                    boundary.sprite = SpriteId{spriteId};
                    std::set<std::string> boundaryConsumed {"id"};
                    Reader reader{&item, ctx, &boundaryConsumed};
                    reader.field("name", boundary.desc.name);
                    reader.field("shape", boundary.desc.shape);
                    reader.field("falloff", boundary.desc.falloff);
                    reader.field("falloffWidth", boundary.desc.falloffWidth);
                    if (boundary.desc.shape.valid()) {
                        impl.addDependencyEdge(boundary.desc.shape.value, id);
                    }
                    impl.boundaries.emplace(id, std::move(boundary));
                    sprite.boundaries.push_back(BoundaryId{id});
                }
            }
            if (const json::Value* list = spriteObj.find("groups")) {
                for (const json::Value& item : list->items()) {
                    const uint64_t id = mapped(item);
                    if (id == 0) {
                        continue;
                    }
                    GroupData group;
                    group.sprite = SpriteId{spriteId};
                    std::set<std::string> groupConsumed {"id"};
                    Reader reader{&item, ctx, &groupConsumed};
                    reader.field("name", group.desc.name);
                    reader.field("opacity", group.desc.opacity);
                    reader.field("blend", group.desc.blend);
                    reader.field("visible", group.desc.visible);
                    reader.field("layers", group.layers);
                    impl.groups.emplace(id, std::move(group));
                    sprite.groups.push_back(GroupId{id});
                }
            }

            if (const json::Value* layers = spriteObj.find("layers")) {
                for (const json::Value& layerObj : layers->items()) {
                    const uint64_t layerId = mapped(layerObj);
                    if (layerId == 0) {
                        continue;
                    }
                    LayerData layer;
                    layer.sprite = SpriteId{spriteId};
                    std::set<std::string> layerConsumed {"id", "operations"};
                    {
                        Reader reader{&layerObj, ctx, &layerConsumed};
                        reader.field("name", layer.desc.name);
                        reader.field("layerType", layer.desc.type);
                        reader.field("opacity", layer.desc.opacity);
                        reader.field("blend", layer.desc.blend);
                        reader.field("visible", layer.desc.visible);
                        reader.field("mask", layer.mask);
                        reader.field("clipBase", layer.clipBase);
                        reader.field("parent", layer.parent);
                    }
                    const std::string layerUnknown = collectUnknown(layerObj, layerConsumed);
                    if (!layerUnknown.empty()) {
                        impl.unknownFields[layerId] = layerUnknown;
                    }

                    if (const json::Value* operations = layerObj.find("operations")) {
                        for (const json::Value& opObj : operations->items()) {
                            const uint64_t opId = mapped(opObj);
                            if (opId == 0) {
                                continue;
                            }
                            OperationData record;
                            record.layer = LayerId{layerId};
                            std::set<std::string> opConsumed {"id", "boundary"};
                            if (!readOperation(opObj, ctx, record.op, opConsumed)) {
                                return Result<DocumentId>::err(LSError::DeserializationFailure);
                            }
                            if (const json::Value* boundary = opObj.find("boundary")) {
                                dec(*boundary, ctx, record.assignedBoundary);
                            }
                            const std::string opUnknown = collectUnknown(opObj, opConsumed);
                            if (!opUnknown.empty()) {
                                impl.unknownFields[opId] = opUnknown;
                            }
                            impl.operations.emplace(opId, std::move(record));
                            layer.operations.push_back(OperationId{opId});
                            impl.registerOperationDependencies(OperationId{opId});
                            impl.addDependencyEdge(opId, layerId);
                        }
                    }

                    if (layer.mask.valid()) {
                        impl.addDependencyEdge(layer.mask.value, layerId);
                    }
                    impl.layers.emplace(layerId, std::move(layer));
                    sprite.layers.push_back(LayerId{layerId});
                    impl.addDependencyEdge(layerId, spriteId);
                }
            }

            const std::string spriteUnknown = collectUnknown(spriteObj, consumed);
            if (!spriteUnknown.empty()) {
                impl.unknownFields[spriteId] = spriteUnknown;
            }
            if (sprite.palette.valid()) {
                impl.addDependencyEdge(sprite.palette.value, spriteId);
            }
            impl.sprites.emplace(spriteId, std::move(sprite));
            stored.sprites.push_back(SpriteId{spriteId});
            impl.addDependencyEdge(spriteId, docId);
            if (createdSprites != nullptr) {
                createdSprites->push_back(SpriteId{spriteId});
            }
        }
    }

    if (const json::Value* metadata = root.find("metadata")) {
        for (const auto& [entityText, entity] : metadata->members()) {
            if (!entity.isObject()) {
                continue;
            }
            const uint64_t fileId = std::strtoull(entityText.c_str(), nullptr, 10);
            const uint64_t owner = ctx.mapId(fileId);
            if (owner == 0) {
                continue;   // metadata for an entity this file no longer has
            }
            for (const auto& [key, value] : entity.members()) {
                if (!value.isString() || key.size() > kMetadataMaxKeyLength ||
                    value.asString().size() > kMetadataMaxValueLength) {
                    continue;   // a value outside the limits is dropped, not trusted
                }
                auto& entries = impl.metadata[owner];
                if (entries.size() < kMetadataMaxPerEntity || entries.count(key) != 0) {
                    entries[key] = value.asString();
                }
            }
        }
    }

    ++impl.resourceRevision;
    return Result<DocumentId>::ok(DocumentId{docId});
}

} // namespace

Result<DocumentId> LSContext::deserializeDocument(const SerializedData& data) {
    if (!data.formatTag.empty() && data.formatTag != kDocumentTag && data.formatTag != kSpriteTag) {
        return Result<DocumentId>::err(LSError::DeserializationFailure);
    }
    return loadDocument(*impl_, data, nullptr);
}

Result<SpriteId> LSContext::deserializeSprite(DocumentId into, const SerializedData& data) {
    if (impl_->findDocument(into) == nullptr) {
        return Result<SpriteId>::err(LSError::InvalidId);
    }

    // The payload carries its own resources; load it, then move the sprite and
    // everything it references into the destination document.
    std::vector<SpriteId> created;
    auto loaded = loadDocument(*impl_, data, &created);
    if (loaded.fail()) {
        return Result<SpriteId>::err(loaded.error);
    }
    if (created.empty()) {
        deleteDocument(loaded.value);
        return Result<SpriteId>::err(LSError::DeserializationFailure);
    }

    DocumentData* source = impl_->findDocument(loaded.value);
    DocumentData* target = impl_->findDocument(into);
    if (source == nullptr || target == nullptr) {
        return Result<SpriteId>::err(LSError::InvalidId);
    }

    const SpriteId spriteId = created.front();
    for (GeometryId id : source->geometry) {
        if (GeometryData* geometry = impl_->findGeometry(id)) {
            geometry->document = into;
        }
        target->geometry.push_back(id);
    }
    for (RegionId id : source->regions) {
        if (RegionData* region = impl_->findRegion(id)) {
            region->document = into;
        }
        target->regions.push_back(id);
    }
    for (PaletteId id : source->palettes) {
        if (PaletteData* palette = impl_->findPalette(id)) {
            palette->document = into;
        }
        target->palettes.push_back(id);
    }
    for (RampId id : source->ramps) {
        if (RampData* ramp = impl_->findRamp(id)) {
            ramp->document = into;
        }
        target->ramps.push_back(id);
    }
    for (PatternId id : source->patterns) {
        if (PatternData* pattern = impl_->findPattern(id)) {
            pattern->document = into;
        }
        target->patterns.push_back(id);
    }
    for (SpriteId id : source->sprites) {
        if (SpriteData* sprite = impl_->findSprite(id)) {
            sprite->document = into;
        }
        target->sprites.push_back(id);
        impl_->addDependencyEdge(id.value, into.value);
    }

    // The staging document handed everything over; drop the empty shell.
    source->geometry.clear();
    source->regions.clear();
    source->palettes.clear();
    source->ramps.clear();
    source->patterns.clear();
    source->sprites.clear();
    impl_->documents.erase(loaded.value.value);
    impl_->documentOrder.erase(
        std::remove(impl_->documentOrder.begin(), impl_->documentOrder.end(), loaded.value),
        impl_->documentOrder.end());

    return Result<SpriteId>::ok(spriteId);
}

Result<std::string> LSContext::serializeOperation(OperationId id) const {
    const OperationData* data = impl_->findOperation(id);
    if (data == nullptr) {
        return Result<std::string>::err(LSError::InvalidId);
    }
    json::Value obj = writeOperation(data->op);
    obj["id"] = enc(id);
    obj["boundary"] = enc(data->assignedBoundary);
    auto unknown = impl_->unknownFields.find(id.value);
    if (unknown != impl_->unknownFields.end()) {
        mergeUnknown(obj, unknown->second);
    }
    return Result<std::string>::ok(json::dump(obj));
}

Result<OperationId> LSContext::deserializeOperation(LayerId into, std::string_view text) {
    if (impl_->findLayer(into) == nullptr) {
        return Result<OperationId>::err(LSError::InvalidId);
    }
    json::Value obj;
    if (!json::parse(std::string(text), obj) || !obj.isObject()) {
        return Result<OperationId>::err(LSError::DeserializationFailure);
    }

    // Ids inside a standalone operation refer to entities that already exist in
    // this context, so they are read as-is rather than remapped.
    DecodeContext ctx;
    Operation operation;
    std::set<std::string> consumed {"id", "boundary"};
    if (!readOperation(obj, ctx, operation, consumed)) {
        return Result<OperationId>::err(LSError::DeserializationFailure);
    }

    auto added = addOperation(into, operation);
    if (added.fail()) {
        return added;
    }
    if (const json::Value* boundary = obj.find("boundary")) {
        BoundaryId assigned;
        dec(*boundary, ctx, assigned);
        if (assigned.valid()) {
            assignBoundaryToOperation(assigned, added.value);
        }
    }
    const std::string unknown = collectUnknown(obj, consumed);
    if (!unknown.empty()) {
        impl_->unknownFields[added.value.value] = unknown;
    }
    return added;
}

Result<SerializedData> LSContext::migrateVersion(const SerializedData& data, uint32_t targetVersion) {
    if (targetVersion > LS_ENGINE_VERSION) {
        // This build cannot invent a future schema.
        return Result<SerializedData>::err(LSError::VersionMigrationFailed);
    }

    json::Value root;
    if (!json::parse(fromBytes(data.bytes), root) || !root.isObject()) {
        return Result<SerializedData>::err(LSError::DeserializationFailure);
    }

    const LSError stepped = migrateJson(root, data.engineVersion, targetVersion);
    if (stepped != LSError::None) {
        return Result<SerializedData>::err(stepped);
    }

    // Within a major version the schema is additive, so migration rewrites the
    // stamp and leaves every field, known or not, in place.
    root["engineVersion"] = enc(targetVersion);

    SerializedData migrated;
    migrated.bytes = toBytes(json::dump(root));
    migrated.engineVersion = targetVersion;
    migrated.formatTag = data.formatTag;
    return Result<SerializedData>::ok(std::move(migrated));
}

} // namespace ls
