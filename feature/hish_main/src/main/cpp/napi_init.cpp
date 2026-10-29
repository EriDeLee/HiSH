#include "native_terminal.h"
#ifdef HISH_ENABLE_DIAGNOSTICS
#include "native_probe.h"
#endif
#include <napi/native_api.h>
#include <memory>
#include <cstring>
#include <stdexcept>

namespace {
void Check(napi_status status)
{
    if (status != napi_ok) { throw std::runtime_error("Invalid native terminal arguments"); }
}
std::string String(napi_env env, napi_value value)
{
    size_t size = 0;
    Check(napi_get_value_string_utf8(env, value, nullptr, 0, &size));
    if (size > 4096) { throw std::runtime_error("Native terminal argument is too long"); }
    std::string result(size + 1, '\0');
    Check(napi_get_value_string_utf8(env, value, result.data(), result.size(), &size));
    result.resize(size);
    return result;
}
int Integer(napi_env env, napi_value value)
{
    int32_t result = 0; Check(napi_get_value_int32(env, value, &result)); return result;
}
napi_value Number(napi_env env, int value)
{
    napi_value result; napi_create_int32(env, value, &result); return result;
}
napi_value Boolean(napi_env env, bool value)
{
    napi_value result; napi_get_boolean(env, value, &result); return result;
}
napi_value Undefined(napi_env env)
{
    napi_value result; napi_get_undefined(env, &result); return result;
}
struct Work {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string root, mode, error;
    int columns = 80, rows = 24;
    hish::SessionInfo session {};
#ifdef HISH_ENABLE_DIAGNOSTICS
    bool probe = false;
    std::string report;
#endif
};
napi_value Start(napi_env env, napi_callback_info info)
{
    try {
        napi_value args[4]; size_t argc = 4;
        Check(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));
#ifdef HISH_ENABLE_DIAGNOSTICS
        if (argc != 4 && argc != 2) { throw std::runtime_error("start requires 4 arguments; probe requires root, kind"); }
#else
        if (argc != 4) { throw std::runtime_error("start requires 4 arguments"); }
#endif
        auto work = std::make_unique<Work>();
        work->root = String(env, args[0]);
#ifdef HISH_ENABLE_DIAGNOSTICS
        work->probe = argc == 2;
        if (work->probe) { work->mode = String(env, args[1]); }
        else
#endif
        {
            work->columns = Integer(env, args[1]); work->rows = Integer(env, args[2]); work->mode = String(env, args[3]);
        }
        napi_value promise, name;
        Check(napi_create_promise(env, &work->deferred, &promise));
        Check(napi_create_string_utf8(env, "HiSH native process", NAPI_AUTO_LENGTH, &name));
        Check(napi_create_async_work(env, nullptr, name,
            [](napi_env, void* data) {
                auto* work = static_cast<Work*>(data);
                try {
#ifdef HISH_ENABLE_DIAGNOSTICS
                    if (work->probe) { work->report = hish::Probe(work->root, work->mode); }
                    else
#endif
                    { work->session = hish::Start(work->root, work->columns, work->rows, work->mode); }
                }
                catch (const std::exception& error) { work->error = error.what(); }
            },
            [](napi_env env, napi_status status, void* data) {
                std::unique_ptr<Work> work(static_cast<Work*>(data));
                napi_value result;
                if (status == napi_ok && work->error.empty()) {
#ifdef HISH_ENABLE_DIAGNOSTICS
                    if (work->probe) {
                        napi_create_string_utf8(env, work->report.c_str(), work->report.size(), &result);
                        napi_resolve_deferred(env, work->deferred, result);
                        napi_delete_async_work(env, work->work);
                        return;
                    }
#endif
                    napi_create_object(env, &result);
                    napi_set_named_property(env, result, "id", Number(env, work->session.id));
                    napi_set_named_property(env, result, "pid", Number(env, work->session.pid));
                    napi_set_named_property(env, result, "pty", Boolean(env, work->session.pty));
                    napi_set_named_property(env, result, "ptyError", Number(env, work->session.ptyError));
                    napi_resolve_deferred(env, work->deferred, result);
                } else {
                    if (work->session.id != 0) { hish::Close(work->session.id); }
                    napi_value message;
                    const std::string error = work->error.empty() ? "Native startup cancelled" : work->error;
                    napi_create_string_utf8(env, error.c_str(), error.size(), &message);
                    napi_create_error(env, nullptr, message, &result);
                    napi_reject_deferred(env, work->deferred, result);
                }
                napi_delete_async_work(env, work->work);
            }, work.get(), &work->work));
        const napi_status queued = napi_queue_async_work(env, work->work);
        if (queued != napi_ok) { napi_delete_async_work(env, work->work); Check(queued); }
        work.release();
        return promise;
    } catch (const std::exception& error) { napi_throw_error(env, nullptr, error.what()); return nullptr; }
}

napi_value Read(napi_env env, napi_callback_info info)
{
    try {
        napi_value args[1]; size_t argc = 1;
        Check(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));
        if (argc != 1) { throw std::runtime_error("read requires a session id"); }
        auto output = hish::Read(Integer(env, args[0]));
        napi_value result, bytes; void* buffer = nullptr;
        Check(napi_create_arraybuffer(env, output.bytes.size(), &buffer, &bytes));
        if (!output.bytes.empty()) { memcpy(buffer, output.bytes.data(), output.bytes.size()); }
        Check(napi_create_object(env, &result));
        Check(napi_set_named_property(env, result, "data", bytes));
        Check(napi_set_named_property(env, result, "running", Boolean(env, output.running)));
        Check(napi_set_named_property(env, result, "rawInput", Boolean(env, output.rawInput)));
        Check(napi_set_named_property(env, result, "echoInput", Boolean(env, output.echoInput)));
        Check(napi_set_named_property(env, result, "convertEol", Boolean(env, output.convertEol)));
        napi_value controls;void* controlData = nullptr;
        Check(napi_create_arraybuffer(env, output.controlChars.size(), &controlData, &controls));
        if (!output.controlChars.empty()) memcpy(controlData, output.controlChars.data(), output.controlChars.size());
        Check(napi_set_named_property(env, result, "controlChars", controls));
        Check(napi_set_named_property(env, result, "localFlags", Number(env, output.localFlags)));
        return result;
    } catch (const std::exception& error) { napi_throw_error(env, nullptr, error.what()); return nullptr; }
}
napi_value Write(napi_env env, napi_callback_info info)
{
    try {
        napi_value args[2]; size_t argc = 2;
        Check(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));
        if (argc != 2) { throw std::runtime_error("write requires an id and ArrayBuffer"); }
        void* data = nullptr; size_t size = 0;
        Check(napi_get_arraybuffer_info(env, args[1], &data, &size));
        if (size > 65536) { throw std::runtime_error("Input frame exceeds 64 KiB"); }
        return Number(env, static_cast<int>(hish::Write(Integer(env, args[0]), static_cast<uint8_t*>(data), size)));
    } catch (const std::exception& error) { napi_throw_error(env, nullptr, error.what()); return nullptr; }
}
napi_value Resize(napi_env env, napi_callback_info info)
{
    try {
        napi_value args[3]; size_t argc = 3;
        Check(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));
        if (argc != 3) { throw std::runtime_error("resize requires id, columns, rows"); }
        hish::Resize(Integer(env, args[0]), Integer(env, args[1]), Integer(env, args[2]));
        return Undefined(env);
    } catch (const std::exception& error) { napi_throw_error(env, nullptr, error.what()); return nullptr; }
}
napi_value Close(napi_env env, napi_callback_info info)
{
    try {
        napi_value args[1]; size_t argc = 1;
        Check(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));
        if (argc != 1) { throw std::runtime_error("close requires a session id"); }
        hish::Close(Integer(env, args[0]));
        return Undefined(env);
    } catch (const std::exception& error) { napi_throw_error(env, nullptr, error.what()); return nullptr; }
}
#ifdef HISH_ENABLE_DIAGNOSTICS
napi_value DiagnosticsEnabled(napi_env env, napi_callback_info)
{
    return Boolean(env, true);
}
#endif
napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor properties[] = {
        {"start", nullptr, Start, nullptr, nullptr, nullptr, napi_default, nullptr},
#ifdef HISH_ENABLE_DIAGNOSTICS
        {"diagnosticsEnabled", nullptr, DiagnosticsEnabled, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"probe", nullptr, Start, nullptr, nullptr, nullptr, napi_default, nullptr},
#endif
        {"read", nullptr, Read, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"write", nullptr, Write, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resize", nullptr, Resize, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"close", nullptr, Close, nullptr, nullptr, nullptr, napi_default, nullptr}
    };
    napi_define_properties(env, exports, sizeof(properties) / sizeof(properties[0]), properties);
    return exports;
}
napi_module module = {1, 0, nullptr, Init, "hish_main", nullptr, {0}};
}
extern "C" __attribute__((constructor)) void RegisterHiSHNative()
{
    napi_module_register(&module);
}
