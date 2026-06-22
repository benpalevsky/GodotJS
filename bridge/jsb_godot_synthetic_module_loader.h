#ifndef GODOTJS_GODOT_SYNTHETIC_MODULE_LOADER_H
#define GODOTJS_GODOT_SYNTHETIC_MODULE_LOADER_H

#include "jsb_bridge_pch.h"

#if JSB_NATIVE_ESM && JSB_WITH_V8

namespace jsb
{
    class Environment;

    // Lazily builds V8 SyntheticModule instances for the "godot" and "godot-jsb" specifiers,
    // serving native ESM imports. Coexists with the legacy `GodotModuleLoader` /
    // `BridgeModuleLoader` (CJS path) which keep handling `require("godot")`. Owned by Environment.
    class GodotSyntheticModuleLoader
    {
    public:
        explicit GodotSyntheticModuleLoader(Environment* p_env);
        ~GodotSyntheticModuleLoader() = default;

        // Returns the synthetic module for "godot" / "godot-jsb"; empty for any other specifier
        // (the resolver falls back to the source-text path).
        v8::MaybeLocal<v8::Module> resolve(const v8::Local<v8::Context>& p_context, const String& p_specifier);

        // True when `p_module` is one of our synthetics. Used by the eval-steps trampoline.
        bool owns(const v8::Local<v8::Module>& p_module) const;

        // Called by the V8 SyntheticModuleEvaluationSteps trampoline. Populates exports.
        v8::MaybeLocal<v8::Value> populate(const v8::Local<v8::Context>& p_context, const v8::Local<v8::Module>& p_module);

        // Called by lazy `Proxy` traps to resolve a `godot` export and re-bind it on the
        // synthetic module so subsequent imports skip the proxy.
        bool lazy_resolve_godot(
            const v8::Local<v8::Context>& p_context,
            const v8::Local<v8::String>& p_export_name_js,
            v8::Local<v8::Value>& r_resolved);

    private:
        v8::Local<v8::Module> _get_or_create_godot(const v8::Local<v8::Context>& p_context);
        v8::Local<v8::Module> _get_or_create_godot_jsb(const v8::Local<v8::Context>& p_context);

        static v8::MaybeLocal<v8::Value> _evaluation_steps(v8::Local<v8::Context> p_context, v8::Local<v8::Module> p_module);

        // Build (idempotent) the JS-side export-name table + reverse map for "godot".
        void _ensure_godot_export_table();

        // Shared per-export proxy handler — one set of trap functions used across every export.
        v8::Local<v8::Object> _ensure_proxy_handler(const v8::Local<v8::Context>& p_context);

        v8::Local<v8::Object> _make_lazy_proxy(const v8::Local<v8::Context>& p_context, const v8::Local<v8::String>& p_export_name_js);

        // Trap callbacks. Each receives the proxy's call-arguments and routes through
        // `lazy_resolve_godot`, then forwards via Reflect.{get,construct,getPrototypeOf}.
        static void _trap_get(const v8::FunctionCallbackInfo<v8::Value>& info);
        static void _trap_construct(const v8::FunctionCallbackInfo<v8::Value>& info);
        static void _trap_get_prototype_of(const v8::FunctionCallbackInfo<v8::Value>& info);

        // Cached `Reflect.{construct,get,getPrototypeOf}` for the construct trap's newTarget plumbing.
        v8::Local<v8::Function> _ensure_reflect(const v8::Local<v8::Context>& p_context, const char* p_name, v8::Global<v8::Function>& r_slot);

        Environment* env_;

        v8::Global<v8::Module> godot_module_;
        v8::Global<v8::Module> godot_jsb_module_;

        v8::Global<v8::Object> proxy_handler_;
        v8::Global<v8::Function> reflect_construct_;
        v8::Global<v8::Function> reflect_get_;
        v8::Global<v8::Function> reflect_get_prototype_of_;

        // For godot-jsb, the eager exports object built once (so populate() can copy values out).
        v8::Global<v8::Object> godot_jsb_exports_cache_;
        Vector<StringName> godot_jsb_export_names_;

        // Stable insertion-order list of "godot"'s JS-facing names (one entry per export).
        Vector<StringName> godot_export_names_;
        bool godot_table_built_ = false;
    };
}

#endif // JSB_NATIVE_ESM && JSB_WITH_V8

#endif
