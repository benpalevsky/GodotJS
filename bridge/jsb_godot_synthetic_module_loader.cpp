#include "jsb_godot_synthetic_module_loader.h"

#if JSB_NATIVE_ESM && JSB_WITH_V8

#include "jsb_bridge_module_loader.h"
#include "jsb_environment.h"
#include "jsb_godot_module_loader.h"

#include "core/config/engine.h"
#include "core/core_constants.h"
#include "core/object/class_db.h"
#include "core/variant/variant.h"

namespace jsb
{
    GodotSyntheticModuleLoader::GodotSyntheticModuleLoader(Environment* p_env) : env_(p_env) {}

    v8::MaybeLocal<v8::Module> GodotSyntheticModuleLoader::resolve(const v8::Local<v8::Context>& p_context, const String& p_specifier)
    {
        if (p_specifier == "godot")
        {
            return _get_or_create_godot(p_context);
        }
        if (p_specifier == "godot-jsb")
        {
            return _get_or_create_godot_jsb(p_context);
        }
        return v8::MaybeLocal<v8::Module>();
    }

    bool GodotSyntheticModuleLoader::owns(const v8::Local<v8::Module>& p_module) const
    {
        v8::Isolate* isolate = env_->get_isolate();
        if (!godot_module_.IsEmpty() && godot_module_.Get(isolate) == p_module) return true;
        if (!godot_jsb_module_.IsEmpty() && godot_jsb_module_.Get(isolate) == p_module) return true;
        return false;
    }

    v8::MaybeLocal<v8::Value> GodotSyntheticModuleLoader::populate(const v8::Local<v8::Context>& p_context, const v8::Local<v8::Module>& p_module)
    {
        v8::Isolate* isolate = env_->get_isolate();
        if (!godot_module_.IsEmpty() && godot_module_.Get(isolate) == p_module)
        {
            for (const StringName& js_name : godot_export_names_)
            {
                const v8::Local<v8::String> name_js = impl::Helper::new_string(isolate, js_name);
                const v8::Local<v8::Object> proxy = _make_lazy_proxy(p_context, name_js);
                if (p_module->SetSyntheticModuleExport(isolate, name_js, proxy).IsNothing())
                {
                    return v8::MaybeLocal<v8::Value>();
                }
            }
            return v8::MaybeLocal<v8::Value>(v8::Undefined(isolate));
        }
        if (!godot_jsb_module_.IsEmpty() && godot_jsb_module_.Get(isolate) == p_module)
        {
            const v8::Local<v8::Object> exports = godot_jsb_exports_cache_.Get(isolate);
            for (const StringName& js_name : godot_jsb_export_names_)
            {
                const v8::Local<v8::String> name_js = impl::Helper::new_string(isolate, js_name);
                v8::Local<v8::Value> value;
                if (!exports->Get(p_context, name_js).ToLocal(&value))
                {
                    return v8::MaybeLocal<v8::Value>();
                }
                if (p_module->SetSyntheticModuleExport(isolate, name_js, value).IsNothing())
                {
                    return v8::MaybeLocal<v8::Value>();
                }
            }
            return v8::MaybeLocal<v8::Value>(v8::Undefined(isolate));
        }
        return v8::MaybeLocal<v8::Value>();
    }

    v8::MaybeLocal<v8::Value> GodotSyntheticModuleLoader::_evaluation_steps(v8::Local<v8::Context> p_context, v8::Local<v8::Module> p_module)
    {
        Environment* env = Environment::wrap(p_context);
        jsb_check(env);
        return env->get_synthetic_module_loader()->populate(p_context, p_module);
    }

    void GodotSyntheticModuleLoader::_ensure_godot_export_table()
    {
        if (godot_table_built_) return;

        HashSet<StringName> seen;

        auto append = [&](const StringName& original) {
            if (original == StringName()) return;
            const String js_name = internal::NamingUtil::get_class_name(original);
            const StringName js_sn(js_name);
            if (seen.has(js_sn)) return;
            seen.insert(js_sn);
            godot_export_names_.append(js_sn);

            if (js_name != (String) original)
            {
                // Make `StringNames::get_original_name(js_name)` invertible so
                // `GodotModuleLoader::resolve_godot_export` can route into ClassDB by the original key.
                // Idempotent if the replacement is already registered (e.g. by the camel-case bootstrap
                // in Environment::Environment or by the primitive-binding registration).
                internal::StringNames::get_singleton().add_replacement(original, js_sn);
            }
        };

        // (a) singletons — highest priority in the existing resolver chain.
        {
            List<Engine::Singleton> singletons;
            Engine::get_singleton()->get_singletons(&singletons);
            for (const Engine::Singleton& s : singletons) append(s.name);
        }

        // (b) utility functions.
        {
            List<StringName> util_funcs;
            Variant::get_utility_function_list(&util_funcs);
            for (const StringName& n : util_funcs)
            {
                const String exposed = internal::NamingUtil::get_member_name(n);
                const StringName exposed_sn(exposed);
                if (seen.has(exposed_sn)) continue;
                seen.insert(exposed_sn);
                godot_export_names_.append(exposed_sn);
                if (exposed != (String) n)
                {
                    internal::StringNames::get_singleton().add_replacement(n, exposed_sn);
                }
            }
        }

        // (c) global constants (named integer constants like KEY_A, plus the bare-enum-named ones).
        {
            const int count = CoreConstants::get_global_constant_count();
            for (int i = 0; i < count; ++i)
            {
                const StringName n = CoreConstants::get_global_constant_name(i);
                const StringName exposed_sn(internal::NamingUtil::get_constant_name(n));
                if (seen.has(exposed_sn)) continue;
                seen.insert(exposed_sn);
                godot_export_names_.append(exposed_sn);
            }
        }

        // (d) classes — primitives and ClassDB-exposed object classes.
        {
            // Start at 1 to skip Variant::NIL, which has no resolver binding (`Nil` would
            // surface as an export whose first access throws). Mirrors `register_primitive_bindings`.
            for (int t = 1; t < Variant::VARIANT_MAX; ++t)
            {
                const String original = Variant::get_type_name((Variant::Type) t);
                if (original.is_empty()) continue;
                append(StringName(original));
            }

            for (const StringName& cls : internal::NamingUtil::get_exposed_original_class_list())
            {
                append(cls);
            }
        }

        // (e) global enums.
        {
            List<StringName> enums;
            CoreConstants::get_global_enums(&enums);
            for (const StringName& n : enums)
            {
                const StringName exposed_sn(internal::NamingUtil::get_enum_name(n));
                if (seen.has(exposed_sn)) continue;
                seen.insert(exposed_sn);
                godot_export_names_.append(exposed_sn);
            }
        }

        // (f) the special `Variant` namespace.
        append(jsb_string_name(Variant));

        godot_table_built_ = true;
    }

    v8::Local<v8::Module> GodotSyntheticModuleLoader::_get_or_create_godot(const v8::Local<v8::Context>& p_context)
    {
        v8::Isolate* isolate = env_->get_isolate();
        if (!godot_module_.IsEmpty())
        {
            return godot_module_.Get(isolate);
        }

        _ensure_godot_export_table();

        Vector<v8::Local<v8::String>> name_handles;
        name_handles.resize(godot_export_names_.size());
        for (int i = 0; i < godot_export_names_.size(); ++i)
        {
            name_handles.write[i] = impl::Helper::new_string(isolate, godot_export_names_[i]);
        }

        const v8::Local<v8::String> module_name = impl::Helper::new_string_ascii(isolate, "godot");
        const v8::MemorySpan<const v8::Local<v8::String>> export_names(name_handles.ptr(), name_handles.size());
        const v8::Local<v8::Module> mod = v8::Module::CreateSyntheticModule(isolate, module_name, export_names, &_evaluation_steps);
        godot_module_.Reset(isolate, mod);
        return mod;
    }

    v8::Local<v8::Module> GodotSyntheticModuleLoader::_get_or_create_godot_jsb(const v8::Local<v8::Context>& p_context)
    {
        v8::Isolate* isolate = env_->get_isolate();
        if (!godot_jsb_module_.IsEmpty())
        {
            return godot_jsb_module_.Get(isolate);
        }

        const v8::Local<v8::Object> exports = BridgeModuleLoader::build_exports(isolate, p_context);
        godot_jsb_exports_cache_.Reset(isolate, exports);

        const v8::Local<v8::Array> own_names = exports->GetOwnPropertyNames(p_context).ToLocalChecked();
        const uint32_t name_count = own_names->Length();

        godot_jsb_export_names_.resize((int) name_count);
        Vector<v8::Local<v8::String>> name_handles;
        name_handles.resize((int) name_count);
        for (uint32_t i = 0; i < name_count; ++i)
        {
            const v8::Local<v8::Value> key_val = own_names->Get(p_context, i).ToLocalChecked();
            const v8::Local<v8::String> key_str = key_val.As<v8::String>();
            godot_jsb_export_names_.write[(int) i] = StringName(impl::Helper::to_string(isolate, key_str));
            name_handles.write[(int) i] = key_str;
        }

        const v8::Local<v8::String> module_name = impl::Helper::new_string_ascii(isolate, "godot-jsb");
        const v8::MemorySpan<const v8::Local<v8::String>> export_names(name_handles.ptr(), name_handles.size());
        const v8::Local<v8::Module> mod = v8::Module::CreateSyntheticModule(isolate, module_name, export_names, &_evaluation_steps);
        godot_jsb_module_.Reset(isolate, mod);
        return mod;
    }

    v8::Local<v8::Function> GodotSyntheticModuleLoader::_ensure_reflect(const v8::Local<v8::Context>& p_context, const char* p_name, v8::Global<v8::Function>& r_slot)
    {
        v8::Isolate* isolate = env_->get_isolate();
        if (!r_slot.IsEmpty()) return r_slot.Get(isolate);

        const v8::Local<v8::Object> global = p_context->Global();
        const v8::Local<v8::Value> reflect_val = global->Get(p_context, impl::Helper::new_string_ascii(isolate, "Reflect")).ToLocalChecked();
        jsb_check(reflect_val->IsObject());
        const v8::Local<v8::Value> fn_val = reflect_val.As<v8::Object>()->Get(p_context, impl::Helper::new_string_ascii(isolate, p_name)).ToLocalChecked();
        jsb_check(fn_val->IsFunction());
        const v8::Local<v8::Function> fn = fn_val.As<v8::Function>();
        r_slot.Reset(isolate, fn);
        return fn;
    }

    v8::Local<v8::Object> GodotSyntheticModuleLoader::_ensure_proxy_handler(const v8::Local<v8::Context>& p_context)
    {
        v8::Isolate* isolate = env_->get_isolate();
        if (!proxy_handler_.IsEmpty()) return proxy_handler_.Get(isolate);

        const v8::Local<v8::Object> handler = v8::Object::New(isolate);
        handler->Set(p_context, impl::Helper::new_string_ascii(isolate, "get"), JSB_NEW_FUNCTION(p_context, _trap_get, {})).Check();
        handler->Set(p_context, impl::Helper::new_string_ascii(isolate, "construct"), JSB_NEW_FUNCTION(p_context, _trap_construct, {})).Check();
        handler->Set(p_context, impl::Helper::new_string_ascii(isolate, "getPrototypeOf"), JSB_NEW_FUNCTION(p_context, _trap_get_prototype_of, {})).Check();
        proxy_handler_.Reset(isolate, handler);
        return handler;
    }

    v8::Local<v8::Object> GodotSyntheticModuleLoader::_make_lazy_proxy(const v8::Local<v8::Context>& p_context, const v8::Local<v8::String>& p_export_name_js)
    {
        v8::Isolate* isolate = env_->get_isolate();

        // Target = a noop function carrying the export name as its own `name` property — the trap
        // callbacks read it via `target.As<Function>()->GetName()` to identify which export is being
        // accessed. A function target keeps the proxy callable / constructable.
        const v8::Local<v8::Function> target = impl::Helper::new_noop_constructor(isolate, p_context);
        target->SetName(p_export_name_js);

        const v8::Local<v8::Object> handler = _ensure_proxy_handler(p_context);
        return v8::Proxy::New(p_context, target, handler).ToLocalChecked();
    }

    bool GodotSyntheticModuleLoader::lazy_resolve_godot(
        const v8::Local<v8::Context>& p_context,
        const v8::Local<v8::String>& p_export_name_js,
        v8::Local<v8::Value>& r_resolved)
    {
        v8::Isolate* isolate = env_->get_isolate();
        const StringName js_name = impl::Helper::to_string(isolate, p_export_name_js);
        if (!GodotModuleLoader::resolve_godot_export(env_, p_context, js_name, r_resolved))
        {
            return false;
        }

        if (!godot_module_.IsEmpty())
        {
            const v8::Local<v8::Module> mod = godot_module_.Get(isolate);
            (void) mod->SetSyntheticModuleExport(isolate, p_export_name_js, r_resolved);
        }
        return true;
    }

    void GodotSyntheticModuleLoader::_trap_get(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        v8::Isolate* isolate = info.GetIsolate();
        const v8::Local<v8::Context> context = isolate->GetCurrentContext();
        Environment* env = Environment::wrap(isolate);
        GodotSyntheticModuleLoader* loader = env->get_synthetic_module_loader();

        const v8::Local<v8::Function> target = info[0].As<v8::Function>();
        const v8::Local<v8::String> export_name_js = target->GetName().As<v8::String>();
        v8::Local<v8::Value> resolved;
        if (!loader->lazy_resolve_godot(context, export_name_js, resolved))
        {
            impl::Helper::throw_error(isolate,
                jsb_format("godot export not found '%s'", impl::Helper::to_string(isolate, export_name_js)));
            return;
        }

        const v8::Local<v8::Value> property = info[1];
        if (!resolved->IsObject())
        {
            info.GetReturnValue().Set(v8::Undefined(isolate));
            return;
        }
        v8::Local<v8::Value> result;
        if (resolved.As<v8::Object>()->Get(context, property).ToLocal(&result))
        {
            info.GetReturnValue().Set(result);
        }
    }

    void GodotSyntheticModuleLoader::_trap_construct(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        v8::Isolate* isolate = info.GetIsolate();
        const v8::Local<v8::Context> context = isolate->GetCurrentContext();
        Environment* env = Environment::wrap(isolate);
        GodotSyntheticModuleLoader* loader = env->get_synthetic_module_loader();

        const v8::Local<v8::Function> target = info[0].As<v8::Function>();
        const v8::Local<v8::String> export_name_js = target->GetName().As<v8::String>();
        v8::Local<v8::Value> resolved;
        if (!loader->lazy_resolve_godot(context, export_name_js, resolved))
        {
            impl::Helper::throw_error(isolate,
                jsb_format("godot export not constructable '%s'", impl::Helper::to_string(isolate, export_name_js)));
            return;
        }

        // Forward through `Reflect.construct(real_class, args, newTarget)` to honor subclass [[Construct]]
        // semantics (newTarget must propagate so the result's prototype is the subclass's, not the parent's).
        const v8::Local<v8::Function> reflect_construct = loader->_ensure_reflect(context, "construct", loader->reflect_construct_);
        v8::Local<v8::Value> args[3] = { resolved, info[1], info.Length() >= 3 ? info[2] : resolved };
        v8::Local<v8::Value> result;
        if (reflect_construct->Call(context, v8::Undefined(isolate), 3, args).ToLocal(&result))
        {
            info.GetReturnValue().Set(result);
        }
    }

    void GodotSyntheticModuleLoader::_trap_get_prototype_of(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        v8::Isolate* isolate = info.GetIsolate();
        const v8::Local<v8::Context> context = isolate->GetCurrentContext();
        Environment* env = Environment::wrap(isolate);
        GodotSyntheticModuleLoader* loader = env->get_synthetic_module_loader();

        const v8::Local<v8::Function> target = info[0].As<v8::Function>();
        const v8::Local<v8::String> export_name_js = target->GetName().As<v8::String>();
        v8::Local<v8::Value> resolved;
        if (!loader->lazy_resolve_godot(context, export_name_js, resolved))
        {
            impl::Helper::throw_error(isolate,
                jsb_format("godot export not found '%s'", impl::Helper::to_string(isolate, export_name_js)));
            return;
        }

        if (!resolved->IsObject())
        {
            info.GetReturnValue().Set(v8::Null(isolate));
            return;
        }
        const v8::Local<v8::Object> resolved_obj = resolved.As<v8::Object>();
        v8::Local<v8::Value> proto;
        // Resolve via Reflect.getPrototypeOf so chained Proxies / accessor cases route correctly.
        const v8::Local<v8::Function> reflect_gpo = loader->_ensure_reflect(context, "getPrototypeOf", loader->reflect_get_prototype_of_);
        v8::Local<v8::Value> args[1] = { resolved_obj };
        if (reflect_gpo->Call(context, v8::Undefined(isolate), 1, args).ToLocal(&proto))
        {
            info.GetReturnValue().Set(proto);
        }
    }
}

#endif // JSB_NATIVE_ESM && JSB_WITH_V8
