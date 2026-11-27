#ifndef XSPEECH_GEN_H
#define XSPEECH_GEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XSG_PROVIDER_SIGNATURE "wiksa.x-speech-gen"
#define XSG_ABI_VERSION 1u
#define XSG_MESSAGE_REQUEST 0x58534711
#define XSG_MESSAGE_EVENT 0x58534712
#define XSG_SAMPLE_RATE 24000u
#define XSG_MAX_TEXT_BYTES 8192u
#define XSG_MAX_RULES 64u

enum XsgOperation { XSG_GET_STATUS = 1, XSG_GET_CAPABILITIES, XSG_SPEAK, XSG_CANCEL };
enum XsgStatus {
    XSG_OK, XSG_UNAVAILABLE, XSG_BAD_ABI, XSG_BAD_REQUEST, XSG_BUSY,
    XSG_CANCELLED, XSG_ENGINE_LOST, XSG_ENGINE_ERROR, XSG_NOT_FOUND, XSG_TIMEOUT,
    XSG_MODEL_UNAVAILABLE, XSG_INCOMPATIBLE, XSG_OUTPUT_LIMIT
};
enum XsgDelivery { XSG_COMPLETE = 0, XSG_STREAM = 1 };
enum XsgRadioProfile { XSG_DEFAULT_RADIO_PROFILE = 0, XSG_SERA, XSG_FAA };
enum XsgEventKind { XSG_AUDIO = 1, XSG_DONE };
enum XsgOption {
    XSG_TEMPERATURE = 1u << 0, XSG_SEED = 1u << 1, XSG_GUIDANCE = 1u << 2,
    XSG_DURATION_DELTA = 1u << 3, XSG_MAX_TOKENS = 1u << 4, XSG_GAIN_DB = 1u << 5
};
enum XsgValueKind {
    XSG_RUNWAY = 1, XSG_HEADING, XSG_FLIGHT_LEVEL, XSG_ALTITUDE, XSG_SPEED,
    XSG_WIND, XSG_FREQUENCY, XSG_QNH, XSG_SQUAWK, XSG_REGISTRATION, XSG_CALL_SIGN,
    XSG_NUMBER, XSG_DIGITS, XSG_DECIMAL, XSG_ICAO, XSG_VISIBILITY, XSG_RVR,
    XSG_CLOUD_HEIGHT, XSG_CLOCK, XSG_TIME, XSG_RADIO_NUMBER, XSG_SPELL
};

typedef struct XsgString {
    const char *data;
    uint32_t bytes;
} XsgString;

typedef struct XsgRule {
    XsgString expression;
    XsgString spoken;
    XsgString language;
    uint32_t case_sensitive;
} XsgRule;

typedef struct XsgAnnotation {
    uint32_t start; /* Inclusive UTF-8 byte offset in request.text. */
    uint32_t end;   /* Exclusive UTF-8 byte offset. */
    uint32_t kind;
} XsgAnnotation;

/* Send synchronously from an XPLM callback. Zero-initialize, then set version,
 * struct_size, operation and status = XSG_UNAVAILABLE. Borrowed input data is
 * copied before an accepted SPEAK returns. CANCEL uses the returned request_id.
 * After CANCEL returns XSG_OK, no further AUDIO is dispatched for that request.
 * GET_CAPABILITIES writes UTF-8 JSON including NUL to the caller's buffer; on
 * XSG_OUTPUT_LIMIT, result_bytes is the required size. GET_STATUS reports
 * ready and busy even when the engine is still starting. The four leading
 * uint32_t fields form a stable header. A mismatched version or a request
 * shorter than XsgRequest receives XSG_BAD_ABI if status fits in the header. */
typedef struct XsgRequest {
    uint32_t version, struct_size, operation, status;
    uint64_t request_id;
    XsgString text, model, language, voice;
    uint32_t delivery;
    uint32_t options;
    float temperature, guidance, duration_delta, gain_db;
    uint64_t seed;
    uint32_t max_tokens_per_chunk, rule_count, annotation_count;
    const XsgRule *rules;
    const XsgAnnotation *annotations;
    char *result;
    uint32_t result_capacity, result_bytes, ready, busy;
    uint32_t radio_profile; /* XsgRadioProfile. Zero uses the user's setting. */
} XsgRequest;

/* Receive on X-Plane's main thread. AUDIO buffers are mono float32 at
 * XSG_SAMPLE_RATE and remain valid only during the callback. Offset counts
 * samples from the start. DONE is terminal, even if no AUDIO was received.
 * An error may terminate a stream after partial audio. */
typedef struct XsgEvent {
    uint32_t version, struct_size, kind, status;
    uint64_t request_id, offset;
    const float *samples;
    uint32_t sample_count;
    const char *detail;
} XsgEvent;

#ifdef __cplusplus
}
#endif
#endif
