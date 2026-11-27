#pragma once

#include "xspeech_gen.h"

#include <XPLMPlugin.h>
#include <cstring>
#include <cstdint>
#include <string>

inline XsgString view(const std::string& value) { return {value.data(), std::uint32_t(value.size())}; }
inline XsgRequest make_request(unsigned operation) {
    XsgRequest request{};
    request.version = XSG_ABI_VERSION;
    request.struct_size = sizeof(request);
    request.operation = operation;
    request.status = XSG_UNAVAILABLE;
    return request;
}
inline const char* status_key(unsigned status) {
    switch (status) {
    case XSG_OK: return "status.ready";
    case XSG_BUSY: return "status.busy";
    case XSG_CANCELLED: return "status.cancelled";
    case XSG_ENGINE_ERROR: return "status.engine_error";
    case XSG_ENGINE_LOST: return "status.engine_lost";
    case XSG_BAD_REQUEST: return "status.invalid";
    case XSG_TIMEOUT: return "status.timeout";
    case XSG_MODEL_UNAVAILABLE: return "status.model";
    case XSG_INCOMPATIBLE: return "status.incompatible";
    case XSG_NOT_FOUND: return "status.ended";
    case XSG_BAD_ABI: return "status.bad_abi";
    case XSG_OUTPUT_LIMIT: return "status.output_limit";
    default: return "status.unavailable";
    }
}
inline void metadata(char* name, char* signature, char* description, const char* label,
                     const char* id, const char* detail) {
    std::strcpy(name, label);
    std::strcpy(signature, id);
    std::strcpy(description, detail);
}
