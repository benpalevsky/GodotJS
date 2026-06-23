import { Node2D, GString } from "godot";
import { version, internal, callable } from "godot-jsb";

if (typeof Node2D !== "function") {
    throw new Error(`Node2D should be a constructor, got ${typeof Node2D}`);
}
if (typeof GString !== "function") {
    throw new Error(`GString should be a constructor, got ${typeof GString}`);
}
if (typeof version !== "string" || version.length === 0) {
    throw new Error(`godot-jsb version should be a non-empty string, got ${version}`);
}
if (typeof internal !== "object" || internal === null) {
    throw new Error(`godot-jsb internal should be an object, got ${typeof internal}`);
}
if (typeof callable !== "function") {
    throw new Error(`godot-jsb callable should be a function, got ${typeof callable}`);
}
