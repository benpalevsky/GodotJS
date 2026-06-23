#ifndef GODOTJS_GODOT_MODULE_LOADER_H
#define GODOTJS_GODOT_MODULE_LOADER_H

#include "jsb_module_loader.h"

namespace jsb
{
    // a lazy loader for Godot classes (and singletons/constants)
    class GodotModuleLoader : public IModuleLoader
    {
    public:
        virtual ~GodotModuleLoader() override = default;

        virtual bool load(Environment* p_env, JavaScriptModule& p_module) override;

        // Resolve a top-level `godot` export by JS-side name (e.g. "Node2D", "GString", "callable").
        // Mirrors the priority order used by `GDScriptLanguage::init`: singleton > utility >
        // constant > class > enum > Variant special. Returns true on hit; leaves r_value empty on miss.
        // The caller is responsible for handling thrown exceptions.
        static bool resolve_godot_export(
            Environment* p_env,
            const v8::Local<v8::Context>& p_context,
            const StringName& p_type_name,
            v8::Local<v8::Value>& r_value);

    private:
        v8::Local<v8::Object> _get_loader_proxy(Environment* p_env);

        v8::Global<v8::Object> loader_;

    };

}

#endif
