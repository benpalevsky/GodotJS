#ifndef GODOTJS_BRIDGE_MODULE_LOADER_H
#define GODOTJS_BRIDGE_MODULE_LOADER_H

#include "jsb_module_loader.h"

namespace jsb
{
    // internal module 'godot-jsb'
    class BridgeModuleLoader : public IModuleLoader
    {
    public:
        virtual ~BridgeModuleLoader() override = default;

        virtual bool load(Environment* p_env, JavaScriptModule& p_module) override;

        // Build the `godot-jsb` exports object eagerly. Shared by both the CJS loader
        // and the native ESM synthetic module loader.
        static v8::Local<v8::Object> build_exports(v8::Isolate* p_isolate, const v8::Local<v8::Context>& p_context);
    };

}

#endif
