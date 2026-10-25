#include <string>
#include <cstddef>
#include <set>
#include <vector>
#include <utility>
#include <memory>
#include "Domain.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <regex>
#include <system_error>
#include <openssl/evp.h>
#include "libslic3r/Model.hpp"

namespace Slic3r::AgentControl {
namespace {
void validate(const Json& value, const Json& schema)
{
    const std::string type = schema.value("type", "");
    bool valid = type.empty() || (type == "object" && value.is_object()) ||
        (type == "array" && value.is_array()) || (type == "string" && value.is_string()) ||
        (type == "boolean" && value.is_boolean()) || (type == "integer" && value.is_number_integer()) ||
        (type == "number" && value.is_number());
    ensure(valid, "invalid_params", "Parameter type does not match its advertised schema");
    if (schema.contains("enum"))
        ensure(std::find(schema["enum"].begin(), schema["enum"].end(), value) != schema["enum"].end(),
               "invalid_params", "Parameter is not an advertised enum value");
    if (value.is_number()) {
        const double number = value.get<double>();
        ensure(std::isfinite(number), "invalid_params", "Numeric parameters must be finite");
        if (schema.contains("minimum")) ensure(number >= schema["minimum"].get<double>(), "invalid_params", "Parameter is below its minimum");
        if (schema.contains("maximum")) ensure(number <= schema["maximum"].get<double>(), "invalid_params", "Parameter exceeds its maximum");
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (schema.contains("minLength")) ensure(text.size() >= schema["minLength"].get<std::size_t>(), "invalid_params", "String is too short");
        if (schema.contains("maxLength")) ensure(text.size() <= schema["maxLength"].get<std::size_t>(), "invalid_params", "String is too long");
        if (schema.contains("pattern")) ensure(std::regex_match(text, std::regex(schema["pattern"].get<std::string>())), "invalid_params", "String has invalid format");
    }
    if (value.is_array()) {
        if (schema.contains("minItems")) ensure(value.size() >= schema["minItems"].get<std::size_t>(), "invalid_params", "Array is too short");
        if (schema.contains("maxItems")) ensure(value.size() <= schema["maxItems"].get<std::size_t>(), "invalid_params", "Array is too long");
        if (schema.value("uniqueItems", false)) {
            std::set<Json> unique;
            for (const auto& item : value) ensure(unique.insert(item).second, "invalid_params", "Array entries must be unique");
        }
        if (schema.contains("items")) for (const auto& item : value) validate(item, schema["items"]);
    }
    if (value.is_object()) {
        for (const auto& key : schema.value("required", Json::array()))
            ensure(value.contains(key.get<std::string>()), "invalid_params", "A required parameter is missing");
        const Json properties = schema.value("properties", Json::object());
        for (const auto& item : value.items()) {
            if (properties.contains(item.key())) validate(item.value(), properties[item.key()]);
            else if (schema.contains("additionalProperties") && schema["additionalProperties"].is_object()) validate(item.value(), schema["additionalProperties"]);
            else ensure(schema.value("additionalProperties", Json(true)) != false, "invalid_params", "Unknown parameter; inspect the command schema");
        }
    }
}


}

CommandSpec command_spec(bool mutating, Json properties, std::vector<std::string> required)
{
    properties["target"] = {{"type", "object"}, {"additionalProperties", false},
        {"properties", {{"session_id", {{"type", "string"}}}, {"window_id", {{"type", "string"}}}}},
        {"required", {"session_id", "window_id"}}};
    required.push_back("target");
    if (mutating) {
        properties["state_token"] = {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}};
        required.push_back("state_token");
    }
    return {{{"type", "object"}, {"properties", std::move(properties)},
             {"additionalProperties", false}, {"required", std::move(required)}}, mutating};
}

void validate_parameters(const Json& params, const CommandSpec& spec) { validate(params, spec.schema); }


std::string mesh_revision(const ModelVolume& volume)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    ensure(digest && EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) == 1, "internal_error", "Cannot hash mesh");
    const auto add = [&](const void* bytes, std::size_t size) {
        ensure(EVP_DigestUpdate(digest.get(), bytes, size) == 1, "internal_error", "Cannot hash mesh");
    };
    const auto id = volume.id().id;
    add(&id, sizeof(id));
    const auto& mesh = volume.mesh().its;
    for (const auto& vertex : mesh.vertices) add(vertex.data(), sizeof(float) * 3);
    for (const auto& triangle : mesh.indices) add(triangle.data(), sizeof(int) * 3);
    const auto matrix = volume.get_matrix().matrix();
    add(matrix.data(), sizeof(double) * 16);
    unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned length = 0;
    ensure(EVP_DigestFinal_ex(digest.get(), bytes, &length) == 1, "internal_error", "Cannot finish mesh hash");
    static const char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned i = 0; i < length; ++i) { result += digits[bytes[i] >> 4]; result += digits[bytes[i] & 15]; }
    return result;
}
} // namespace Slic3r::AgentControl
