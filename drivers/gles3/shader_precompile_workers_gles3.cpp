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

// GL and X headers come after the core headers: X11 defines None/Status/Bool
// and friends that clash with engine enums if included first.
#include <GL/glx.h>
#include <X11/Xlib.h>

#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092

typedef GLXContext (*GLXCREATECONTEXTATTRIBSARBPROC)(Display *, GLXFBConfig, GLXContext, Bool, const int *);

ShaderPrecompileWorkersGLES3 *ShaderPrecompileWorkersGLES3::singleton = nullptr;

thread_local void *ShaderPrecompileWorkersGLES3::worker_display = nullptr;
thread_local unsigned long ShaderPrecompileWorkersGLES3::worker_window = 0;
thread_local void *ShaderPrecompileWorkersGLES3::worker_context = nullptr;

ShaderPrecompileWorkersGLES3::ShaderPrecompileWorkersGLES3(int p_worker_count) :
		round_robin(0) {
	for (int i = 0; i < p_worker_count; i++) {
		ThreadedCallableQueue<unsigned int> *worker = memnew(ThreadedCallableQueue<unsigned int>);
		// First job on the worker thread: open its own X connection, create a
		// hidden window and a GLX context. Sharing one Display across threads
		// corrupts Xlib even with XInitThreads, so each worker owns its own.
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
	ERR_FAIL_COND_MSG(worker_display != nullptr, "Worker GL context already set up");
	Display *display = XOpenDisplay(nullptr);
	ERR_FAIL_COND_MSG(!display, "Shader compile worker: XOpenDisplay failed");
	int screen = DefaultScreen(display);

	// Same FBConfig and context attributes as the main GLX context
	// (GLES_3_0_COMPATIBLE branch in ContextGL_X11): program binaries must be
	// compatible between worker and main contexts.
	static int visual_attribs[] = {
		GLX_RENDER_TYPE, GLX_RGBA_BIT,
		GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
		GLX_DOUBLEBUFFER, true,
		GLX_RED_SIZE, 1,
		GLX_GREEN_SIZE, 1,
		GLX_BLUE_SIZE, 1,
		GLX_DEPTH_SIZE, 24,
		None
	};
	static int context_attribs[] = {
		GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
		GLX_CONTEXT_MINOR_VERSION_ARB, 3,
		GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
		GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
		None
	};

	GLXCREATECONTEXTATTRIBSARBPROC glXCreateContextAttribsARB =
			(GLXCREATECONTEXTATTRIBSARBPROC)glXGetProcAddress((const GLubyte *)"glXCreateContextAttribsARB");
	ERR_FAIL_COND_MSG(!glXCreateContextAttribsARB, "Shader compile worker: glXCreateContextAttribsARB missing");

	int fbcount = 0;
	GLXFBConfig *fbc = glXChooseFBConfig(display, screen, visual_attribs, &fbcount);
	ERR_FAIL_COND_MSG(!fbc || fbcount == 0, "Shader compile worker: glXChooseFBConfig failed");
	GLXFBConfig fbconfig = fbc[0];
	XVisualInfo *vi = glXGetVisualFromFBConfig(display, fbconfig);
	XFree(fbc);
	ERR_FAIL_COND_MSG(!vi, "Shader compile worker: no visual for FBConfig");

	XSetWindowAttributes swa;
	swa.event_mask = StructureNotifyMask;
	swa.border_pixel = 0;
	swa.background_pixmap = None;
	swa.background_pixel = 0;
	swa.colormap = XCreateColormap(display, RootWindow(display, vi->screen), vi->visual, AllocNone);
	Window window = XCreateWindow(display, RootWindow(display, vi->screen), 0, 0, 32, 32, 0, vi->depth, InputOutput, vi->visual, CWBorderPixel | CWColormap | CWEventMask | CWBackPixel, &swa);
	GLXContext context = glXCreateContextAttribsARB(display, fbconfig, nullptr, true, context_attribs);
	ERR_FAIL_COND_MSG(!context || !glXMakeCurrent(display, window, context), "Shader compile worker: GLX context failed");

	worker_display = display;
	worker_window = (unsigned long)window;
	worker_context = context;
}

void ShaderPrecompileWorkersGLES3::teardown_worker() {
	if (worker_context) {
		glXMakeCurrent((Display *)worker_display, None, nullptr);
		glXDestroyContext((Display *)worker_display, (GLXContext)worker_context);
		worker_context = nullptr;
	}
	if (worker_window != 0) {
		XDestroyWindow((Display *)worker_display, (Window)worker_window);
		worker_window = 0;
	}
	if (worker_display) {
		XCloseDisplay((Display *)worker_display);
		worker_display = nullptr;
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
