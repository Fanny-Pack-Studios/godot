/**************************************************************************/
/*  shader_precompile_workers_gles3.h                                     */
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

#ifndef SHADER_PRECOMPILE_WORKERS_GLES3_H
#define SHADER_PRECOMPILE_WORKERS_GLES3_H

#include "core/safe_refcount.h"
#include "core/threaded_callable_queue.h"
#include "shader_gles3.h"

// Shared job result registry: workers publish binaries by key, the state
// machine consumes them. This keeps worker lambdas free of Version pointers
// (the version_map may rehash and move values while a job is queued).
class CompileJobResultMap {
private:
	struct JobResult {
		GLenum format;
		PoolByteArray data; // empty means failure
		bool finished;
	};
	HashMap<unsigned int, JobResult> results;
	BinaryMutex results_mutex;

public:
	void register_job(unsigned int p_key) {
		MutexLock lock(results_mutex);
		JobResult result;
		result.finished = false;
		results[p_key] = result;
	}

	void complete_job(unsigned int p_key, GLenum p_format, const PoolByteArray &p_data) {
		MutexLock lock(results_mutex);
		JobResult *result = results.getptr(p_key);
		if (!result) {
			return; // cancelled while compiling
		}
		result->format = p_format;
		result->data = p_data;
		result->finished = true;
	}

	bool poll_job_result(unsigned int p_key, GLenum *r_format, PoolByteArray *r_data) {
		MutexLock lock(results_mutex);
		JobResult *result = results.getptr(p_key);
		if (!result || !result->finished) {
			return false;
		}
		*r_format = result->format;
		*r_data = result->data;
		results.erase(p_key);
		return true;
	}

	void discard_job(unsigned int p_key) {
		MutexLock lock(results_mutex);
		results.erase(p_key);
	}
};

// Pool of N shader compile workers, each with its own platform GL context
// (created via OS::create_worker_gl_context: GLX with an own X connection on
// X11, WGL with an own hidden window on Windows). The driver serializes GL
// compile threads that share one context, so an external worker pool is the
// only way to compile a large batch of variants in parallel. Platforms without
// worker context support never create the pool, and recipe compilation stays
// synchronous. Programs are transferred to the main context as binaries by the
// existing SOURCE_QUEUE path in ShaderGLES3.
class ShaderPrecompileWorkersGLES3 : public ShaderCompileQueueGLES3 {
private:
	Vector<ThreadedCallableQueue<unsigned int> *> inner_queues;
	CompileJobResultMap job_results;
	HashMap<unsigned int, uint64_t> program_keys; // job key -> (worker index << 48 | local key)
	uint32_t round_robin;
	BinaryMutex queue_mutex;

	// Opaque handle to the per-thread platform worker context (the platform
	// headers stay out of the header to avoid macro clashes).
	static thread_local void *worker_handle;

	void setup_worker();
	void teardown_worker();

	ShaderPrecompileWorkersGLES3(int p_worker_count);

	static uint64_t make_inner_key(int p_worker_index); // atomic sequence below

public:
	static ShaderPrecompileWorkersGLES3 *singleton;

	virtual void enqueue_compile(unsigned int p_program, const ThreadedCallableQueue<unsigned int>::Job &p_job);
	virtual void cancel_compile(unsigned int p_program);
	virtual void complete_job(unsigned int p_key, GLenum p_format, const PoolByteArray &p_data);
	virtual bool poll_job_result(unsigned int p_key, GLenum *r_format, PoolByteArray *r_data);

	static bool create_workers(int p_worker_count);
	static void release_workers();
};

#endif // SHADER_PRECOMPILE_WORKERS_GLES3_H
