/**************************************************************************/
/*  engine.cpp                                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "engine.h"

#include "core/authors.gen.h"
#include "core/donors.gen.h"
#include "core/license.gen.h"
#include "core/os/thread.h"
#include "core/version.h"

static const int MAX_QUEUED_SHADER_COMPILATION_EVENTS = 4096;
static const int MAX_QUEUED_TEXTURE_DIAGNOSTICS_EVENTS = 16384;
static const int MAX_QUEUED_FRAME_DIAGNOSTICS_EVENTS = 4096;
static const int MAX_QUEUED_SCENE_DIAGNOSTICS_EVENTS = 32768;

Engine::ShaderCompilationEvent::ShaderCompilationEvent() {
	compilation_id = 0;
	timestamp_usec = 0;
	duration_usec = 0;
	idle_frame = 0;
	render_frame = 0;
	variant = 0;
	custom_code_id = 0;
	custom_code_version = 0;
	material_rid = 0;
	object_id = 0;
	debug_target = false;
	cache_eligible = false;
	cache_lookup_attempted = false;
	cache_hit = false;
	resident_program_hit = false;
	success = true;
}

Engine::SceneDiagnosticsEvent::SceneDiagnosticsEvent() {
	started_usec = 0;
	finished_usec = 0;
	idle_frame = 0;
	thread_id = 0;
	node_count = 0;
	property_count = 0;
	main_thread = false;
}

Engine::TextureDiagnosticsEvent::TextureDiagnosticsEvent() {
	event_id = 0;
	started_usec = 0;
	finished_usec = 0;
	idle_frame = 0;
	render_frame = 0;
	thread_id = 0;
	data_size_bytes = 0;
	width = 0;
	height = 0;
	format = 0;
	mipmap_count = 0;
	main_thread = false;
	compressed = false;
	success = true;
}

Engine::FrameDiagnosticsEvent::FrameDiagnosticsEvent() {
	started_usec = 0;
	finished_usec = 0;
	idle_frame = 0;
	thread_id = 0;
	main_thread = false;
}

void Engine::set_iterations_per_second(int p_ips) {
	ERR_FAIL_COND_MSG(p_ips <= 0, "Engine iterations per second must be greater than 0.");
	ips = p_ips;
}
int Engine::get_iterations_per_second() const {
	return ips;
}

void Engine::set_physics_jitter_fix(float p_threshold) {
	if (p_threshold < 0) {
		p_threshold = 0;
	}
	physics_jitter_fix = p_threshold;
}

float Engine::get_physics_jitter_fix() const {
	return physics_jitter_fix;
}

void Engine::set_target_fps(int p_fps) {
	_target_fps = p_fps > 0 ? p_fps : 0;
}

int Engine::get_target_fps() const {
	return _target_fps;
}

uint64_t Engine::get_frames_drawn() {
	return frames_drawn;
}

void Engine::set_frame_delay(uint32_t p_msec) {
	_frame_delay = p_msec;
}

uint32_t Engine::get_frame_delay() const {
	return _frame_delay;
}

void Engine::set_time_scale(float p_scale) {
	_time_scale = p_scale;
}

float Engine::get_time_scale() const {
	return _time_scale;
}

void Engine::set_portals_active(bool p_active) {
	_portals_active = p_active;
}

Dictionary Engine::get_version_info() const {
	Dictionary dict;
	dict["major"] = VERSION_MAJOR;
	dict["minor"] = VERSION_MINOR;
	dict["patch"] = VERSION_PATCH;
	dict["hex"] = VERSION_HEX;
	dict["status"] = VERSION_STATUS;
	dict["build"] = VERSION_BUILD;
	dict["year"] = VERSION_YEAR;

	String hash = String(VERSION_HASH);
	dict["hash"] = hash.empty() ? String("unknown") : hash;

	String stringver = String(dict["major"]) + "." + String(dict["minor"]);
	if ((int)dict["patch"] != 0) {
		stringver += "." + String(dict["patch"]);
	}
	stringver += "-" + String(dict["status"]) + " (" + String(dict["build"]) + ")";
	dict["string"] = stringver;

	return dict;
}

static Array array_from_info(const char *const *info_list) {
	Array arr;
	for (int i = 0; info_list[i] != nullptr; i++) {
		arr.push_back(String::utf8(info_list[i]));
	}
	return arr;
}

static Array array_from_info_count(const char *const *info_list, int info_count) {
	Array arr;
	for (int i = 0; i < info_count; i++) {
		arr.push_back(String::utf8(info_list[i]));
	}
	return arr;
}

Dictionary Engine::get_author_info() const {
	Dictionary dict;

	dict["lead_developers"] = array_from_info(AUTHORS_LEAD_DEVELOPERS);
	dict["project_managers"] = array_from_info(AUTHORS_PROJECT_MANAGERS);
	dict["founders"] = array_from_info(AUTHORS_FOUNDERS);
	dict["developers"] = array_from_info(AUTHORS_DEVELOPERS);

	return dict;
}

Array Engine::get_copyright_info() const {
	Array components;
	for (int component_index = 0; component_index < COPYRIGHT_INFO_COUNT; component_index++) {
		const ComponentCopyright &cp_info = COPYRIGHT_INFO[component_index];
		Dictionary component_dict;
		component_dict["name"] = String::utf8(cp_info.name);
		Array parts;
		for (int i = 0; i < cp_info.part_count; i++) {
			const ComponentCopyrightPart &cp_part = cp_info.parts[i];
			Dictionary part_dict;
			part_dict["files"] = array_from_info_count(cp_part.files, cp_part.file_count);
			part_dict["copyright"] = array_from_info_count(cp_part.copyright_statements, cp_part.copyright_count);
			part_dict["license"] = String::utf8(cp_part.license);
			parts.push_back(part_dict);
		}
		component_dict["parts"] = parts;

		components.push_back(component_dict);
	}
	return components;
}

Dictionary Engine::get_donor_info() const {
	Dictionary donors;
	donors["patrons"] = array_from_info(DONORS_PATRONS);
	donors["platinum_sponsors"] = array_from_info(DONORS_SPONSORS_PLATINUM);
	donors["gold_sponsors"] = array_from_info(DONORS_SPONSORS_GOLD);
	donors["silver_sponsors"] = array_from_info(DONORS_SPONSORS_SILVER);
	donors["diamond_members"] = array_from_info(DONORS_MEMBERS_DIAMOND);
	donors["titanium_members"] = array_from_info(DONORS_MEMBERS_TITANIUM);
	donors["platinum_members"] = array_from_info(DONORS_MEMBERS_PLATINUM);
	donors["gold_members"] = array_from_info(DONORS_MEMBERS_GOLD);
	return donors;
}

Dictionary Engine::get_license_info() const {
	Dictionary licenses;
	for (int i = 0; i < LICENSE_COUNT; i++) {
		licenses[LICENSE_NAMES[i]] = LICENSE_BODIES[i];
	}
	return licenses;
}

String Engine::get_license_text() const {
	return String(GODOT_LICENSE_TEXT);
}

void Engine::set_print_error_messages(bool p_enabled) {
	_print_error_enabled = p_enabled;
}

bool Engine::is_printing_error_messages() const {
	return _print_error_enabled;
}

void Engine::set_shader_compilation_tracking_enabled(bool p_enabled) {
	if (shader_compilation_tracking_enabled.is_set() == p_enabled) {
		return;
	}

	shader_compilation_tracking_enabled.set_to(p_enabled);
	clear_shader_compilation_events();
}

bool Engine::is_shader_compilation_tracking_enabled() const {
	return shader_compilation_tracking_enabled.is_set();
}

void Engine::set_shader_program_residency_enabled(bool p_enabled) {
	shader_program_residency_enabled.set_to(p_enabled);
}

bool Engine::is_shader_program_residency_enabled() const {
	return shader_program_residency_enabled.is_set();
}

void Engine::set_shader_program_residency_owner(const String &p_owner) {
	MutexLock lock(shader_program_residency_owner_mutex);
	shader_program_residency_owner = p_owner;
}

String Engine::get_shader_program_residency_owner() const {
	MutexLock lock(shader_program_residency_owner_mutex);
	return shader_program_residency_owner;
}

uint32_t Engine::get_shader_resident_program_count() const {
	return shader_resident_program_count.get();
}

void Engine::notify_shader_program_retained() {
	shader_resident_program_count.increment();
}

void Engine::notify_shader_program_released() {
	ERR_FAIL_COND(shader_resident_program_count.get() == 0);
	shader_resident_program_count.decrement();
}

uint64_t Engine::record_shader_compilation_event(const ShaderCompilationEvent &p_event) {
	if (!shader_compilation_tracking_enabled.is_set()) {
		return 0;
	}

	ShaderCompilationEvent event = p_event;
	if (event.compilation_id == 0) {
		event.compilation_id = shader_compilation_sequence.increment();
	}

	MutexLock lock(shader_compilation_events_mutex);
	if (shader_compilation_events.size() >= MAX_QUEUED_SHADER_COMPILATION_EVENTS) {
		shader_compilation_events.remove(0);
	}
	shader_compilation_events.push_back(event);
	return event.compilation_id;
}

Array Engine::drain_shader_compilation_events() {
	Array result;
	MutexLock lock(shader_compilation_events_mutex);
	result.resize(shader_compilation_events.size());
	for (int i = 0; i < shader_compilation_events.size(); i++) {
		const ShaderCompilationEvent &event = shader_compilation_events[i];
		Dictionary item;
		item["compilation_id"] = event.compilation_id;
		item["timestamp_usec"] = event.timestamp_usec;
		item["duration_usec"] = event.duration_usec;
		item["idle_frame"] = event.idle_frame;
		item["render_frame"] = event.render_frame;
		item["variant"] = event.variant;
		item["custom_code_id"] = event.custom_code_id;
		item["custom_code_version"] = event.custom_code_version;
		item["material_rid"] = event.material_rid;
		item["object_id"] = event.object_id;
		item["phase"] = event.phase;
		item["operation"] = event.operation;
		item["backend"] = event.backend;
		item["compilation_mode"] = event.compilation_mode;
		item["source"] = event.source;
		item["shader_name"] = event.shader_name;
		item["material_path"] = event.material_path;
		item["custom_code_hash"] = event.custom_code_hash;
		item["debug_target"] = event.debug_target;
		item["cache_eligible"] = event.cache_eligible;
		item["cache_lookup_attempted"] = event.cache_lookup_attempted;
		item["cache_hit"] = event.cache_hit;
		item["resident_program_hit"] = event.resident_program_hit;
		item["program_cache_key"] = event.program_cache_key;
		item["vertex_source_hash"] = event.vertex_source_hash;
		item["fragment_source_hash"] = event.fragment_source_hash;
		Array enabled_conditionals;
		for (int j = 0; j < event.enabled_conditionals.size(); j++) {
			enabled_conditionals.push_back(event.enabled_conditionals[j]);
		}
		item["enabled_conditionals"] = enabled_conditionals;
		Array custom_defines;
		for (int j = 0; j < event.custom_defines.size(); j++) {
			custom_defines.push_back(event.custom_defines[j]);
		}
		item["custom_defines"] = custom_defines;
		if (!event.generated_vertex_source.empty()) {
			item["generated_vertex_source"] = event.generated_vertex_source;
		}
		if (!event.generated_fragment_source.empty()) {
			item["generated_fragment_source"] = event.generated_fragment_source;
		}
		item["success"] = event.success;
		result[i] = item;
	}
	shader_compilation_events.clear();
	return result;
}

void Engine::clear_shader_compilation_events() {
	MutexLock lock(shader_compilation_events_mutex);
	shader_compilation_events.clear();
}

void Engine::set_shader_compilation_debug_targets(const PoolStringArray &p_targets) {
	Vector<String> normalized_targets;
	PoolStringArray::Read targets = p_targets.read();
	for (int i = 0; i < p_targets.size(); i++) {
		String target = targets[i].strip_edges().replace("\\", "/");
		if (!target.empty() && normalized_targets.find(target) == -1) {
			normalized_targets.push_back(target);
		}
	}

	MutexLock lock(shader_compilation_debug_targets_mutex);
	shader_compilation_debug_targets = normalized_targets;
}

PoolStringArray Engine::get_shader_compilation_debug_targets() const {
	PoolStringArray result;
	MutexLock lock(shader_compilation_debug_targets_mutex);
	for (int i = 0; i < shader_compilation_debug_targets.size(); i++) {
		result.push_back(shader_compilation_debug_targets[i]);
	}
	return result;
}

bool Engine::is_shader_compilation_debug_target(const String &p_material_path) const {
	String material_path = p_material_path.replace("\\", "/");
	String material_file = material_path.get_file().get_slice("::", 0);

	MutexLock lock(shader_compilation_debug_targets_mutex);
	for (int i = 0; i < shader_compilation_debug_targets.size(); i++) {
		const String &target = shader_compilation_debug_targets[i];
		if (material_path == target || material_path.begins_with(target + "::")) {
			return true;
		}
		if (target.find("/") == -1 && material_file == target) {
			return true;
		}
	}
	return false;
}

void Engine::set_texture_diagnostics_tracking_enabled(bool p_enabled) {
	if (texture_diagnostics_tracking_enabled.is_set() == p_enabled) {
		return;
	}
	texture_diagnostics_tracking_enabled.set_to(p_enabled);
	clear_texture_diagnostics_events();
	clear_frame_diagnostics_events();
	clear_scene_diagnostics_events();
}

bool Engine::is_texture_diagnostics_tracking_enabled() const {
	return texture_diagnostics_tracking_enabled.is_set();
}

uint64_t Engine::record_texture_diagnostics_event(const TextureDiagnosticsEvent &p_event) {
	if (!texture_diagnostics_tracking_enabled.is_set()) {
		return 0;
	}
	TextureDiagnosticsEvent event = p_event;
	if (event.event_id == 0) {
		event.event_id = texture_diagnostics_sequence.increment();
	}
	MutexLock lock(texture_diagnostics_events_mutex);
	if (texture_diagnostics_events.size() >= MAX_QUEUED_TEXTURE_DIAGNOSTICS_EVENTS) {
		texture_diagnostics_events.remove(0);
	}
	texture_diagnostics_events.push_back(event);
	return event.event_id;
}

Array Engine::drain_texture_diagnostics_events() {
	Array result;
	MutexLock lock(texture_diagnostics_events_mutex);
	result.resize(texture_diagnostics_events.size());
	for (int i = 0; i < texture_diagnostics_events.size(); i++) {
		const TextureDiagnosticsEvent &event = texture_diagnostics_events[i];
		Dictionary item;
		item["event_id"] = event.event_id;
		item["started_usec"] = event.started_usec;
		item["finished_usec"] = event.finished_usec;
		item["duration_usec"] = event.finished_usec - event.started_usec;
		item["idle_frame"] = event.idle_frame;
		item["render_frame"] = event.render_frame;
		item["thread_id"] = event.thread_id;
		item["data_size_bytes"] = event.data_size_bytes;
		item["width"] = event.width;
		item["height"] = event.height;
		item["format"] = event.format;
		item["mipmap_count"] = event.mipmap_count;
		item["operation"] = event.operation;
		item["path"] = event.path;
		item["backend"] = event.backend;
		item["main_thread"] = event.main_thread;
		item["compressed"] = event.compressed;
		item["success"] = event.success;
		result[i] = item;
	}
	texture_diagnostics_events.clear();
	return result;
}

void Engine::clear_texture_diagnostics_events() {
	MutexLock lock(texture_diagnostics_events_mutex);
	texture_diagnostics_events.clear();
}

void Engine::record_frame_diagnostics_event(const String &p_phase, uint64_t p_started_usec, uint64_t p_finished_usec) {
	if (!texture_diagnostics_tracking_enabled.is_set()) {
		return;
	}
	FrameDiagnosticsEvent event;
	event.started_usec = p_started_usec;
	event.finished_usec = p_finished_usec;
	event.idle_frame = get_idle_frames();
	event.thread_id = Thread::get_caller_id();
	event.phase = p_phase;
	event.main_thread = event.thread_id == Thread::get_main_id();
	MutexLock lock(frame_diagnostics_events_mutex);
	if (frame_diagnostics_events.size() >= MAX_QUEUED_FRAME_DIAGNOSTICS_EVENTS) {
		frame_diagnostics_events.remove(0);
	}
	frame_diagnostics_events.push_back(event);
}

Array Engine::drain_frame_diagnostics_events() {
	Array result;
	MutexLock lock(frame_diagnostics_events_mutex);
	result.resize(frame_diagnostics_events.size());
	for (int i = 0; i < frame_diagnostics_events.size(); i++) {
		const FrameDiagnosticsEvent &event = frame_diagnostics_events[i];
		Dictionary item;
		item["started_usec"] = event.started_usec;
		item["finished_usec"] = event.finished_usec;
		item["duration_usec"] = event.finished_usec - event.started_usec;
		item["idle_frame"] = event.idle_frame;
		item["thread_id"] = event.thread_id;
		item["phase"] = event.phase;
		item["main_thread"] = event.main_thread;
		result[i] = item;
	}
	frame_diagnostics_events.clear();
	return result;
}

void Engine::clear_frame_diagnostics_events() {
	MutexLock lock(frame_diagnostics_events_mutex);
	frame_diagnostics_events.clear();
}

void Engine::record_scene_diagnostics_event(const SceneDiagnosticsEvent &p_event) {
	if (!texture_diagnostics_tracking_enabled.is_set()) {
		return;
	}
	SceneDiagnosticsEvent event = p_event;
	event.idle_frame = get_idle_frames();
	event.thread_id = Thread::get_caller_id();
	event.main_thread = event.thread_id == Thread::get_main_id();
	MutexLock lock(scene_diagnostics_events_mutex);
	if (scene_diagnostics_events.size() >= MAX_QUEUED_SCENE_DIAGNOSTICS_EVENTS) {
		scene_diagnostics_events.remove(0);
	}
	scene_diagnostics_events.push_back(event);
}

Array Engine::drain_scene_diagnostics_events() {
	Array result;
	MutexLock lock(scene_diagnostics_events_mutex);
	result.resize(scene_diagnostics_events.size());
	for (int i = 0; i < scene_diagnostics_events.size(); i++) {
		const SceneDiagnosticsEvent &event = scene_diagnostics_events[i];
		Dictionary item;
		item["started_usec"] = event.started_usec;
		item["finished_usec"] = event.finished_usec;
		item["duration_usec"] = event.finished_usec - event.started_usec;
		item["idle_frame"] = event.idle_frame;
		item["thread_id"] = event.thread_id;
		item["node_count"] = event.node_count;
		item["property_count"] = event.property_count;
		item["operation"] = event.operation;
		item["scene_path"] = event.scene_path;
		item["node_path"] = event.node_path;
		item["node_class"] = event.node_class;
		item["script_path"] = event.script_path;
		item["main_thread"] = event.main_thread;
		result[i] = item;
	}
	scene_diagnostics_events.clear();
	return result;
}

void Engine::clear_scene_diagnostics_events() {
	MutexLock lock(scene_diagnostics_events_mutex);
	scene_diagnostics_events.clear();
}

void Engine::add_singleton(const Singleton &p_singleton) {
	singletons.push_back(p_singleton);
	singleton_ptrs[p_singleton.name] = p_singleton.ptr;
}

Object *Engine::get_singleton_object(const String &p_name) const {
	const Map<StringName, Object *>::Element *E = singleton_ptrs.find(p_name);
	ERR_FAIL_COND_V_MSG(!E, nullptr, "Failed to retrieve non-existent singleton '" + p_name + "'.");
	return E->get();
};

bool Engine::has_singleton(const String &p_name) const {
	return singleton_ptrs.has(p_name);
};

void Engine::get_singletons(List<Singleton> *p_singletons) {
	for (List<Singleton>::Element *E = singletons.front(); E; E = E->next()) {
		p_singletons->push_back(E->get());
	}
}

Engine *Engine::singleton = nullptr;

Engine *Engine::get_singleton() {
	return singleton;
}

Engine::Engine() {
	singleton = this;
	frames_drawn = 0;
	ips = 60;
	physics_jitter_fix = 0.5;
	_physics_interpolation_fraction = 0.0f;
	_frame_delay = 0;
	_fps = 1;
	_target_fps = 0;
	_time_scale = 1.0;
	_gpu_pixel_snap = false;
	_physics_frames = 0;
	_idle_frames = 0;
	_in_physics = false;
	_frame_ticks = 0;
	_frame_step = 0;
	editor_hint = false;
	shader_compilation_tracking_enabled.clear();
	shader_compilation_sequence.set(0);
	shader_program_residency_enabled.clear();
	shader_resident_program_count.set(0);
	shader_program_residency_owner = String();
	_portals_active = false;
	_occlusion_culling_active = false;
}

Engine::Singleton::Singleton(const StringName &p_name, Object *p_ptr) :
		name(p_name),
		ptr(p_ptr) {
#ifdef DEBUG_ENABLED
	Reference *ref = Object::cast_to<Reference>(p_ptr);
	if (ref && !ref->is_referenced()) {
		WARN_PRINT("You must use Ref<> to ensure the lifetime of a Reference object intended to be used as a singleton.");
	}
#endif
}
