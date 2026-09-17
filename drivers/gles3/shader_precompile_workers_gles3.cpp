/**************************************************************************/
/*  shader_precompile_workers_gles3.cpp                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                   */
/*                                                                         */
/* Permission is hereby granted, free of charge, to any person obtaining   */
/* a copy of this software and associated documentation files (the         */
/* "Software"), to deal in the Software without restriction, including     */
/* without limitation the rights to use, copy, modify, merge, publish,     */
/* distribute, sublicense, and/or sell copies of the Software, and to      */
/* permit persons to whom the Software is furnished to do so, subject to   */
/* the following conditions:                                               */
/*                                                                         */
/* The above copyright notice and this permission notice shall be          */
/* included in all copies or substantial portions of the Software.         */
/*                                                                         */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,         */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF      */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY    */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,    */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE       */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                  */
/**************************************************************************/

#include "shader_precompile_workers_gles3.h"

#include "core/os/os.h"

ShaderPrecompileWorkersGLES3 *ShaderPrecompileWorkersGLES3::singleton = nullptr;

// Opaque handle to the per-worker platform context (own connection/window on
// X11, own hidden window and device context on Windows), created through the
// OS::create_worker_gl_context abstraction.
thread_local void *ShaderPrecompileWorkersGLES3::worker_handle = nullptr;

ShaderPrecompileWorkersGLES3::ShaderPrecompileWorkersGLES3(int p_worker_count) :
		round_robin(0) {
	for (int i = 0; i < p_worker_count; i++) {
		ThreadedCallableQueue<unsigned int> *worker = memnew(ThreadedCallableQueue<unsigned int>);
		// First job on the worker thread: create the platform worker context.
		worker->enqueue(make_inner_key(i), [this]() { setup_worker(); });
		inner_queues.push_back(worker);
	}
}

uint64_t ShaderPrecompileWorkersGLES3::make_inner_key(int p_worker_index) {
	// Top bits encode the owning queue so cancel() can find it; the low bits
	// are a process-wide sequence that never repeats.
	static SafeNumeric<uint64_t> sequence;
	return (uint64_t)p_worker_index << 48 | (sequence.increment() & 0xFFFFFFFFFFFFULL);
}

void ShaderPrecompileWorkersGLES3::setup_worker() {
	ERR_FAIL_COND_MSG(worker_handle != nullptr, "Worker GL context already set up");
	void *handle = nullptr;
	ERR_FAIL_COND_MSG(OS::get_singleton()->create_worker_gl_context(&handle) != OK || !handle,
			"Shader compile worker: platform GL context creation failed");
	worker_handle = handle;
}

void ShaderPrecompileWorkersGLES3::teardown_worker() {
	if (worker_handle) {
		OS::get_singleton()->release_worker_gl_context_current(worker_handle);
		OS::get_singleton()->destroy_worker_gl_context(worker_handle);
		worker_handle = nullptr;
	}
}

void ShaderPrecompileWorkersGLES3::enqueue_compile(unsigned int p_program, const ThreadedCallableQueue<unsigned int>::Job &p_job) {
	// Called from the visual server thread: choose a worker, key the job and
	// hand it over. The job itself runs on the worker thread that owns the GL
	// context.
	uint32_t queue_index = 0;
	uint64_t inner_key = 0;
	{
		MutexLock lock(queue_mutex);
		queue_index = round_robin;
		round_robin = (round_robin + 1) % inner_queues.size();
		inner_key = make_inner_key(queue_index);
	}
	inner_queues[queue_index]->enqueue((unsigned int)inner_key, p_job);
	{
		MutexLock lock(queue_mutex);
		program_keys[p_program] = inner_key;
	}
	job_results.register_job(p_program);
}

void ShaderPrecompileWorkersGLES3::cancel_compile(unsigned int p_program) {
	uint64_t inner_key = 0;
	{
		MutexLock lock(queue_mutex);
		uint64_t *stored = program_keys.getptr(p_program);
		if (!stored) {
			return;
		}
		inner_key = *stored;
		program_keys.erase(p_program);
	}
	int worker_index = int(inner_key >> 48);
	inner_queues[worker_index]->cancel((unsigned int)(inner_key & 0xFFFFFFFFFFFFULL));
	job_results.discard_job(p_program);
}

void ShaderPrecompileWorkersGLES3::complete_job(unsigned int p_key, GLenum p_format, const PoolByteArray &p_data) {
	job_results.complete_job(p_key, p_format, p_data);
}

bool ShaderPrecompileWorkersGLES3::poll_job_result(unsigned int p_key, GLenum *r_format, PoolByteArray *r_data) {
	return job_results.poll_job_result(p_key, r_format, r_data);
}

bool ShaderPrecompileWorkersGLES3::create_workers(int p_worker_count) {
	ERR_FAIL_COND_V(p_worker_count <= 0, false);
	if (singleton) {
		return true;
	}
	if (!OS::get_singleton()->can_create_worker_gl_context()) {
		// No worker context support on this platform: keep recipe compilation
		// on the synchronous path.
		return false;
	}
	singleton = memnew(ShaderPrecompileWorkersGLES3(p_worker_count));
	return true;
}

void ShaderPrecompileWorkersGLES3::release_workers() {
	if (!singleton) {
		return;
	}
	ShaderPrecompileWorkersGLES3 *workers = singleton;
	singleton = nullptr;
	// Each worker queue drains its pending jobs on its own thread before the
	// destructor finishes, so enqueueing teardown first is enough.
	for (int i = 0; i < workers->inner_queues.size(); i++) {
		workers->inner_queues[i]->enqueue(make_inner_key(i), [workers]() { workers->teardown_worker(); });
	}
	for (int i = 0; i < workers->inner_queues.size(); i++) {
		memdelete(workers->inner_queues[i]);
	}
	memdelete(workers);
}
