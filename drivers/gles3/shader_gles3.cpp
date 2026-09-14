/**************************************************************************/
/*  shader_gles3.cpp                                                      */
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

#include "shader_gles3.h"

#include "core/crypto/crypto_core.h"
#include "core/engine.h"
#include "core/local_vector.h"
#include "core/os/os.h"
#include "core/print_string.h"
#include "core/threaded_callable_queue.h"
#include "drivers/gles3/rasterizer_storage_gles3.h"
#include "drivers/gles3/shader_cache_gles3.h"
#include "servers/visual_server.h"

//#define DEBUG_OPENGL

#ifdef DEBUG_OPENGL

#define DEBUG_TEST_ERROR(m_section)                                         \
	{                                                                       \
		uint32_t err = glGetError();                                        \
		if (err) {                                                          \
			print_line("OpenGL Error #" + itos(err) + " at: " + m_section); \
		}                                                                   \
	}
#else

#define DEBUG_TEST_ERROR(m_section)

#endif

ShaderGLES3 *ShaderGLES3::active = nullptr;
SelfList<ShaderGLES3::Version>::List ShaderGLES3::versions_compiling;

ShaderCacheGLES3 *ShaderGLES3::shader_cache;
ThreadedCallableQueue<GLuint> *ShaderGLES3::cache_write_queue;

ThreadedCallableQueue<GLuint> *ShaderGLES3::compile_queue;
bool ShaderGLES3::parallel_compile_supported;

bool ShaderGLES3::async_hidden_forbidden;
uint32_t *ShaderGLES3::compiles_started_this_frame;
uint32_t *ShaderGLES3::max_frame_compiles_in_progress;
uint32_t ShaderGLES3::max_simultaneous_compiles;
uint32_t ShaderGLES3::active_compiles_count;
ShaderCompileQueueGLES3 *ShaderGLES3::recipe_compile_queue = nullptr;

Vector<ShaderGLES3::RecipeJob> ShaderGLES3::recipe_jobs;
uint64_t ShaderGLES3::recipe_handle_sequence = 0;
uint32_t ShaderGLES3::recipe_job_id_sequence = 0;
HashMap<String, Vector<String>> ShaderGLES3::recipe_alias_groups;
bool ShaderGLES3::recipe_source_hashing = false;

// Concatenates a shader string array into a single null-terminated buffer for
// compile jobs that run on another thread.
static void concat_shader_strings(const LocalVector<const char *> &p_shader_strings, LocalVector<char> *r_out) {
	r_out->clear();
	for (uint32_t i = 0; i < p_shader_strings.size(); i++) {
		uint32_t initial_size = r_out->size();
		uint32_t piece_len = strlen(reinterpret_cast<const char *>(p_shader_strings[i]));
		r_out->resize(initial_size + piece_len + 1);
		memcpy(r_out->ptr() + initial_size, p_shader_strings[i], piece_len);
		*(r_out->ptr() + initial_size + piece_len) = '\n';
	}
	*(r_out->ptr() + r_out->size() - 1) = '\0';
}
#ifdef DEBUG_ENABLED
bool ShaderGLES3::log_active_async_compiles_count;
#endif

uint64_t ShaderGLES3::current_frame;

//#define DEBUG_SHADER

#ifdef DEBUG_SHADER
#define DEBUG_PRINT(m_text) print_line(m_text);
#else
#define DEBUG_PRINT(m_text)
#endif

#define _EXT_COMPLETION_STATUS 0x91B1

GLint ShaderGLES3::get_uniform_location(int p_index) const {
	ERR_FAIL_COND_V(!version, -1);

	return version->uniform_location[p_index];
}

bool ShaderGLES3::bind() {
	return _bind(false);
}

bool ShaderGLES3::_bind(bool p_binding_fallback) {
	// Same base shader and version valid version?
	if (active == this && version) {
		if (new_conditional_version.code_version == conditional_version.code_version) {
			if (new_conditional_version.version == conditional_version.version) {
				return false;
			}
			// From ubershader to ubershader of the same code?
			if ((conditional_version.version & VersionKey::UBERSHADER_FLAG) && (new_conditional_version.version & VersionKey::UBERSHADER_FLAG)) {
				conditional_version.version = new_conditional_version.version;
				return false;
			}
		}
	}

	bool must_be_ready_now = !is_async_compilation_supported() || p_binding_fallback;

	conditional_version = new_conditional_version;
	version = get_current_version(must_be_ready_now);
	ERR_FAIL_COND_V(!version, false);

	bool ready = false;
	ready = _process_program_state(version, must_be_ready_now);
	if (version->compile_status == Version::COMPILE_STATUS_RESTART_NEEDED) {
		get_current_version(must_be_ready_now); // Trigger recompile
		ready = _process_program_state(version, must_be_ready_now);
	}

#ifdef DEBUG_ENABLED
	if (ready) {
		if (VS::get_singleton()->is_force_shader_fallbacks_enabled() && !must_be_ready_now && get_ubershader_flags_uniform() != -1) {
			ready = false;
		}
	}
#endif

	if (ready) {
		glUseProgram(version->ids.main);
		if (!version->uniforms_ready) {
			_setup_uniforms(custom_code_map.getptr(conditional_version.code_version));
			version->uniforms_ready = true;
		}
		DEBUG_TEST_ERROR("Use Program");
		active = this;
		return true;
	} else if (!must_be_ready_now && version->async_mode == ASYNC_MODE_VISIBLE && !p_binding_fallback && get_ubershader_flags_uniform() != -1) {
		// We can and have to fall back to the ubershader
		return _bind_ubershader();
	} else {
		// We have a compile error or must fall back by skipping render
		unbind();
		return false;
	}
}

bool ShaderGLES3::is_custom_code_ready_for_render(uint32_t p_code_id) {
	if (p_code_id == 0) {
		return true;
	}
	if (!is_async_compilation_supported() || get_ubershader_flags_uniform() == -1) {
		return true;
	}

	CustomCode *cc = custom_code_map.getptr(p_code_id);
	ERR_FAIL_COND_V(!cc, false);
	if (cc->async_mode == ASYNC_MODE_HIDDEN) {
#ifdef DEBUG_ENABLED
		if (VS::get_singleton()->is_force_shader_fallbacks_enabled()) {
			return false;
		}
#endif
		VersionKey effective_version;
		effective_version.version = new_conditional_version.version;
		effective_version.code_version = p_code_id;
		Version *v = version_map.getptr(effective_version);
		if (!v || cc->version != v->code_version || v->compile_status != Version::COMPILE_STATUS_OK) {
			return false;
		}
	}

	return true;
}

bool ShaderGLES3::_bind_ubershader(bool p_for_warmup) {
#ifdef DEBUG_ENABLED
	ERR_FAIL_COND_V(!is_async_compilation_supported(), false);
	ERR_FAIL_COND_V(get_ubershader_flags_uniform() == -1, false);
#endif
	new_conditional_version.version |= VersionKey::UBERSHADER_FLAG;
	bool bound = _bind(true);
	new_conditional_version.version &= ~VersionKey::UBERSHADER_FLAG;
	if (p_for_warmup) {
		// Avoid GL UB message id 131222 caused by shadow samplers not properly set up yet
		unbind();
		return bound;
	}
	int conditionals_uniform = _get_uniform(get_ubershader_flags_uniform());
#ifdef DEBUG_ENABLED
	ERR_FAIL_COND_V(conditionals_uniform == -1, false);
#endif
#ifdef DEV_ENABLED
	// So far we don't need bit 31 for conditionals. That allows us to use signed integers,
	// which are more compatible across GL driver vendors.
	CRASH_COND(new_conditional_version.version >= 0x80000000);
#endif
	glUniform1i(conditionals_uniform, new_conditional_version.version);
	return bound;
}

void ShaderGLES3::advance_async_shaders_compilation() {
	SelfList<ShaderGLES3::Version> *curr = versions_compiling.first();
	while (curr) {
		SelfList<ShaderGLES3::Version> *next = curr->next();

		ShaderGLES3::Version *v = curr->self();
		// Only if it didn't already have a chance to be processed in this frame
		if (v->last_frame_processed != current_frame) {
			v->shader->_process_program_state(v, false);
		}

		curr = next;
	}
}

void ShaderGLES3::_log_active_compiles() {
#ifdef DEBUG_ENABLED
	if (log_active_async_compiles_count) {
		if (parallel_compile_supported) {
			print_line("Async. shader compiles: " + itos(active_compiles_count));
		} else if (compile_queue) {
			print_line("Queued shader compiles: " + itos(active_compiles_count));
		} else {
			CRASH_NOW();
		}
	}
#endif
}

String ShaderGLES3::_diagnostic_compilation_mode() const {
	if (compile_queue) {
		return "secondary_context_queue";
	}
	if (parallel_compile_supported) {
		return "parallel_compile";
	}
	return "synchronous";
}

String ShaderGLES3::_diagnostic_source(const Version *p_version) const {
	if (p_version->diagnostic_resident_program_hit) {
		return "resident_program";
	}
	switch (p_version->program_binary.source) {
		case Version::ProgramBinary::SOURCE_LOCAL:
			return "source";
		case Version::ProgramBinary::SOURCE_QUEUE:
			return "compile_queue";
		case Version::ProgramBinary::SOURCE_CACHE:
			return "program_binary_cache";
		case Version::ProgramBinary::SOURCE_NONE:
			return "unknown";
	}
	return "unknown";
}

String ShaderGLES3::_join_shader_source(const LocalVector<const char *> &p_strings) {
	String source;
	for (uint32_t i = 0; i < p_strings.size(); i++) {
		source += String::utf8(p_strings[i]);
	}
	return source;
}

void ShaderGLES3::_diagnostic_start(Version *p_version, const String &p_operation) {
	Engine *engine = Engine::get_singleton();
	if (p_version->diagnostic_started || !engine->is_shader_compilation_tracking_enabled()) {
		return;
	}

	p_version->diagnostic_started_usec = OS::get_singleton()->get_ticks_usec();
	p_version->diagnostic_operation = p_operation;
	Engine::ShaderCompilationEvent event;
	event.timestamp_usec = p_version->diagnostic_started_usec;
	event.idle_frame = engine->get_idle_frames();
	event.render_frame = current_frame;
	event.variant = p_version->version_key.version;
	event.custom_code_id = p_version->version_key.code_version;
	event.custom_code_version = p_version->diagnostic_custom_code_version;
	event.phase = "started";
	event.operation = p_operation;
	event.backend = "gles3";
	event.compilation_mode = _diagnostic_compilation_mode();
	event.source = _diagnostic_source(p_version);
	event.shader_name = get_shader_name();
	event.material_path = p_version->diagnostic_material_path;
	event.debug_target = p_version->diagnostic_debug_target;
	event.cache_eligible = p_version->diagnostic_cache_eligible;
	event.cache_lookup_attempted = p_version->diagnostic_cache_lookup_attempted;
	event.cache_hit = p_version->diagnostic_cache_hit;
	event.resident_program_hit = p_version->diagnostic_resident_program_hit;
	event.program_cache_key = p_version->resident_program_key;
	event.vertex_source_hash = p_version->diagnostic_vertex_source_hash;
	event.fragment_source_hash = p_version->diagnostic_fragment_source_hash;
	event.enabled_conditionals = p_version->diagnostic_enabled_conditionals;
	event.custom_defines = p_version->diagnostic_custom_defines;
	event.generated_vertex_source = p_version->diagnostic_vertex_source;
	event.generated_fragment_source = p_version->diagnostic_fragment_source;
	event.success = true;
	p_version->diagnostic_compilation_id = engine->record_shader_compilation_event(event);
	p_version->diagnostic_started = p_version->diagnostic_compilation_id != 0;
	p_version->diagnostic_vertex_source = String();
	p_version->diagnostic_fragment_source = String();
}

void ShaderGLES3::_diagnostic_finish(Version *p_version, bool p_success) {
	if (!p_version->diagnostic_started || p_version->diagnostic_finished) {
		return;
	}
	p_version->diagnostic_finished = true;

	Engine *engine = Engine::get_singleton();
	if (!engine->is_shader_compilation_tracking_enabled()) {
		return;
	}

	Engine::ShaderCompilationEvent event;
	event.compilation_id = p_version->diagnostic_compilation_id;
	event.timestamp_usec = OS::get_singleton()->get_ticks_usec();
	event.duration_usec = event.timestamp_usec - p_version->diagnostic_started_usec;
	event.idle_frame = engine->get_idle_frames();
	event.render_frame = current_frame;
	event.variant = p_version->version_key.version;
	event.custom_code_id = p_version->version_key.code_version;
	event.custom_code_version = p_version->diagnostic_custom_code_version;
	event.phase = "finished";
	event.operation = p_version->diagnostic_operation;
	event.backend = "gles3";
	event.compilation_mode = _diagnostic_compilation_mode();
	event.source = _diagnostic_source(p_version);
	event.shader_name = get_shader_name();
	event.material_path = p_version->diagnostic_material_path;
	event.debug_target = p_version->diagnostic_debug_target;
	event.cache_eligible = p_version->diagnostic_cache_eligible;
	event.cache_lookup_attempted = p_version->diagnostic_cache_lookup_attempted;
	event.cache_hit = p_version->diagnostic_cache_hit;
	event.resident_program_hit = p_version->diagnostic_resident_program_hit;
	event.program_cache_key = p_version->resident_program_key;
	event.vertex_source_hash = p_version->diagnostic_vertex_source_hash;
	event.fragment_source_hash = p_version->diagnostic_fragment_source_hash;
	event.enabled_conditionals = p_version->diagnostic_enabled_conditionals;
	event.custom_defines = p_version->diagnostic_custom_defines;
	event.success = p_success;
	engine->record_shader_compilation_event(event);
}

bool ShaderGLES3::_process_program_state(Version *p_version, bool p_async_forbidden) {
	bool ready = false;
	bool run_next_step = true;
	while (run_next_step) {
		run_next_step = false;
		switch (p_version->compile_status) {
			case Version::COMPILE_STATUS_OK: {
				// Yeaaah!
				ready = true;
			} break;
			case Version::COMPILE_STATUS_ERROR: {
				// Sad, but we have to accept it
			} break;
			case Version::COMPILE_STATUS_PENDING:
			case Version::COMPILE_STATUS_RESTART_NEEDED: {
				// These lead to nowhere unless other piece of code starts the compile process
			} break;
			case Version::COMPILE_STATUS_SOURCE_PROVIDED: {
				p_version->shader->_diagnostic_start(p_version, "compile");
				uint32_t start_compiles_count = p_async_forbidden ? 2 : 0;
				if (!start_compiles_count) {
					uint32_t used_async_slots = MAX(active_compiles_count, *compiles_started_this_frame);
					uint32_t free_async_slots = used_async_slots < max_simultaneous_compiles ? max_simultaneous_compiles - used_async_slots : 0;
					start_compiles_count = MIN(2, free_async_slots);
				}
				if (start_compiles_count >= 1) {
					glCompileShader(p_version->ids.vert);
					if (start_compiles_count == 1) {
						p_version->compile_status = Version::COMPILE_STATUS_COMPILING_VERTEX;
					} else {
						glCompileShader(p_version->ids.frag);
						p_version->compile_status = Version::COMPILE_STATUS_COMPILING_VERTEX_AND_FRAGMENT;
					}
					if (!p_async_forbidden) {
						versions_compiling.add_last(&p_version->compiling_list);
						// Vertex and fragment shaders take independent compile slots
						active_compiles_count += start_compiles_count;
						*max_frame_compiles_in_progress = MAX(*max_frame_compiles_in_progress, active_compiles_count);
						_log_active_compiles();
					}
					(*compiles_started_this_frame) += start_compiles_count;
					run_next_step = p_async_forbidden;
				}
			} break;
			case Version::COMPILE_STATUS_COMPILING_VERTEX: {
				bool must_compile_frag_now = p_async_forbidden;
				if (!must_compile_frag_now) {
					if (active_compiles_count < max_simultaneous_compiles && *compiles_started_this_frame < max_simultaneous_compiles) {
						must_compile_frag_now = true;
					}
				}
				if (must_compile_frag_now) {
					glCompileShader(p_version->ids.frag);
					if (p_version->compiling_list.in_list()) {
						active_compiles_count++;
						*max_frame_compiles_in_progress = MAX(*max_frame_compiles_in_progress, active_compiles_count);
						_log_active_compiles();
					}
					p_version->compile_status = Version::COMPILE_STATUS_COMPILING_VERTEX_AND_FRAGMENT;
				} else if (parallel_compile_supported) {
					GLint completed = 0;
					glGetShaderiv(p_version->ids.vert, _EXT_COMPLETION_STATUS, &completed);
					if (completed) {
						// Not touching compiles count since the same slot used for vertex is now used for fragment
						glCompileShader(p_version->ids.frag);
						p_version->compile_status = Version::COMPILE_STATUS_COMPILING_FRAGMENT;
					}
				}
				run_next_step = p_async_forbidden;
			} break;
			case Version::COMPILE_STATUS_COMPILING_FRAGMENT:
			case Version::COMPILE_STATUS_COMPILING_VERTEX_AND_FRAGMENT: {
				bool must_complete_now = p_async_forbidden;
				if (!must_complete_now && parallel_compile_supported) {
					GLint vertex_completed = 0;
					if (p_version->compile_status == Version::COMPILE_STATUS_COMPILING_FRAGMENT) {
						vertex_completed = true;
					} else {
						glGetShaderiv(p_version->ids.vert, _EXT_COMPLETION_STATUS, &vertex_completed);
						if (p_version->compiling_list.in_list()) {
							active_compiles_count--;
#ifdef DEV_ENABLED
							CRASH_COND(active_compiles_count == UINT32_MAX);
#endif
							*max_frame_compiles_in_progress = MAX(*max_frame_compiles_in_progress, active_compiles_count);
							_log_active_compiles();
						}
						p_version->compile_status = Version::COMPILE_STATUS_COMPILING_FRAGMENT;
					}
					if (vertex_completed) {
						GLint frag_completed = 0;
						glGetShaderiv(p_version->ids.frag, _EXT_COMPLETION_STATUS, &frag_completed);
						if (frag_completed) {
							must_complete_now = true;
						}
					}
				}
				if (must_complete_now) {
					bool must_save_to_cache = p_version->version_key.is_subject_to_caching() && p_version->program_binary.source != Version::ProgramBinary::SOURCE_CACHE && shader_cache;
					bool ok = p_version->shader->_complete_compile(p_version->ids, must_save_to_cache, p_version->version_key.version);
					if (ok) {
						p_version->compile_status = Version::COMPILE_STATUS_LINKING;
						run_next_step = p_async_forbidden;
					} else {
						p_version->compile_status = Version::COMPILE_STATUS_ERROR;
						p_version->shader->_diagnostic_finish(p_version, false);
						if (p_version->compiling_list.in_list()) {
							p_version->compiling_list.remove_from_list();
							active_compiles_count--;
#ifdef DEV_ENABLED
							CRASH_COND(active_compiles_count == UINT32_MAX);
#endif
							_log_active_compiles();
						}
					}
				}
			} break;
			case Version::COMPILE_STATUS_PROCESSING_AT_QUEUE: {
				// This is from the async. queue
				switch (p_version->program_binary.result_from_queue.get()) {
					case -1: { // Error
						p_version->compile_status = Version::COMPILE_STATUS_ERROR;
						p_version->shader->_diagnostic_finish(p_version, false);
						p_version->compiling_list.remove_from_list();
						active_compiles_count--;
#ifdef DEV_ENABLED
						CRASH_COND(active_compiles_count == UINT32_MAX);
#endif
						_log_active_compiles();
					} break;
					case 0: { // In progress
						if (p_async_forbidden) {
							OS::get_singleton()->delay_usec(1000);
							run_next_step = true;
						}
					} break;
					case 1: { // Complete
						p_version->compile_status = Version::COMPILE_STATUS_BINARY_READY;
						run_next_step = true;
					} break;
				}
			} break;
			case Version::COMPILE_STATUS_BINARY_READY_FROM_CACHE: {
				bool eat_binary_now = p_async_forbidden;
				if (!eat_binary_now) {
					if (active_compiles_count < max_simultaneous_compiles && *compiles_started_this_frame < max_simultaneous_compiles) {
						eat_binary_now = true;
					}
				}
				if (eat_binary_now) {
					p_version->compile_status = Version::COMPILE_STATUS_BINARY_READY;
					run_next_step = true;
					if (!p_async_forbidden) {
						versions_compiling.add_last(&p_version->compiling_list);
						active_compiles_count++;
						*max_frame_compiles_in_progress = MAX(*max_frame_compiles_in_progress, active_compiles_count);
						_log_active_compiles();
						(*compiles_started_this_frame)++;
					}
				}
			} break;
			case Version::COMPILE_STATUS_BINARY_READY: {
				if (p_version->program_binary.source == Version::ProgramBinary::SOURCE_CACHE) {
					p_version->shader->_diagnostic_start(p_version, "program_binary_load");
				}
				PoolByteArray::Read r = p_version->program_binary.data.read();
				glProgramBinary(p_version->ids.main, static_cast<GLenum>(p_version->program_binary.format), r.ptr(), p_version->program_binary.data.size());
				p_version->compile_status = Version::COMPILE_STATUS_LINKING;
				run_next_step = true;
			} break;
			case Version::COMPILE_STATUS_LINKING: {
				bool must_complete_now = p_async_forbidden || p_version->program_binary.source == Version::ProgramBinary::SOURCE_QUEUE;
				if (!must_complete_now && parallel_compile_supported) {
					GLint link_completed;
					glGetProgramiv(p_version->ids.main, _EXT_COMPLETION_STATUS, &link_completed);
					must_complete_now = link_completed;
				}
				if (must_complete_now) {
					bool must_save_to_cache = p_version->version_key.is_subject_to_caching() && p_version->program_binary.source != Version::ProgramBinary::SOURCE_CACHE && shader_cache;
					bool ok = false;
					if (must_save_to_cache && p_version->program_binary.source == Version::ProgramBinary::SOURCE_LOCAL) {
						ok = p_version->shader->_complete_link(p_version->ids, &p_version->program_binary.format, &p_version->program_binary.data);
					} else {
						ok = p_version->shader->_complete_link(p_version->ids);
#ifdef DEBUG_ENABLED
#if 0
						// Simulate GL rejecting program from cache
						if (p_version->program_binary.source == Version::ProgramBinary::SOURCE_CACHE) {
							ok = false;
						}
#endif
#endif
					}
					if (ok) {
						if (must_save_to_cache) {
							String &tmp_hash = p_version->program_binary.cache_hash;
							GLenum &tmp_format = p_version->program_binary.format;
							PoolByteArray &tmp_data = p_version->program_binary.data;
							cache_write_queue->enqueue(p_version->ids.main, [=]() {
								shader_cache->store(tmp_hash, static_cast<uint32_t>(tmp_format), tmp_data);
							});
						}
						p_version->compile_status = Version::COMPILE_STATUS_OK;
						p_version->shader->_retain_resident_program(p_version);
						p_version->shader->_diagnostic_finish(p_version, true);
						ready = true;
					} else {
						p_version->shader->_diagnostic_finish(p_version, false);
						if (p_version->program_binary.source == Version::ProgramBinary::SOURCE_CACHE) {
#ifdef DEBUG_ENABLED
							WARN_PRINT("Program binary from cache has been rejected by the GL. Removing from cache.");
#endif
							shader_cache->remove(p_version->program_binary.cache_hash);
							p_version->compile_status = Version::COMPILE_STATUS_RESTART_NEEDED;
						} else {
							if (p_version->program_binary.source == Version::ProgramBinary::SOURCE_QUEUE) {
								ERR_PRINT("Program binary from compile queue has been rejected by the GL. Bug?");
							}
							p_version->compile_status = Version::COMPILE_STATUS_ERROR;
						}
					}
					p_version->program_binary.data = PoolByteArray();
					p_version->program_binary.cache_hash.clear();
					if (p_version->compiling_list.in_list()) {
						p_version->compiling_list.remove_from_list();
						active_compiles_count--;
#ifdef DEV_ENABLED
						CRASH_COND(active_compiles_count == UINT32_MAX);
#endif
						_log_active_compiles();
					}
				}
			} break;
		}
	}
	return ready;
}

void ShaderGLES3::unbind() {
	version = nullptr;
	glUseProgram(0);
	active = nullptr;
}

static void _display_error_with_code(const String &p_error, GLuint p_shader_id) {
	int line = 1;

	GLint source_len;
	glGetShaderiv(p_shader_id, GL_SHADER_SOURCE_LENGTH, &source_len);
	LocalVector<GLchar> source_buffer;
	source_buffer.resize(source_len);
	glGetShaderSource(p_shader_id, source_len, NULL, source_buffer.ptr());

	String total_code(source_buffer.ptr());
	Vector<String> lines = String(total_code).split("\n");

	for (int j = 0; j < lines.size(); j++) {
		print_line(vformat("%4d | %s", line, lines[j]));
		line++;
	}

	ERR_PRINT(p_error);
}

static CharString _prepare_ubershader_chunk(const CharString &p_chunk) {
	String s(p_chunk.get_data());
	Vector<String> lines = s.split("\n");
	s.clear();
	for (int i = 0; i < lines.size(); ++i) {
		if (lines[i].ends_with("//ubershader-skip")) {
			continue;
		} else if (lines[i].ends_with("//ubershader-runtime")) {
			// Move from the preprocessor world to the true code realm
			String l = lines[i].trim_suffix("//ubershader-runtime").strip_edges();
			{
				// Ignore other comments
				Vector<String> pieces = l.split("//");
				l = pieces[0].strip_edges();
			}
			if (l == "#else") {
				s += "} else {\n";
			} else if (l == "#endif") {
				s += "}\n";
			} else if (l.begins_with("#ifdef")) {
				Vector<String> pieces = l.split_spaces();
				CRASH_COND(pieces.size() != 2);
				s += "if ((ubershader_flags & FLAG_" + pieces[1] + ") != 0) {\n";
			} else if (l.begins_with("#ifndef")) {
				Vector<String> pieces = l.split_spaces();
				CRASH_COND(pieces.size() != 2);
				s += "if ((ubershader_flags & FLAG_" + pieces[1] + ") == 0) {\n";
			} else {
				CRASH_NOW_MSG("The shader template is using too complex syntax in a line marked with ubershader-runtime.");
			}
			continue;
		}
		s += lines[i] + "\n";
	}
	return s.ascii();
}

// Possible source-status pairs after this:
// Local - Source provided
// Queue - Processing / Binary ready / Error
// Cache - Binary ready
ShaderGLES3::Version *ShaderGLES3::get_current_version(bool &r_async_forbidden) {
	VersionKey effective_version;
	effective_version.key = conditional_version.key;
	// Store and look up ubershader with all other version bits set to zero
	if ((conditional_version.version & VersionKey::UBERSHADER_FLAG)) {
		effective_version.version = VersionKey::UBERSHADER_FLAG;
	}

	Version *_v = version_map.getptr(effective_version);
	CustomCode *cc = nullptr;
	if (_v) {
		if (_v->compile_status == Version::COMPILE_STATUS_RESTART_NEEDED) {
			_v->program_binary.source = Version::ProgramBinary::SOURCE_NONE;
		} else {
			if (effective_version.code_version != 0) {
				cc = custom_code_map.getptr(effective_version.code_version);
				ERR_FAIL_COND_V(!cc, _v);
				if (cc->version == _v->code_version) {
					return _v;
				}
			} else {
				return _v;
			}
		}
	}

	if (!_v) {
		_v = &version_map[effective_version];
		_v->version_key = effective_version;
		_v->shader = this;
		_v->uniform_location = memnew_arr(GLint, uniform_count);
	}

	Version &v = *_v;
	v.diagnostic_compilation_id = 0;
	v.diagnostic_started_usec = 0;
	v.diagnostic_material_path = diagnostic_material_path;
	v.diagnostic_custom_code_version = 0;
	v.diagnostic_program_cache_key = String();
	v.diagnostic_vertex_source_hash = String();
	v.diagnostic_fragment_source_hash = String();
	v.diagnostic_vertex_source = String();
	v.diagnostic_fragment_source = String();
	v.diagnostic_enabled_conditionals.clear();
	v.diagnostic_custom_defines.clear();
	v.diagnostic_operation = String();
	v.resident_program_key = String();
	const bool tracking_enabled = Engine::get_singleton()->is_shader_compilation_tracking_enabled();
	v.diagnostic_debug_target = tracking_enabled && Engine::get_singleton()->is_shader_compilation_debug_target(diagnostic_material_path);
	v.diagnostic_cache_eligible = effective_version.is_subject_to_caching();
	v.diagnostic_cache_lookup_attempted = false;
	v.diagnostic_cache_hit = false;
	v.diagnostic_resident_program_hit = false;
	v.diagnostic_started = false;
	v.diagnostic_finished = false;

	if (effective_version.code_version != 0) {
		cc = custom_code_map.getptr(effective_version.code_version);
		ERR_FAIL_COND_V(!cc, nullptr);
		if (cc->version != v.code_version) {
			v.code_version = cc->version;
			v.async_mode = cc->async_mode;
			v.uniforms_ready = false;
		}
	}
	v.diagnostic_custom_code_version = v.code_version;

	CompileSourceBuild src;
	if (!build_compile_sources(effective_version, cc, tracking_enabled, v.diagnostic_debug_target, src)) {
		return nullptr;
	}
	if (tracking_enabled) {
		v.diagnostic_enabled_conditionals = src.diagnostic_enabled_conditionals;
	}
	if (v.diagnostic_debug_target) {
		v.diagnostic_custom_defines = src.diagnostic_custom_defines;
	}

	if (!r_async_forbidden) {
		r_async_forbidden =
				(v.async_mode == ASYNC_MODE_HIDDEN && async_hidden_forbidden) ||
				(v.async_mode == ASYNC_MODE_VISIBLE && get_ubershader_flags_uniform() == -1);
	}

	v.resident_program_key = compute_resident_program_key(src);
	if (v.diagnostic_cache_eligible || v.diagnostic_debug_target) {
		v.program_binary.cache_hash = v.resident_program_key;
		v.diagnostic_program_cache_key = v.program_binary.cache_hash;
	}
	if (v.diagnostic_debug_target) {
		const char *no_platform_strings[] = { nullptr };
		LocalVector<const char *> no_shader_strings;
		v.diagnostic_vertex_source_hash = ShaderCacheGLES3::hash_program(no_platform_strings, src.strings_vertex, no_shader_strings);
		v.diagnostic_fragment_source_hash = ShaderCacheGLES3::hash_program(no_platform_strings, no_shader_strings, src.strings_fragment);
		v.diagnostic_vertex_source = _join_shader_source(src.strings_vertex);
		v.diagnostic_fragment_source = _join_shader_source(src.strings_fragment);
	}

	if (_reuse_resident_program(&v)) {
		if (cc) {
			cc->versions.insert(effective_version.version);
		}
		_diagnostic_start(&v, "resident_program_reuse");
		_diagnostic_finish(&v, true);
		return &v;
	}

	v.ids.main = glCreateProgram();
	ERR_FAIL_COND_V(v.ids.main == 0, nullptr);

	bool in_cache = false;
	if (shader_cache && v.diagnostic_cache_eligible) {
		v.diagnostic_cache_lookup_attempted = true;
		if (shader_cache->retrieve(v.program_binary.cache_hash, &v.program_binary.format, &v.program_binary.data)) {
			in_cache = true;
			v.diagnostic_cache_hit = true;
			v.program_binary.source = Version::ProgramBinary::SOURCE_CACHE;
			v.compile_status = Version::COMPILE_STATUS_BINARY_READY_FROM_CACHE;
		}
	}
	if (!in_cache) {
		if (compile_queue && !r_async_forbidden) {
			// Asynchronous compilation via queue (secondary context)
			// Remarks:
			// 1. We need to save vertex and fragment strings because they will not live beyond this function.
			// 2. We'll create another program since the other GL context is not shared.
			//    We are doing it that way since GL drivers can implement context sharing via locking, which
			//    would render (no pun intended) this whole effort to asynchronous useless.


			LocalVector<char> vertex_code;
			concat_shader_strings(src.strings_vertex, &vertex_code);
			LocalVector<char> fragment_code;
			concat_shader_strings(src.strings_fragment, &fragment_code);

			v.program_binary.source = Version::ProgramBinary::SOURCE_QUEUE;
			v.compile_status = Version::COMPILE_STATUS_PROCESSING_AT_QUEUE;
			_diagnostic_start(&v, "compile");
			versions_compiling.add_last(&v.compiling_list);
			active_compiles_count++;
			*max_frame_compiles_in_progress = MAX(*max_frame_compiles_in_progress, active_compiles_count);
			_log_active_compiles();
			(*compiles_started_this_frame)++;

			compile_queue->enqueue(v.ids.main, [this, &v, vertex_code, fragment_code]() {
				Version::Ids async_ids;
				async_ids.main = glCreateProgram();
				async_ids.vert = glCreateShader(GL_VERTEX_SHADER);
				async_ids.frag = glCreateShader(GL_FRAGMENT_SHADER);

				LocalVector<const char *> async_strings_vertex;
				async_strings_vertex.push_back(vertex_code.ptr());
				LocalVector<const char *> async_strings_fragment;
				async_strings_fragment.push_back(fragment_code.ptr());

				_set_source(async_ids, async_strings_vertex, async_strings_fragment);
				glCompileShader(async_ids.vert);
				glCompileShader(async_ids.frag);
				if (_complete_compile(async_ids, true, v.version_key.version) && _complete_link(async_ids, &v.program_binary.format, &v.program_binary.data)) {
					glDeleteShader(async_ids.frag);
					glDeleteShader(async_ids.vert);
					glDeleteProgram(async_ids.main);
					v.program_binary.result_from_queue.set(1);
				} else {
					v.program_binary.result_from_queue.set(0);
				}
			});
		} else {
			// Synchronous compilation, or async. via native support
			v.ids.vert = glCreateShader(GL_VERTEX_SHADER);
			v.ids.frag = glCreateShader(GL_FRAGMENT_SHADER);
			_set_source(v.ids, src.strings_vertex, src.strings_fragment);
			v.program_binary.source = Version::ProgramBinary::SOURCE_LOCAL;
			v.compile_status = Version::COMPILE_STATUS_SOURCE_PROVIDED;
		}
	}

	if (cc) {
		cc->versions.insert(effective_version.version);
	}

	return &v;
}

bool ShaderGLES3::build_compile_sources(const VersionKey &p_key, const CustomCode *p_cc, bool p_track_conditionals, bool p_track_defines, CompileSourceBuild &r_out) const {
	/* SETUP CONDITIONALS */

	LocalVector<const char *> strings_common;
#ifdef GLES_OVER_GL
	strings_common.push_back("#version 330\n");
	strings_common.push_back("#define GLES_OVER_GL\n");
#else
	strings_common.push_back("#version 300 es\n");
#endif

#ifdef ANDROID_ENABLED
	strings_common.push_back("#define ANDROID_ENABLED\n");
#endif

	for (int i = 0; i < custom_defines.size(); i++) {
		strings_common.push_back(custom_defines[i].get_data());
		strings_common.push_back("\n");
		if (p_track_defines) {
			r_out.diagnostic_custom_defines.push_back(String(custom_defines[i]).strip_edges());
		}
	}

	if (is_async_compilation_supported() && get_ubershader_flags_uniform() != -1) {
		// Indicate that this shader may be used both as ubershader and conditioned during the session
		strings_common.push_back("#define UBERSHADER_COMPAT\n");
	}

	LocalVector<CharString> flag_macros;
	bool build_ubershader = get_ubershader_flags_uniform() != -1 && (p_key.version & VersionKey::UBERSHADER_FLAG);
	if (build_ubershader) {
		strings_common.push_back("#define IS_UBERSHADER\n");
		if (p_track_conditionals) {
			r_out.diagnostic_enabled_conditionals.push_back("IS_UBERSHADER");
		}
		for (int i = 0; i < conditional_count; i++) {
			String s = vformat("#define FLAG_%s (1 << %d)\n", String(conditional_defines[i]).strip_edges().trim_prefix("#define "), i);
			CharString cs = s.ascii();
			flag_macros.push_back(cs);
			strings_common.push_back(cs.ptr());
		}
		strings_common.push_back("\n");
	} else {
		for (int i = 0; i < conditional_count; i++) {
			bool enable = ((1 << i) & p_key.version);
			strings_common.push_back(enable ? conditional_defines[i] : "");
			if (p_track_conditionals && enable) {
				r_out.diagnostic_enabled_conditionals.push_back(String(conditional_defines[i]).strip_edges().trim_prefix("#define ").strip_edges());
			}

			if (enable) {
				DEBUG_PRINT(conditional_defines[i]);
			}
		}
	}



	// Transient CharStrings (ubershader chunks, custom code strings) live in
	// r_out.owned_strings so the string arrays stay valid after this returns.
	List<CharString> &filtered_strings = r_out.owned_strings;

	/* VERTEX SHADER */

	if (p_cc) {
		for (int i = 0; i < p_cc->custom_defines.size(); i++) {
			strings_common.push_back(p_cc->custom_defines[i].get_data());
			if (p_track_defines) {
				r_out.diagnostic_custom_defines.push_back(String(p_cc->custom_defines[i]).strip_edges());
			}
			DEBUG_PRINT("CD #" + itos(i) + ": " + String(p_cc->custom_defines[i]));
		}
	}

	LocalVector<const char *> strings_vertex(strings_common);

	//vertex precision is high
	strings_vertex.push_back("precision highp float;\n");
	strings_vertex.push_back("precision highp int;\n");
#ifndef GLES_OVER_GL
	strings_vertex.push_back("precision highp sampler2D;\n");
	strings_vertex.push_back("precision highp samplerCube;\n");
	strings_vertex.push_back("precision highp sampler2DArray;\n");
#endif

	if (build_ubershader) {
		filtered_strings.push_back(_prepare_ubershader_chunk(vertex_code0));
		strings_vertex.push_back(filtered_strings.back()->get().get_data());
	} else {
		strings_vertex.push_back(vertex_code0.get_data());
	}

	if (p_cc) {
		r_out.owned_strings.push_back(p_cc->uniforms.ascii());
		strings_vertex.push_back(r_out.owned_strings.back()->get().get_data());
	}

	if (build_ubershader) {
		filtered_strings.push_back(_prepare_ubershader_chunk(vertex_code1));
		strings_vertex.push_back(filtered_strings.back()->get().get_data());
	} else {
		strings_vertex.push_back(vertex_code1.get_data());
	}

	if (p_cc) {
		r_out.owned_strings.push_back(p_cc->vertex_globals.ascii());
		strings_vertex.push_back(r_out.owned_strings.back()->get().get_data());
	}

	if (build_ubershader) {
		filtered_strings.push_back(_prepare_ubershader_chunk(vertex_code2));
		strings_vertex.push_back(filtered_strings.back()->get().get_data());
	} else {
		strings_vertex.push_back(vertex_code2.get_data());
	}

	if (p_cc) {
		r_out.owned_strings.push_back(p_cc->vertex.ascii());
		strings_vertex.push_back(r_out.owned_strings.back()->get().get_data());
	}

	if (build_ubershader) {
		filtered_strings.push_back(_prepare_ubershader_chunk(vertex_code3));
		strings_vertex.push_back(filtered_strings.back()->get().get_data());
	} else {
		strings_vertex.push_back(vertex_code3.get_data());
	}

#ifdef DEBUG_SHADER
	DEBUG_PRINT("\nVertex Code:\n\n" + String(code_string.get_data()));
	for (int i = 0; i < strings_vertex.size(); i++) {
		//print_line("vert strings "+itos(i)+":"+String(strings_vertex[i]));
	}
#endif

	/* FRAGMENT SHADER */

	LocalVector<const char *> strings_fragment(strings_common);

	//fragment precision is medium
	strings_fragment.push_back("precision highp float;\n");
	strings_fragment.push_back("precision highp int;\n");
#ifndef GLES_OVER_GL
	strings_fragment.push_back("precision highp sampler2D;\n");
	strings_fragment.push_back("precision highp samplerCube;\n");
	strings_fragment.push_back("precision highp sampler2DArray;\n");
#endif

	if (build_ubershader) {
		filtered_strings.push_back(_prepare_ubershader_chunk(fragment_code0));
		strings_fragment.push_back(filtered_strings.back()->get().get_data());
	} else {
		strings_fragment.push_back(fragment_code0.get_data());
	}

	if (p_cc) {
		r_out.owned_strings.push_back(p_cc->uniforms.ascii());
		strings_fragment.push_back(r_out.owned_strings.back()->get().get_data());
	}

	if (build_ubershader) {
		filtered_strings.push_back(_prepare_ubershader_chunk(fragment_code1));
		strings_fragment.push_back(filtered_strings.back()->get().get_data());
	} else {
		strings_fragment.push_back(fragment_code1.get_data());
	}

	if (p_cc) {
		r_out.owned_strings.push_back(p_cc->fragment_globals.ascii());
		strings_fragment.push_back(r_out.owned_strings.back()->get().get_data());
	}

	if (build_ubershader) {
		filtered_strings.push_back(_prepare_ubershader_chunk(fragment_code2));
		strings_fragment.push_back(filtered_strings.back()->get().get_data());
	} else {
		strings_fragment.push_back(fragment_code2.get_data());
	}

	if (p_cc) {
		r_out.owned_strings.push_back(p_cc->light.ascii());
		strings_fragment.push_back(r_out.owned_strings.back()->get().get_data());
	}

	if (build_ubershader) {
		filtered_strings.push_back(_prepare_ubershader_chunk(fragment_code3));
		strings_fragment.push_back(filtered_strings.back()->get().get_data());
	} else {
		strings_fragment.push_back(fragment_code3.get_data());
	}

	if (p_cc) {
		r_out.owned_strings.push_back(p_cc->fragment.ascii());
		strings_fragment.push_back(r_out.owned_strings.back()->get().get_data());
	}

	if (build_ubershader) {
		filtered_strings.push_back(_prepare_ubershader_chunk(fragment_code4));
		strings_fragment.push_back(filtered_strings.back()->get().get_data());
	} else {
		strings_fragment.push_back(fragment_code4.get_data());
	}

#ifdef DEBUG_SHADER
	DEBUG_PRINT("\nFragment Globals:\n\n" + String(code_globals.get_data()));
	DEBUG_PRINT("\nFragment Code:\n\n" + String(code_string2.get_data()));
	for (int i = 0; i < strings_fragment.size(); i++) {
		//print_line("frag strings "+itos(i)+":"+String(strings_fragment[i]));
	}
#endif

	r_out.strings_vertex = strings_vertex;
	r_out.strings_fragment = strings_fragment;
	return true;
}

String ShaderGLES3::compute_resident_program_key(const CompileSourceBuild &p_src) const {
	// The platform strings are constant per process: cache them so the recipe
	// submit path (and repeated version builds) don't issue glGetString calls
	// that contend with the driver while workers compile.
	static CharString vendor, renderer, version_str;
	if (vendor.size() == 0) {
		vendor = (const char *)glGetString(GL_VENDOR);
		renderer = (const char *)glGetString(GL_RENDERER);
		version_str = (const char *)glGetString(GL_VERSION);
	}
	const char *strings_platform[] = {
		vendor.get_data(),
		renderer.get_data(),
		version_str.get_data(),
		nullptr,
	};
	return ShaderCacheGLES3::hash_program(strings_platform, p_src.strings_vertex, p_src.strings_fragment);
}

// Defined after the version machinery; declared here for the recipe path.
static String _shader_conditional_name(const char *p_define);

// --- Recipe equivalence hashing ------------------------------------------
//
// The residency key hashes platform strings + source text, so it already
// dedups recipes that build the exact same sources. The hashes here classify
// the remaining equivalences:
// - source hash: vertex+fragment text only (no platform strings), so it is
//   comparable across machines and drivers;
// - normalized source hash: the text after dropping #define lines of enabled
//   conditionals that are never referenced anywhere else in either stage.
//   Sources differing only in those lines preprocess to the same translation
//   unit, so the equivalence holds on any conformant driver (commiteable).
// - binary hash: bytes of the compiled program binary. Observed equality is
//   only valid for the driver that produced the binaries.

static bool _is_recipe_word_char(CharType p_c) {
	return (p_c >= '0' && p_c <= '9') || (p_c >= 'a' && p_c <= 'z') || (p_c >= 'A' && p_c <= 'Z') || p_c == '_';
}

// Returns the macro name of a "#define NAME" line, or an empty string.
static String _define_line_token(const String &p_line) {
	const String trimmed = p_line.strip_edges();
	if (!trimmed.begins_with("#define")) {
		return String();
	}
	if (trimmed.length() < 8 || (trimmed[7] != ' ' && trimmed[7] != '\t')) {
		return String();
	}
	int start = 7;
	while (start < trimmed.length() && (trimmed[start] == ' ' || trimmed[start] == '\t')) {
		start++;
	}
	int end = start;
	while (end < trimmed.length() && _is_recipe_word_char(trimmed[end])) {
		end++;
	}
	if (start == end) {
		return String();
	}
	return trimmed.substr(start, end - start);
}

static void _collect_referenced_tokens(const String &p_line, Set<String> &r_tokens) {
	const int len = p_line.length();
	int i = 0;
	while (i < len) {
		if (!_is_recipe_word_char(p_line[i])) {
			i++;
			continue;
		}
		const int start = i;
		while (i < len && _is_recipe_word_char(p_line[i])) {
			i++;
		}
		r_tokens.insert(p_line.substr(start, i - start));
	}
}

static String _strip_lines_without_references(const Vector<String> &p_lines, const Set<String> &p_referenced, const Set<String> &p_conditional_names) {
	String out;
	for (int i = 0; i < p_lines.size(); i++) {
		const String token = _define_line_token(p_lines[i]);
		const bool drop = !token.empty() && p_conditional_names.has(token) && !p_referenced.has(token);
		if (!drop) {
			out += p_lines[i];
			out += "\n";
		}
	}
	return out;
}

static String _sha256_hex_pair(const CharString &p_a, const CharString &p_b) {
	CryptoCore::SHA256Context ctx;
	ctx.start();
	if (p_a.size() > 0) {
		ctx.update((const uint8_t *)p_a.get_data(), p_a.size());
	}
	const uint8_t separator[1] = { 0 };
	ctx.update(separator, 1);
	if (p_b.size() > 0) {
		ctx.update((const uint8_t *)p_b.get_data(), p_b.size());
	}
	unsigned char hash[32];
	ctx.finish(hash);
	String hex;
	for (int i = 0; i < 32; i++) {
		hex += vformat("%02x", hash[i]);
	}
	return hex;
}

static String _sha256_hex_bytes(const PoolByteArray &p_data) {
	PoolByteArray::Read r = p_data.read();
	CryptoCore::SHA256Context ctx;
	ctx.start();
	ctx.update(r.ptr(), p_data.size());
	unsigned char hash[32];
	ctx.finish(hash);
	String hex;
	for (int i = 0; i < 32; i++) {
		hex += vformat("%02x", hash[i]);
	}
	return hex;
}

void ShaderGLES3::set_recipe_source_hashing(bool p_enabled) {
	recipe_source_hashing = p_enabled;
}

void ShaderGLES3::compute_source_hashes(const CompileSourceBuild &p_src, Dictionary &r_result) const {
	if (!recipe_source_hashing) {
		// Discovery-only cost: the equivalence tooling needs these hashes;
		// production warm-ups go straight to the residency key.
		return;
	}
	LocalVector<char> vertex_code;
	concat_shader_strings(p_src.strings_vertex, &vertex_code);
	LocalVector<char> fragment_code;
	concat_shader_strings(p_src.strings_fragment, &fragment_code);

	// Source-level hash: no platform strings, comparable across machines.
	const String vertex_text((const char *)vertex_code.ptr());
	const String fragment_text((const char *)fragment_code.ptr());
	r_result["source_sha256"] = _sha256_hex_pair(vertex_text.ascii(), fragment_text.ascii());

	Set<String> conditional_names;
	for (int i = 0; i < conditional_count; i++) {
		const String name = _shader_conditional_name(conditional_defines[i]);
		if (!name.empty()) {
			conditional_names.insert(name);
		}
	}
	if (conditional_names.empty()) {
		// Nothing to normalize: the normalized source is the source itself.
		r_result["normalized_source_sha256"] = r_result["source_sha256"];
		return;
	}
	Set<String> referenced;
	for (int stage = 0; stage < 2; stage++) {
		const String &text = stage == 0 ? vertex_text : fragment_text;
		const Vector<String> lines = text.split("\n");
		for (int li = 0; li < lines.size(); li++) {
			const String token = _define_line_token(lines[li]);
			if (!token.empty() && conditional_names.has(token)) {
				continue;
			}
			_collect_referenced_tokens(lines[li], referenced);
		}
	}
	const String normalized_vertex = _strip_lines_without_references(vertex_text.split("\n"), referenced, conditional_names);
	const String normalized_fragment = _strip_lines_without_references(fragment_text.split("\n"), referenced, conditional_names);
	r_result["normalized_source_sha256"] = _sha256_hex_pair(normalized_vertex.ascii(), normalized_fragment.ascii());
}

Dictionary ShaderGLES3::submit_recipe(uint32_t p_code_id, const PoolStringArray &p_enabled_conditionals, const String &p_material_path) {
	Dictionary result;
	result["success"] = false;
	result["shader_name"] = get_shader_name();
	result["material_path"] = p_material_path;

	if (!recipe_compile_queue) {
		result["error"] = "recipe_workers_not_set";
		return result;
	}
	if (p_code_id != CUSTOM_SHADER_DISABLED && !custom_code_map.has(p_code_id)) {
		result["error"] = "invalid_custom_code_id";
		return result;
	}

	// Normalize the requested conditionals into the version mask, like the
	// variant submit path does.
	uint32_t requested_variant = 0;
	PoolStringArray normalized_conditionals;
	PoolStringArray::Read requested = p_enabled_conditionals.read();
	for (int requested_index = 0; requested_index < p_enabled_conditionals.size(); requested_index++) {
		String requested_name = requested[requested_index].strip_edges().trim_prefix("#define").strip_edges();
		bool found = false;
		for (int conditional_index = 0; conditional_index < conditional_count; conditional_index++) {
			const String conditional_name = _shader_conditional_name(conditional_defines[conditional_index]);
			if (requested_name == conditional_name) {
				requested_variant |= uint32_t(1) << conditional_index;
				normalized_conditionals.append(conditional_name);
				found = true;
				break;
			}
		}
		if (!found) {
			result["error"] = "unknown_conditional";
			result["unknown_conditional"] = requested_name;
			return result;
		}
	}
	result["enabled_conditionals"] = normalized_conditionals;
	result["variant"] = requested_variant;

	VersionKey key;
	key.version = requested_variant;
	key.code_version = p_code_id;

	// Build the sources and the residency key on the main thread: no GL work
	// beyond the cached glGetString, no version_map entry, no state machine.
	CompileSourceBuild src;
	const CustomCode *cc = nullptr;
	if (p_code_id != CUSTOM_SHADER_DISABLED) {
		cc = custom_code_map.getptr(p_code_id);
	}
	if (!build_compile_sources(key, cc, false, false, src)) {
		result["error"] = "source_build_failed";
		return result;
	}
	String program_key = compute_resident_program_key(src);
	result["program_cache_key"] = program_key;
	compute_source_hashes(src, result);

	if (Engine::get_singleton()->is_shader_program_residency_enabled()) {
		if (resident_programs.getptr(program_key)) {
			result["success"] = true;
			result["pending"] = false;
			result["already_resident"] = true;
			return result;
		}
	}

	if (recipe_jobs_in_flight.has(program_key)) {
		// Another recipe is already compiling this exact program; the poll
		// will make it resident before the handles drain.
		result["success"] = true;
		result["pending"] = false;
		result["already_queued"] = true;
		return result;
	}

	LocalVector<char> vertex_code;
	concat_shader_strings(src.strings_vertex, &vertex_code);
	LocalVector<char> fragment_code;
	concat_shader_strings(src.strings_fragment, &fragment_code);

	const uint32_t job_id = ++recipe_job_id_sequence;
	const uint64_t handle = ++recipe_handle_sequence;

	recipe_jobs_in_flight[program_key] = job_id;
	RecipeJob job;
	job.owner = this;
	job.program_key = program_key;
	job.job_id = job_id;
	job.handle = handle;
	job.material_path = p_material_path;
	job.shader_name = get_shader_name();
	recipe_jobs.push_back(job);

	// The job owns copies of the sources: no Version pointer, no shared state.
	recipe_compile_queue->enqueue_compile(job_id, [this, job_id, variant_mask = key.version, vertex_code, fragment_code]() {
		Version::Ids async_ids;
		async_ids.main = glCreateProgram();
		async_ids.vert = glCreateShader(GL_VERTEX_SHADER);
		async_ids.frag = glCreateShader(GL_FRAGMENT_SHADER);

		LocalVector<const char *> job_strings_vertex;
		job_strings_vertex.push_back(vertex_code.ptr());
		LocalVector<const char *> job_strings_fragment;
		job_strings_fragment.push_back(fragment_code.ptr());

		_set_source(async_ids, job_strings_vertex, job_strings_fragment);
		glCompileShader(async_ids.vert);
		glCompileShader(async_ids.frag);
		if (_complete_compile(async_ids, true, variant_mask)) {
			GLenum format = 0;
			PoolByteArray data;
			if (_complete_link(async_ids, &format, &data)) {
				glDeleteShader(async_ids.frag);
				glDeleteShader(async_ids.vert);
				glDeleteProgram(async_ids.main);
				recipe_compile_queue->complete_job(job_id, format, data);
				return;
			}
		}
		recipe_compile_queue->complete_job(job_id, 0, PoolByteArray());
	});

	result["success"] = true;
	result["pending"] = true;
	result["handle"] = handle;
	return result;
}

bool ShaderGLES3::_load_program_binary(const String &p_program_key, GLenum p_format, const PoolByteArray &p_data) {
	Version::Ids ids;
	ids.main = glCreateProgram();
	ERR_FAIL_COND_V(ids.main == 0, false);
	PoolByteArray::Read r = p_data.read();
	glProgramBinary(ids.main, p_format, r.ptr(), p_data.size());
	GLint status = 0;
	glGetProgramiv(ids.main, GL_LINK_STATUS, &status);
	if (status == GL_FALSE) {
		glDeleteProgram(ids.main);
		return false;
	}
	if (shader_cache) {
		// Store the binary for the next session's engine cache lookup.
		cache_write_queue->enqueue(ids.main, [=]() {
			shader_cache->store(p_program_key, (uint32_t)p_format, p_data);
		});
	}
	if (Engine::get_singleton()->is_shader_program_residency_enabled()) {
		register_resident_program_ids(p_program_key, ids);
	} else {
		glDeleteProgram(ids.main);
	}
	return true;
}

bool ShaderGLES3::consume_recipe_binary(const String &p_program_key, GLenum p_format, const PoolByteArray &p_data) {
	if (!_load_program_binary(p_program_key, p_format, p_data)) {
		return false;
	}
	// Recipes declared equivalent to this one (the equivalence analysis
	// proved they compile to the same program): replay the canonical binary
	// under each alias key so the gameplay bind path finds them resident.
	// Each alias gets its own program object: the residency store owns and
	// evicts the GL objects per key.
	Vector<String> *aliases = recipe_alias_groups.getptr(p_program_key);
	if (aliases) {
		for (int i = 0; i < aliases->size(); i++) {
			_load_program_binary((*aliases)[i], p_format, p_data);
		}
		recipe_alias_groups.erase(p_program_key);
	}
	return true;
}

void ShaderGLES3::declare_recipe_alias(const String &p_alias_key, const String &p_canonical_key) {
	if (p_alias_key.empty() || p_canonical_key.empty() || p_alias_key == p_canonical_key) {
		return;
	}
	Vector<String> *aliases = recipe_alias_groups.getptr(p_canonical_key);
	if (!aliases) {
		Vector<String> fresh;
		fresh.push_back(p_alias_key);
		recipe_alias_groups.set(p_canonical_key, fresh);
		return;
	}
	if (aliases->find(p_alias_key) < 0) {
		aliases->push_back(p_alias_key);
	}
}

void ShaderGLES3::register_resident_program_ids(const String &p_program_key, const Version::Ids &p_ids) {
	if (!Engine::get_singleton()->is_shader_program_residency_enabled()) {
		glDeleteProgram(p_ids.main);
		return;
	}
	ResidentProgram *resident = resident_programs.getptr(p_program_key);
	if (resident) {
		// Another program already covers this key: keep that one.
		_claim_resident_program(resident);
		glDeleteProgram(p_ids.main);
		return;
	}
	ResidentProgram stored;
	stored.ids = p_ids;
	_claim_resident_program(&stored);
	resident_programs[p_program_key] = stored;
	Engine::get_singleton()->notify_shader_program_retained();
}

Dictionary ShaderGLES3::poll_recipe_compiles() {
	Dictionary result;
	Array finished;
	for (uint32_t i = 0; recipe_compile_queue && i < recipe_jobs.size();) {
		RecipeJob &job = recipe_jobs.write[i];
		GLenum format = 0;
		PoolByteArray data;
		if (!recipe_compile_queue->poll_job_result(job.job_id, &format, &data)) {
			i++;
			continue;
		}
		Dictionary item;
		item["handle"] = job.handle;
		item["shader_name"] = job.shader_name;
		item["material_path"] = job.material_path;
		item["program_cache_key"] = job.program_key;
		bool success = false;
		if (data.size() > 0) {
			success = job.owner->consume_recipe_binary(job.program_key, format, data);
			item["binary_sha256"] = _sha256_hex_bytes(data);
			item["binary_size"] = (int)data.size();
		}
		item["success"] = success;
		finished.push_back(item);
		job.owner->recipe_jobs_in_flight.erase(job.program_key);
		recipe_jobs.remove(i);
	}
	result["pending"] = (int)recipe_jobs.size();
	result["finished"] = finished;
	return result;
}


void ShaderGLES3::_set_source(Version::Ids p_ids, const LocalVector<const char *> &p_vertex_strings, const LocalVector<const char *> &p_fragment_strings) const {
	glShaderSource(p_ids.vert, p_vertex_strings.size(), p_vertex_strings.ptr(), nullptr);
	glShaderSource(p_ids.frag, p_fragment_strings.size(), p_fragment_strings.ptr(), nullptr);
}

bool ShaderGLES3::_complete_compile(Version::Ids p_ids, bool p_retrievable, uint32_t p_conditional_mask) const {
	GLint status;

	glGetShaderiv(p_ids.vert, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE) {
		// error compiling
		GLsizei iloglen;
		glGetShaderiv(p_ids.vert, GL_INFO_LOG_LENGTH, &iloglen);

		if (iloglen < 0) {
			glDeleteShader(p_ids.frag);
			glDeleteShader(p_ids.vert);
			glDeleteProgram(p_ids.main);

			ERR_PRINT("Vertex shader compilation failed with empty log");
		} else {
			if (iloglen == 0) {
				iloglen = 4096; //buggy driver (Adreno 220+....)
			}

			char *ilogmem = (char *)memalloc(iloglen + 1);
			ilogmem[iloglen] = 0;
			glGetShaderInfoLog(p_ids.vert, iloglen, &iloglen, ilogmem);

			String err_string = get_shader_name() + ": Vertex Program Compilation Failed:\n";

			err_string += ilogmem;
			_display_error_with_code(err_string, p_ids.vert);
			ERR_PRINT(err_string.ascii().get_data());
			memfree(ilogmem);
			glDeleteShader(p_ids.frag);
			glDeleteShader(p_ids.vert);
			glDeleteProgram(p_ids.main);
		}

		return false;
	}

	glGetShaderiv(p_ids.frag, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE) {
		// error compiling
		GLsizei iloglen;
		glGetShaderiv(p_ids.frag, GL_INFO_LOG_LENGTH, &iloglen);

		if (iloglen < 0) {
			glDeleteShader(p_ids.frag);
			glDeleteShader(p_ids.vert);
			glDeleteProgram(p_ids.main);
			ERR_PRINT("Fragment shader compilation failed with empty log");
		} else {
			if (iloglen == 0) {
				iloglen = 4096; //buggy driver (Adreno 220+....)
			}

			char *ilogmem = (char *)memalloc(iloglen + 1);
			ilogmem[iloglen] = 0;
			glGetShaderInfoLog(p_ids.frag, iloglen, &iloglen, ilogmem);

			String err_string = get_shader_name() + ": Fragment Program Compilation Failed:\n";

			err_string += ilogmem;
			_display_error_with_code(err_string, p_ids.frag);
			ERR_PRINT(err_string.ascii().get_data());
			memfree(ilogmem);
			glDeleteShader(p_ids.frag);
			glDeleteShader(p_ids.vert);
			glDeleteProgram(p_ids.main);
		}

		return false;
	}

	glAttachShader(p_ids.main, p_ids.frag);
	glAttachShader(p_ids.main, p_ids.vert);

	// bind attributes before linking
	for (int i = 0; i < attribute_pair_count; i++) {
		glBindAttribLocation(p_ids.main, attribute_pairs[i].index, attribute_pairs[i].name);
	}

	//if feedback exists, set it up

	if (feedback_count) {
		Vector<const char *> feedback;
		for (int i = 0; i < feedback_count; i++) {
			// Use the mask of the version being compiled, not the shader's
			// current conditional_version: this may run on a worker thread (or
			// be driven by the poll) while conditional_version moved on.
			if (feedbacks[i].conditional == -1 || (1 << feedbacks[i].conditional) & p_conditional_mask) {
				//conditional for this feedback is enabled
				feedback.push_back(feedbacks[i].name);
			}
		}

		if (feedback.size()) {
			glTransformFeedbackVaryings(p_ids.main, feedback.size(), feedback.ptr(), GL_INTERLEAVED_ATTRIBS);
		}
	}

	if (p_retrievable) {
		glProgramParameteri(p_ids.main, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
	}
	glLinkProgram(p_ids.main);

	return true;
}

bool ShaderGLES3::_complete_link(Version::Ids p_ids, GLenum *r_program_format, PoolByteArray *r_program_binary) const {
	GLint status;
	glGetProgramiv(p_ids.main, GL_LINK_STATUS, &status);

	if (status == GL_FALSE) {
		// error linking
		GLsizei iloglen;
		glGetProgramiv(p_ids.main, GL_INFO_LOG_LENGTH, &iloglen);

		if (iloglen < 0) {
			glDeleteShader(p_ids.frag);
			glDeleteShader(p_ids.vert);
			glDeleteProgram(p_ids.main);
			ERR_FAIL_COND_V(iloglen < 0, false);
		}

		if (iloglen == 0) {
			iloglen = 4096; //buggy driver (Adreno 220+....)
		}

		char *ilogmem = (char *)Memory::alloc_static(iloglen + 1);
		ilogmem[iloglen] = 0;
		glGetProgramInfoLog(p_ids.main, iloglen, &iloglen, ilogmem);

		String err_string = get_shader_name() + ": Program LINK FAILED:\n";

		err_string += ilogmem;
		ERR_PRINT(err_string.ascii().get_data());
		Memory::free_static(ilogmem);
		glDeleteShader(p_ids.frag);
		glDeleteShader(p_ids.vert);
		glDeleteProgram(p_ids.main);

		return false;
	}

	if (r_program_binary) {
		GLint program_len;
		glGetProgramiv(p_ids.main, GL_PROGRAM_BINARY_LENGTH, &program_len);
		r_program_binary->resize(program_len);
		PoolByteArray::Write w = r_program_binary->write();
		glGetProgramBinary(p_ids.main, program_len, NULL, r_program_format, w.ptr());
	}

	return true;
}

void ShaderGLES3::_setup_uniforms(CustomCode *p_cc) const {
	//print_line("uniforms:  ");
	for (int j = 0; j < uniform_count; j++) {
		version->uniform_location[j] = glGetUniformLocation(version->ids.main, uniform_names[j]);
		//print_line("uniform "+String(uniform_names[j])+" location "+itos(version->uniform_location[j]));
	}

	// set texture uniforms
	for (int i = 0; i < texunit_pair_count; i++) {
		GLint loc = glGetUniformLocation(version->ids.main, texunit_pairs[i].name);
		if (loc >= 0) {
			if (texunit_pairs[i].index < 0) {
				glUniform1i(loc, max_image_units + texunit_pairs[i].index); //negative, goes down
			} else {
				glUniform1i(loc, texunit_pairs[i].index);
			}
		}
	}

	// assign uniform block bind points
	for (int i = 0; i < ubo_count; i++) {
		GLint loc = glGetUniformBlockIndex(version->ids.main, ubo_pairs[i].name);
		if (loc >= 0)
			glUniformBlockBinding(version->ids.main, loc, ubo_pairs[i].index);
	}

	if (p_cc) {
		version->texture_uniform_locations.resize(p_cc->texture_uniforms.size());
		for (int i = 0; i < p_cc->texture_uniforms.size(); i++) {
			version->texture_uniform_locations.write[i] = glGetUniformLocation(version->ids.main, String(p_cc->texture_uniforms[i]).ascii().get_data());
			glUniform1i(version->texture_uniform_locations[i], i + base_material_tex_index);
		}
	}
}

bool ShaderGLES3::_reuse_resident_program(Version *p_version) {
	if (!Engine::get_singleton()->is_shader_program_residency_enabled()) {
		return false;
	}
	ResidentProgram *resident = resident_programs.getptr(p_version->resident_program_key);
	if (!resident) {
		return false;
	}

	_claim_resident_program(resident);
	p_version->ids = resident->ids;
	p_version->program_binary.source = Version::ProgramBinary::SOURCE_NONE;
	p_version->compile_status = Version::COMPILE_STATUS_OK;
	p_version->uniforms_ready = false;
	p_version->diagnostic_resident_program_hit = true;
	return true;
}

void ShaderGLES3::_claim_resident_program(ResidentProgram *p_resident) {
	ERR_FAIL_NULL(p_resident);
	const String owner = Engine::get_singleton()->get_shader_program_residency_owner();
	if (owner.empty()) {
		return;
	}
	p_resident->owner_managed = true;
	p_resident->owners.insert(owner);
}

void ShaderGLES3::_retain_resident_program(Version *p_version) {
	if (!Engine::get_singleton()->is_shader_program_residency_enabled()) {
		return;
	}
	ERR_FAIL_COND(p_version->resident_program_key.empty());

	ResidentProgram *resident = resident_programs.getptr(p_version->resident_program_key);
	if (resident) {
		_claim_resident_program(resident);
		if (resident->ids.main != p_version->ids.main) {
			glDeleteShader(p_version->ids.vert);
			glDeleteShader(p_version->ids.frag);
			glDeleteProgram(p_version->ids.main);
			p_version->ids = resident->ids;
			p_version->uniforms_ready = false;
		}
		return;
	}

	ResidentProgram stored;
	stored.ids = p_version->ids;
	_claim_resident_program(&stored);
	resident_programs[p_version->resident_program_key] = stored;
	Engine::get_singleton()->notify_shader_program_retained();
}

uint32_t ShaderGLES3::_evict_resident_program(const String &p_program_key) {
	ResidentProgram *resident = resident_programs.getptr(p_program_key);
	ERR_FAIL_NULL_V(resident, 0);

	if (version && version->resident_program_key == p_program_key && version->ids.main == resident->ids.main) {
		if (active == this) {
			unbind();
		} else {
			version = nullptr;
		}
	}

	Vector<VersionKey> versions_to_remove;
	const VersionKey *version_key = nullptr;
	while ((version_key = version_map.next(version_key))) {
		const Version &candidate = version_map[*version_key];
		// Matching sources can still have a separate program compiling on the worker.
		if (candidate.resident_program_key == p_program_key && candidate.ids.main == resident->ids.main) {
			versions_to_remove.push_back(*version_key);
		}
	}

	for (int i = 0; i < versions_to_remove.size(); i++) {
		const VersionKey key = versions_to_remove[i];
		Version &candidate = version_map[key];
		if (key.code_version != CUSTOM_SHADER_DISABLED) {
			CustomCode *custom_code = custom_code_map.getptr(key.code_version);
			if (custom_code) {
				custom_code->versions.erase(key.version);
			}
		}
		_dispose_program(&candidate);
		memdelete_arr(candidate.uniform_location);
		version_map.erase(key);
	}

	glDeleteShader(resident->ids.vert);
	glDeleteShader(resident->ids.frag);
	glDeleteProgram(resident->ids.main);
	resident_programs.erase(p_program_key);
	Engine::get_singleton()->notify_shader_program_released();
	return versions_to_remove.size();
}

Dictionary ShaderGLES3::release_resident_program_owner(const String &p_owner) {
	Dictionary result;
	result["programs_released"] = 0;
	result["versions_invalidated"] = 0;
	if (p_owner.empty()) {
		result["success"] = false;
		result["error"] = "empty_owner";
		return result;
	}

	Vector<String> programs_to_release;
	const String *program_key = nullptr;
	while ((program_key = resident_programs.next(program_key))) {
		ResidentProgram &resident = resident_programs[*program_key];
		if (!resident.owner_managed || !resident.owners.has(p_owner)) {
			continue;
		}
		resident.owners.erase(p_owner);
		if (resident.owners.empty()) {
			programs_to_release.push_back(*program_key);
		}
	}

	uint32_t versions_invalidated = 0;
	for (int i = 0; i < programs_to_release.size(); i++) {
		versions_invalidated += _evict_resident_program(programs_to_release[i]);
	}
	result["success"] = true;
	result["programs_released"] = programs_to_release.size();
	result["versions_invalidated"] = versions_invalidated;
	return result;
}

void ShaderGLES3::_free_resident_programs() {
	const String *key = nullptr;
	while ((key = resident_programs.next(key))) {
		const ResidentProgram &resident = resident_programs[*key];
		glDeleteShader(resident.ids.vert);
		glDeleteShader(resident.ids.frag);
		glDeleteProgram(resident.ids.main);
		Engine::get_singleton()->notify_shader_program_released();
	}
	resident_programs.clear();
}

void ShaderGLES3::_dispose_program(Version *p_version) {
	_diagnostic_finish(p_version, false);
	if (compile_queue) {
		if (p_version->compile_status == Version::COMPILE_STATUS_PROCESSING_AT_QUEUE) {
			compile_queue->cancel(p_version->ids.main);
		}
	}
	const ResidentProgram *resident = resident_programs.getptr(p_version->resident_program_key);
	const bool is_resident = resident && resident->ids.main == p_version->ids.main;
	if (!is_resident) {
		glDeleteShader(p_version->ids.vert);
		glDeleteShader(p_version->ids.frag);
		glDeleteProgram(p_version->ids.main);
	}
	p_version->ids = Version::Ids();

	if (p_version->compiling_list.in_list()) {
		p_version->compiling_list.remove_from_list();
		active_compiles_count--;
#ifdef DEV_ENABLED
		CRASH_COND(active_compiles_count == UINT32_MAX);
#endif
		if (p_version->compile_status == Version::COMPILE_STATUS_COMPILING_VERTEX_AND_FRAGMENT) {
			active_compiles_count--;
#ifdef DEV_ENABLED
			CRASH_COND(active_compiles_count == UINT32_MAX);
#endif
		}
		_log_active_compiles();
	}
	p_version->compile_status = Version::COMPILE_STATUS_ERROR;
}

GLint ShaderGLES3::get_uniform_location(const String &p_name) const {
	ERR_FAIL_COND_V(!version, -1);
	return glGetUniformLocation(version->ids.main, p_name.ascii().get_data());
}

void ShaderGLES3::setup(const char **p_conditional_defines, int p_conditional_count, const char **p_uniform_names, int p_uniform_count, const AttributePair *p_attribute_pairs, int p_attribute_count, const TexUnitPair *p_texunit_pairs, int p_texunit_pair_count, const UBOPair *p_ubo_pairs, int p_ubo_pair_count, const Feedback *p_feedback, int p_feedback_count, const char *p_vertex_code, const char *p_fragment_code, int p_vertex_code_start, int p_fragment_code_start) {
	ERR_FAIL_COND(version);
	conditional_version.key = 0;
	new_conditional_version.key = 0;
	uniform_count = p_uniform_count;
	conditional_count = p_conditional_count;
	conditional_defines = p_conditional_defines;
	uniform_names = p_uniform_names;
	vertex_code = p_vertex_code;
	fragment_code = p_fragment_code;
	texunit_pairs = p_texunit_pairs;
	texunit_pair_count = p_texunit_pair_count;
	vertex_code_start = p_vertex_code_start;
	fragment_code_start = p_fragment_code_start;
	attribute_pairs = p_attribute_pairs;
	attribute_pair_count = p_attribute_count;
	ubo_pairs = p_ubo_pairs;
	ubo_count = p_ubo_pair_count;
	feedbacks = p_feedback;
	feedback_count = p_feedback_count;

	//split vertex and shader code (thank you, shader compiler programmers from you know what company).
	{
		String globals_tag = "\nVERTEX_SHADER_GLOBALS";
		String material_tag = "\nMATERIAL_UNIFORMS";
		String code_tag = "\nVERTEX_SHADER_CODE";
		String code = vertex_code;
		int cpos = code.find(material_tag);
		if (cpos == -1) {
			vertex_code0 = code.ascii();
		} else {
			vertex_code0 = code.substr(0, cpos).ascii();
			code = code.substr(cpos + material_tag.length(), code.length());

			cpos = code.find(globals_tag);

			if (cpos == -1) {
				vertex_code1 = code.ascii();
			} else {
				vertex_code1 = code.substr(0, cpos).ascii();
				String code2 = code.substr(cpos + globals_tag.length(), code.length());

				cpos = code2.find(code_tag);
				if (cpos == -1) {
					vertex_code2 = code2.ascii();
				} else {
					vertex_code2 = code2.substr(0, cpos).ascii();
					vertex_code3 = code2.substr(cpos + code_tag.length(), code2.length()).ascii();
				}
			}
		}
	}

	{
		String globals_tag = "\nFRAGMENT_SHADER_GLOBALS";
		String material_tag = "\nMATERIAL_UNIFORMS";
		String code_tag = "\nFRAGMENT_SHADER_CODE";
		String light_code_tag = "\nLIGHT_SHADER_CODE";
		String code = fragment_code;
		int cpos = code.find(material_tag);
		if (cpos == -1) {
			fragment_code0 = code.ascii();
		} else {
			fragment_code0 = code.substr(0, cpos).ascii();
			//print_line("CODE0:\n"+String(fragment_code0.get_data()));
			code = code.substr(cpos + material_tag.length(), code.length());
			cpos = code.find(globals_tag);

			if (cpos == -1) {
				fragment_code1 = code.ascii();
			} else {
				fragment_code1 = code.substr(0, cpos).ascii();
				//print_line("CODE1:\n"+String(fragment_code1.get_data()));

				String code2 = code.substr(cpos + globals_tag.length(), code.length());
				cpos = code2.find(light_code_tag);

				if (cpos == -1) {
					fragment_code2 = code2.ascii();
				} else {
					fragment_code2 = code2.substr(0, cpos).ascii();
					//print_line("CODE2:\n"+String(fragment_code2.get_data()));

					String code3 = code2.substr(cpos + light_code_tag.length(), code2.length());

					cpos = code3.find(code_tag);
					if (cpos == -1) {
						fragment_code3 = code3.ascii();
					} else {
						fragment_code3 = code3.substr(0, cpos).ascii();
						//print_line("CODE3:\n"+String(fragment_code3.get_data()));
						fragment_code4 = code3.substr(cpos + code_tag.length(), code3.length()).ascii();
						//print_line("CODE4:\n"+String(fragment_code4.get_data()));
					}
				}
			}
		}
	}

	// The upper limit must match the version used in storage.
	max_image_units = RasterizerStorageGLES3::safe_gl_get_integer(GL_MAX_TEXTURE_IMAGE_UNITS, RasterizerStorageGLES3::Config::max_desired_texture_image_units);
}

void ShaderGLES3::init_async_compilation() {
	if (is_async_compilation_supported() && get_ubershader_flags_uniform() != -1) {
		// Warm up the ubershader for the case of no custom code
		new_conditional_version.code_version = 0;
		_bind_ubershader(true);
	}
}

bool ShaderGLES3::is_async_compilation_supported() const {
	return max_simultaneous_compiles > 0 && (compile_queue || parallel_compile_supported);
}

void ShaderGLES3::_cancel_pending_recipe_jobs() {
	for (int i = recipe_jobs.size() - 1; i >= 0; i--) {
		if (recipe_jobs[i].owner == this) {
			if (recipe_compile_queue) {
				recipe_compile_queue->cancel_compile(recipe_jobs[i].job_id);
			}
			recipe_jobs.remove(i);
		}
	}
	// Jobs already completed in the result map are simply never consumed.
}

void ShaderGLES3::finish() {
	_cancel_pending_recipe_jobs();
	const VersionKey *V = nullptr;
	while ((V = version_map.next(V))) {
		Version &v = version_map[*V];
		_dispose_program(&v);
		memdelete_arr(v.uniform_location);
	}
	_free_resident_programs();
	ERR_FAIL_COND(versions_compiling.first());
	ERR_FAIL_COND(active_compiles_count != 0);
}

void ShaderGLES3::clear_caches() {
	const VersionKey *V = nullptr;
	while ((V = version_map.next(V))) {
		Version &v = version_map[*V];
		_dispose_program(&v);
		memdelete_arr(v.uniform_location);
	}
	ERR_FAIL_COND(versions_compiling.first());
	ERR_FAIL_COND(active_compiles_count != 0);

	version_map.clear();

	custom_code_map.clear();
	version = nullptr;
	last_custom_code = 1;
}

uint32_t ShaderGLES3::create_custom_shader() {
	custom_code_map[last_custom_code] = CustomCode();
	custom_code_map[last_custom_code].version = 1;
	return last_custom_code++;
}

void ShaderGLES3::set_custom_shader_code(uint32_t p_code_id, const String &p_vertex, const String &p_vertex_globals, const String &p_fragment, const String &p_light, const String &p_fragment_globals, const String &p_uniforms, const Vector<StringName> &p_texture_uniforms, const Vector<CharString> &p_custom_defines, AsyncMode p_async_mode) {
	ERR_FAIL_COND(!custom_code_map.has(p_code_id));
	CustomCode *cc = &custom_code_map[p_code_id];

	cc->vertex = p_vertex;
	cc->vertex_globals = p_vertex_globals;
	cc->fragment = p_fragment;
	cc->fragment_globals = p_fragment_globals;
	cc->light = p_light;
	cc->texture_uniforms = p_texture_uniforms;
	cc->uniforms = p_uniforms;
	cc->custom_defines = p_custom_defines;
	cc->async_mode = p_async_mode;
	cc->version++;

	if (p_async_mode == ASYNC_MODE_VISIBLE && is_async_compilation_supported() && get_ubershader_flags_uniform() != -1) {
		// Warm up the ubershader for this custom code
		diagnostic_material_path = String();
		new_conditional_version.code_version = p_code_id;
		_bind_ubershader(true);
	}
}

void ShaderGLES3::set_custom_shader(uint32_t p_code_id, const String &p_material_path) {
	new_conditional_version.code_version = p_code_id;
	diagnostic_material_path = p_material_path;
}

static String _shader_conditional_name(const char *p_define) {
	String name = String(p_define).strip_edges().trim_prefix("#define").strip_edges();
	for (int i = 0; i < name.length(); i++) {
		if (name[i] <= 32) {
			return name.substr(0, i);
		}
	}
	return name;
}

Dictionary ShaderGLES3::precompile_custom_shader_variant(uint32_t p_code_id, const PoolStringArray &p_enabled_conditionals, const String &p_material_path) {
	Dictionary result;
	result["success"] = false;
	result["shader_name"] = get_shader_name();
	result["material_path"] = p_material_path;

	CustomCode *custom_code = custom_code_map.getptr(p_code_id);
	if (p_code_id != CUSTOM_SHADER_DISABLED && !custom_code) {
		result["error"] = "invalid_custom_code_id";
		return result;
	}

	PoolStringArray available_conditionals;
	for (int i = 0; i < conditional_count; i++) {
		available_conditionals.append(_shader_conditional_name(conditional_defines[i]));
	}
	result["available_conditionals"] = available_conditionals;

	uint32_t requested_variant = 0;
	PoolStringArray normalized_conditionals;
	PoolStringArray::Read requested = p_enabled_conditionals.read();
	for (int requested_index = 0; requested_index < p_enabled_conditionals.size(); requested_index++) {
		String requested_name = requested[requested_index].strip_edges().trim_prefix("#define").strip_edges();
		bool found = false;
		for (int conditional_index = 0; conditional_index < conditional_count; conditional_index++) {
			const String conditional_name = _shader_conditional_name(conditional_defines[conditional_index]);
			if (requested_name == conditional_name) {
				requested_variant |= uint32_t(1) << conditional_index;
				normalized_conditionals.append(conditional_name);
				found = true;
				break;
			}
		}
		if (!found) {
			result["error"] = "unknown_conditional";
			result["unknown_conditional"] = requested_name;
			return result;
		}
	}

	result["enabled_conditionals"] = normalized_conditionals;
	result["variant"] = requested_variant;
	result["custom_code_id"] = p_code_id;
	result["custom_code_version"] = custom_code ? custom_code->version : 0;

	VersionKey requested_key;
	requested_key.version = requested_variant;
	requested_key.code_version = p_code_id;
	const Version *existing = version_map.getptr(requested_key);
	const uint32_t requested_code_version = custom_code ? custom_code->version : 0;
	const bool already_compiled = existing && existing->code_version == requested_code_version && existing->compile_status == Version::COMPILE_STATUS_OK;

	const VersionKey previous_requested_version = new_conditional_version;
	const String previous_material_path = diagnostic_material_path;
	if (active) {
		active->unbind();
	}
	new_conditional_version = requested_key;
	diagnostic_material_path = p_material_path;
	const bool bound = _bind(true);

	Version *compiled_version = version;
	const bool success = bound && compiled_version && compiled_version->compile_status == Version::COMPILE_STATUS_OK;
	if (success) {
		_retain_resident_program(compiled_version);
	}
	result["success"] = success;
	result["already_compiled"] = already_compiled;
	result["resident_program_count"] = Engine::get_singleton()->get_shader_resident_program_count();
	if (compiled_version) {
		result["program_cache_key"] = compiled_version->resident_program_key;
		result["resident_program_hit"] = compiled_version->diagnostic_resident_program_hit;
		result["operation"] = already_compiled ? "already_compiled" : compiled_version->diagnostic_operation;
	}
	if (!success) {
		result["error"] = "shader_compilation_failed";
	}

	unbind();
	new_conditional_version = previous_requested_version;
	diagnostic_material_path = previous_material_path;
	return result;
}

void ShaderGLES3::free_custom_shader(uint32_t p_code_id) {
	ERR_FAIL_COND(!custom_code_map.has(p_code_id));
	if (conditional_version.code_version == p_code_id) {
		conditional_version.code_version = 0; //do not keep using a version that is going away
		unbind();
	}

	VersionKey key;
	key.code_version = p_code_id;
	for (Set<uint32_t>::Element *E = custom_code_map[p_code_id].versions.front(); E; E = E->next()) {
		key.version = E->get();
		ERR_CONTINUE(!version_map.has(key));
		Version &v = version_map[key];

		_dispose_program(&v);
		memdelete_arr(v.uniform_location);

		version_map.erase(key);
	}

	custom_code_map.erase(p_code_id);
}

void ShaderGLES3::set_base_material_tex_index(int p_idx) {
	base_material_tex_index = p_idx;
}

ShaderGLES3::ShaderGLES3() {
	version = nullptr;
	last_custom_code = 1;
	base_material_tex_index = 0;
}

ShaderGLES3::~ShaderGLES3() {
	finish();
}
