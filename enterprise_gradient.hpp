#pragma once

#include "platform.hpp"
#include <array>
#include <atomic>
#include <cmath>

// Copy gradients at paint time. Cached source patterns always retain native colors.
namespace enterprise_gradient {
using SetSource = void (*)(void*, void*);
using Transform = void (*)(double&, double&, double&);
using TransformSurface = void* (*)(void*);
struct Matrix { double xx, yx, xy, yy, x0, y0; };

namespace detail {
struct Api {
    HMODULE module{};
    int (*status)(void*){};
    int (*type)(void*){};
    int (*rgba)(void*, double*, double*, double*, double*){};
    void* (*createSolid)(double, double, double, double){};
    int (*surface)(void*,void**){};
    void* (*createForSurface)(void*){};
    void (*destroySurface)(void*){};
    int (*linear)(void*, double*, double*, double*, double*){};
    int (*radial)(void*, double*, double*, double*, double*, double*, double*){};
    int (*count)(void*, int*){};
    int (*stop)(void*, int, double*, double*, double*, double*, double*){};
    void* (*createLinear)(double, double, double, double){};
    void* (*createRadial)(double, double, double, double, double, double){};
    void (*addStop)(void*, double, double, double, double, double){};
    void (*matrix)(void*, Matrix*){};
    void (*setMatrix)(void*, const Matrix*){};
    int (*filter)(void*){};
    void (*setFilter)(void*, int){};
    int (*extend)(void*){};
    void (*setExtend)(void*, int){};
    void (*destroy)(void*){};
};
inline Api storage;
inline std::atomic<const Api*> published{};
inline SRWLOCK initializationLock = SRWLOCK_INIT;
struct LastErrorGuard {
    DWORD saved{GetLastError()};
    ~LastErrorGuard() { SetLastError(saved); }
};
struct ModuleRef {
    HMODULE module{};
    ~ModuleRef() { if (module) FreeLibrary(module); }
};
struct OwnedPattern {
    const Api* api;
    void* value{};
    ~OwnedPattern() { if (value) api->destroy(value); }
};
struct Stop { double offset, red, green, blue, alpha; };
inline bool channel(double value) { return std::isfinite(value) && value >= 0 && value <= 1; }
inline bool finite(const Matrix& m) {
    return std::isfinite(m.xx) && std::isfinite(m.yx) && std::isfinite(m.xy) &&
        std::isfinite(m.yy) && std::isfinite(m.x0) && std::isfinite(m.y0);
}

inline void* clone(const Api& api, void* source, Transform transform, OwnedPattern& owned)
{
    if (!source || !transform || api.status(source)) return nullptr;
    const auto type = api.type(source);
    if (type != 2 && type != 3) return nullptr;
    std::array<double, 6> geometry{};
    if ((type == 2 ? api.linear(source, &geometry[0], &geometry[1], &geometry[2], &geometry[3]) :
        api.radial(source, &geometry[0], &geometry[1], &geometry[2], &geometry[3], &geometry[4], &geometry[5])))
        return nullptr;
    for (const auto value : geometry) if (!std::isfinite(value)) return nullptr;
    if (type == 3 && (geometry[2] < 0 || geometry[5] < 0)) return nullptr;
    int count{};
    if (api.count(source, &count) || count < 1 || count > 128) return nullptr;
    std::array<Stop, 128> stops{};
    bool changed{};
    for (int i = 0; i < count; ++i) {
        auto& stop = stops[static_cast<size_t>(i)];
        if (api.stop(source, i, &stop.offset, &stop.red, &stop.green, &stop.blue, &stop.alpha) ||
            !channel(stop.offset) || !channel(stop.red) || !channel(stop.green) ||
            !channel(stop.blue) || !channel(stop.alpha)) return nullptr;
        const auto red=stop.red, green=stop.green, blue=stop.blue;
        transform(stop.red, stop.green, stop.blue);
        if (!channel(stop.red) || !channel(stop.green) || !channel(stop.blue)) return nullptr;
        changed |= stop.red != red || stop.green != green || stop.blue != blue;
    }
    if (!changed) return nullptr;
    Matrix matrix{};
    api.matrix(source, &matrix);
    const auto filter=api.filter(source), extend=api.extend(source);
    if (!finite(matrix) || filter < 0 || filter > 5 || extend < 0 || extend > 3 || api.status(source))
        return nullptr;
    auto* destination = type == 2 ? api.createLinear(geometry[0], geometry[1], geometry[2], geometry[3]) :
        api.createRadial(geometry[0], geometry[1], geometry[2], geometry[3], geometry[4], geometry[5]);
    // A broken factory returning the borrowed pattern does not transfer ownership.
    if (!destination || destination == source) return nullptr;
    owned.value=destination;
    if (api.status(destination)) return nullptr;
    for (int i=0; i<count; ++i) {
        const auto& stop=stops[static_cast<size_t>(i)];
        api.addStop(destination,stop.offset,stop.red,stop.green,stop.blue,stop.alpha);
    }
    api.setMatrix(destination,&matrix);
    api.setFilter(destination,filter);
    api.setExtend(destination,extend);
    return !api.status(destination) && !api.status(source) ? destination : nullptr;
}

inline void* cloneSolid(const Api& api, void* source, Transform transform, OwnedPattern& owned)
{
    if (!source || !transform || api.status(source) || api.type(source) != 0) return nullptr;
    double red{}, green{}, blue{}, alpha{};
    if (api.rgba(source,&red,&green,&blue,&alpha) ||
        !channel(red) || !channel(green) || !channel(blue) || !channel(alpha)) return nullptr;
    const auto oldR=red, oldG=green, oldB=blue;
    transform(red,green,blue);
    if ((red == oldR && green == oldG && blue == oldB) ||
        !channel(red) || !channel(green) || !channel(blue)) return nullptr;
    auto* destination = api.createSolid(red,green,blue,alpha);
    if (!destination || destination == source) return nullptr;
    owned.value=destination;
    return !api.status(destination) ? destination : nullptr;
}

inline void* cloneImage(const Api& api, void* source, TransformSurface transform, OwnedPattern& owned)
{
    if (api.status(source) || api.type(source)!=1) return nullptr;
    void* surface{};
    if (api.surface(source,&surface) || !surface) return nullptr;
    Matrix matrix{};
    api.matrix(source,&matrix);
    const auto filter=api.filter(source), extend=api.extend(source);
    if (!finite(matrix) || filter<0 || filter>5 || extend<0 || extend>3) return nullptr;
    auto* themed=transform(surface);
    if (!themed || themed==surface) return nullptr;
    auto* destination=api.createForSurface(themed);
    api.destroySurface(themed);
    if (!destination || destination==source) return nullptr;
    owned.value=destination;
    api.setMatrix(destination,&matrix);
    api.setFilter(destination,filter);
    api.setExtend(destination,extend);
    return api.status(destination) ? nullptr : destination;
}
}

inline bool initialize(HMODULE cairo)
{
    detail::LastErrorGuard error;
    if (!cairo) return false;
    AcquireSRWLockExclusive(&detail::initializationLock);
    struct Unlock { ~Unlock() { ReleaseSRWLockExclusive(&detail::initializationLock); } } unlock;
    if (const auto* existing=detail::published.load(std::memory_order_acquire))
        return existing->module == cairo;
    detail::ModuleRef reference;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(cairo),&reference.module) ||
        !matchesSha256(modulePath(reference.module),cairoHash)) return false;
    detail::Api api;
    api.module=reference.module;
#define ENTERPRISE_GRADIENT_BIND(member, symbol) \
    api.member=reinterpret_cast<decltype(api.member)>(GetProcAddress(reference.module,symbol)); \
    if (!api.member) return false
    ENTERPRISE_GRADIENT_BIND(status,"cairo_pattern_status");
    ENTERPRISE_GRADIENT_BIND(type,"cairo_pattern_get_type");
    ENTERPRISE_GRADIENT_BIND(rgba,"cairo_pattern_get_rgba");
    ENTERPRISE_GRADIENT_BIND(createSolid,"cairo_pattern_create_rgba");
    ENTERPRISE_GRADIENT_BIND(surface,"cairo_pattern_get_surface");
    ENTERPRISE_GRADIENT_BIND(createForSurface,"cairo_pattern_create_for_surface");
    ENTERPRISE_GRADIENT_BIND(destroySurface,"cairo_surface_destroy");
    ENTERPRISE_GRADIENT_BIND(linear,"cairo_pattern_get_linear_points");
    ENTERPRISE_GRADIENT_BIND(radial,"cairo_pattern_get_radial_circles");
    ENTERPRISE_GRADIENT_BIND(count,"cairo_pattern_get_color_stop_count");
    ENTERPRISE_GRADIENT_BIND(stop,"cairo_pattern_get_color_stop_rgba");
    ENTERPRISE_GRADIENT_BIND(createLinear,"cairo_pattern_create_linear");
    ENTERPRISE_GRADIENT_BIND(createRadial,"cairo_pattern_create_radial");
    ENTERPRISE_GRADIENT_BIND(addStop,"cairo_pattern_add_color_stop_rgba");
    ENTERPRISE_GRADIENT_BIND(matrix,"cairo_pattern_get_matrix");
    ENTERPRISE_GRADIENT_BIND(setMatrix,"cairo_pattern_set_matrix");
    ENTERPRISE_GRADIENT_BIND(filter,"cairo_pattern_get_filter");
    ENTERPRISE_GRADIENT_BIND(setFilter,"cairo_pattern_set_filter");
    ENTERPRISE_GRADIENT_BIND(extend,"cairo_pattern_get_extend");
    ENTERPRISE_GRADIENT_BIND(setExtend,"cairo_pattern_set_extend");
    ENTERPRISE_GRADIENT_BIND(destroy,"cairo_pattern_destroy");
#undef ENTERPRISE_GRADIENT_BIND
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(cairo),&pinned)) return false;
    detail::storage=api;
    detail::published.store(&detail::storage,std::memory_order_release);
    return true;
}

inline void setSource(SetSource original, void* cr, void* pattern, Transform transform,
                      Transform solid = nullptr, TransformSurface image = nullptr)
{
    detail::LastErrorGuard error;
    if (!original) return;
    const auto* api=detail::published.load(std::memory_order_acquire);
    if (!api || !pattern || !transform) { original(cr,pattern); error.saved=GetLastError(); return; }
    detail::OwnedPattern owned{api};
    void* selected{};
    try {
        const auto type=api->type(pattern);
        if (image && type==1) selected=detail::cloneImage(*api,pattern,image,owned);
        else if (solid && type==0) selected=detail::cloneSolid(*api,pattern,solid,owned);
        else selected=detail::clone(*api,pattern,transform,owned);
    }
    catch (...) { selected=nullptr; }
    SetLastError(error.saved);
    original(cr,selected ? selected : pattern);
    error.saved=GetLastError();
}
}
