#define  _GNU_SOURCE

#include <ctype.h>
#include <features.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <assert.h>
#include <time.h>
#include <getopt.h>
#include <elf.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/input.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#define  EGL_EGLEXT_PROTOTYPES
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#define  GL_GLEXT_PROTOTYPES
#include <GLES2/gl2ext.h>
#include <libinput.h>
#include <libudev.h>
#include <systemd/sd-event.h>
#include <flutter_embedder.h>

#include <flutter-pi.h>
#include <compositor.h>
#include <keyboard.h>
#include <messenger.h>
#include <pluginregistry.h>
#include <texture_registry.h>
#include <dylib_deps.h>
#include <user_input.h>
#include <renderer.h>
#include <event_loop.h>

#include <plugins/text_input.h>
#include <plugins/raw_keyboard.h>

const char *const usage ="\
flutter-pi - run flutter apps on your Raspberry Pi.\n\
\n\
USAGE:\n\
  flutter-pi [options] <asset bundle path> [flutter engine options]\n\
\n\
OPTIONS:\n\
  --release                  Run the app in release mode. The AOT snapshot\n\
                             of the app (\"app.so\") must be located inside the\n\
                             asset bundle directory.\n\
                             This also requires a libflutter_engine.so that was\n\
                             built with --runtime-mode=release.\n\
                             \n\
  -o, --orientation <orientation>  Start the app in this orientation. Valid\n\
                             for <orientation> are: portrait_up, landscape_left,\n\
                             portrait_down, landscape_right.\n\
                             For more information about this orientation, see\n\
                             the flutter docs for the \"DeviceOrientation\"\n\
                             enum.\n\
                             Only one of the --orientation and --rotation\n\
                             options can be specified.\n\
                             \n\
  -r, --rotation <degrees>   Start the app with this rotation. This is just an\n\
                             alternative, more intuitive way to specify the\n\
                             startup orientation. The angle is in degrees and\n\
                             clock-wise.\n\
                             Valid values are 0, 90, 180 and 270.\n\
                             \n\
  -d, --dimensions \"width_mm,height_mm\" The width & height of your display in\n\
                             millimeters. Useful if your GPU doesn't provide\n\
                             valid physical dimensions for your display.\n\
                             The physical dimensions of your display are used\n\
                             to calculate the flutter device-pixel-ratio, which\n\
                             in turn basically \"scales\" the UI.\n\
                             \n\
  -i, --input <glob pattern> Appends all files matching this glob pattern to the\n\
                             list of input (touchscreen, mouse, touchpad, \n\
                             keyboard) devices. Brace and tilde expansion is \n\
                             enabled.\n\
                             Every file that matches this pattern, but is not\n\
                             a valid touchscreen / -pad, mouse or keyboard is \n\
                             silently ignored.\n\
                             If no -i options are given, flutter-pi will try to\n\
                             use all input devices assigned to udev seat0.\n\
                             If that fails, or udev is not installed, flutter-pi\n\
                             will fallback to using all devices matching \n\
                             \"/dev/input/event*\" as inputs.\n\
                             In most cases, there's no need to specify this\n\
                             option.\n\
                             Note that you need to properly escape each glob \n\
                             pattern you use as a parameter so it isn't \n\
                             implicitly expanded by your shell.\n\
                             \n\
  -h, --help                 Show this help and exit.\n\
\n\
EXAMPLES:\n\
  flutter-pi ~/hello_world_app\n\
  flutter-pi --release ~/hello_world_app\n\
  flutter-pi -o portrait_up ./my_app\n\
  flutter-pi -r 90 ./my_app\n\
  flutter-pi -d \"155, 86\" ./my_app\n\
  flutter-pi -i \"/dev/input/event{0,1}\" -i \"/dev/input/event{2,3}\" /home/pi/helloworld_flutterassets\n\
  flutter-pi -i \"/dev/input/mouse*\" /home/pi/helloworld_flutterassets\n\
\n\
SEE ALSO:\n\
  Author:  Hannes Winkler, a.k.a ardera\n\
  Source:  https://github.com/ardera/flutter-pi\n\
  License: MIT\n\
\n\
  For instructions on how to build an asset bundle or an AOT snapshot\n\
    of your app, please see the linked github repository.\n\
  For a list of options you can pass to the flutter engine, look here:\n\
    https://github.com/flutter/engine/blob/master/shell/common/switches.h\n\
";

struct flutterpi_flutter_task {
	struct flutterpi *flutterpi;
	FlutterTask task;
};

struct flutterpi_private {
	struct flutterpi *flutterpi;
	struct text_input_plugin *textin;
	struct raw_keyboard_plugin *rawkb;
	struct renderer *renderer;
	struct compositor *compositor;
	struct event_loop *platform, *render;
};

#define FLUTTERPI_PRIVATE(flutterpi) ((struct flutterpi_private*) (flutterpi)->private)

struct flutterpi_frame_request {
	struct flutterpi *flutterpi;
	intptr_t baton;
};

// OpenGL contexts are thread-local. So this needs to be thread-local as well.
static bool on_flutter_gl_make_current(void* userdata) {
	struct flutterpi *flutterpi = userdata;
	return gl_renderer_flutter_make_rendering_context_current(flutterpi->renderer);
}

static bool on_flutter_gl_clear_current(void *userdata) {
	struct flutterpi *flutterpi = userdata;
	return gl_renderer_flutter_make_rendering_context_current(flutterpi->renderer);
}

static bool on_flutter_gl_present(void *userdata) {
	return gl_renderer_flutter_present(((struct flutterpi*) userdata)->renderer);
}

static uint32_t on_flutter_gl_get_fbo(void* userdata) {
	return gl_renderer_flutter_get_fbo(((struct flutterpi *) userdata)->renderer);
}

static bool on_flutter_gl_make_resource_context_current(void *userdata) {
	struct flutterpi *flutterpi = userdata;
	return gl_renderer_flutter_make_resource_context_current(flutterpi->renderer);
}

static FlutterTransformation on_flutter_gl_get_surface_transformation(void *userdata) {
	return gl_renderer_flutter_get_surface_transformation(((struct flutterpi *) userdata)->renderer);
}

static void *on_flutter_gl_resolve_proc(void *userdata, const char *name) {
	return gl_renderer_flutter_resolve_gl_proc(((struct flutterpi *) userdata)->renderer, name);
}

static bool on_flutter_gl_get_external_texture_frame(
	void *userdata,
	int64_t texture_id,
	size_t width, size_t height,
	FlutterOpenGLTexture *texture_out
) {
	return texreg_on_external_texture_frame_callback(
		((struct flutterpi *) userdata)->texture_registry,
		texture_id,
		width, height,
		texture_out
	) == 0;
}

static uint32_t on_flutter_gl_get_fbo_with_info(void *userdata, const FlutterFrameInfo *info) {
	return gl_renderer_flutter_get_fbo_with_info(((struct flutterpi *) userdata)->renderer, info);
}

bool on_flutter_gl_present_with_info(void *userdata, const FlutterPresentInfo *info) {
	return gl_renderer_flutter_present_with_info(((struct flutterpi *) userdata)->renderer, info);
}

static const struct flutter_renderer_gl_interface gl_interface = {
	.make_current = on_flutter_gl_make_current,
	.clear_current = on_flutter_gl_clear_current,
	.present = on_flutter_gl_present,
	.fbo_callback = on_flutter_gl_get_fbo,
	.make_resource_current = on_flutter_gl_make_resource_context_current,
	.surface_transformation = on_flutter_gl_get_surface_transformation,
	.gl_proc_resolver = on_flutter_gl_resolve_proc,
	.gl_external_texture_frame_callback = on_flutter_gl_get_external_texture_frame,
	.fbo_with_frame_info_callback = on_flutter_gl_get_fbo_with_info,
	.present_with_info = on_flutter_gl_present_with_info
};

bool on_flutter_sw_present(void *userdata, const void *allocation, size_t bytes_per_row, size_t height) {
	return sw_renderer_flutter_present(((struct flutterpi *) userdata)->renderer, allocation, bytes_per_row, height);
}

static const struct flutter_renderer_sw_interface sw_interface = {
	.surface_present_callback = on_flutter_sw_present
};

static bool runs_platform_tasks_on_current_thread(void *userdata);

/// Cut a word from a string, mutating "string"
static void cut_word_from_string(
	char* string,
	const char* word
) {
	size_t word_length = strlen(word);
	char*  word_in_str = strstr(string, word);

	// check if the given word is surrounded by spaces in the string
	if (word_in_str
		&& ((word_in_str == string) || (word_in_str[-1] == ' '))
		&& ((word_in_str[word_length] == 0) || (word_in_str[word_length] == ' '))
	) {
		if (word_in_str[word_length] == ' ') word_length++;

		int i = 0;
		do {
			word_in_str[i] = word_in_str[i+word_length];
		} while (word_in_str[i++ + word_length] != 0);
	}
}


static void on_platform_message(
	const FlutterPlatformMessage* message,
	void* userdata
) {
	struct flutterpi *fpi;
	int ok;

	fpi = userdata;

	DEBUG_ASSERT(fpi != NULL);

	ok = fm_on_platform_message(fpi->flutter_messenger, message->response_handle, message->channel, message->message, message->message_size);
	if (ok != 0) {
		LOG_FLUTTERPI_ERROR("Error handling platform message. fm_on_platform_message: %s\n", strerror(ok));
	}
}

static void on_begin_frame(uint64_t frame_nanos, uint64_t next_frame_nanos, void *userdata) {
	struct flutterpi_frame_request *req;
	struct flutterpi *flutterpi;
	intptr_t baton;

	req = userdata;
	flutterpi = req->flutterpi;
	baton = req->baton;

	DEBUG_ASSERT(req != NULL);

	/// FIXME: We need to make sure this is called on the platform thread.
	flutterpi->flutter.libflutter_engine->FlutterEngineOnVsync(
		flutterpi->flutter.engine,
		frame_nanos,
		next_frame_nanos,
		baton
	);

	free(req);	
}

/// Called on some flutter internal thread to request a frame,
/// and also get the vblank timestamp of the pageflip preceding that frame.
static void on_frame_request(void* userdata, intptr_t baton) {
	struct flutterpi_frame_request *req;
	struct flutterpi *flutterpi;
	int ok;

	flutterpi = userdata;

	DEBUG_ASSERT(flutterpi != NULL);

	req = malloc(sizeof *req);
	DEBUG_ASSERT(req != NULL);

	req->flutterpi = flutterpi;
	req->baton = baton;
	
	ok = compositor_request_frame(flutterpi->compositor, on_begin_frame, req);
	DEBUG_ASSERT(ok == 0);
	(void) ok;
}

struct event_loop *flutterpi_get_event_loop(struct flutterpi *flutterpi, enum event_loop_type type) {
	struct flutterpi_private *private;

	DEBUG_ASSERT(flutterpi != NULL);
	private = FLUTTERPI_PRIVATE(flutterpi);

	if (type == kPlatform) {
		return private->platform;
	} else if (type == kRender) {
		return private->render;
	} else {
		DEBUG_ASSERT(false);
		return NULL;
	}
}

static FlutterTransformation flutterpi_on_get_transformation(void *userdata) {
	struct flutterpi *fpi;
	
	fpi = userdata;
	
	return fpi->view.view_to_display_transform;
}

/// flutter tasks
static void on_execute_flutter_task(
	void *userdata
) {
	struct flutterpi_flutter_task *task;
	FlutterEngineResult engine_result;

	task = userdata;

	engine_result = task->flutterpi->flutter.libflutter_engine->FlutterEngineRunTask(task->flutterpi->flutter.engine, &task->task);
	if (engine_result != kSuccess) {
		LOG_FLUTTERPI_ERROR("Error running platform task. FlutterEngineRunTask: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
	}

	free(task);
}

static void on_post_flutter_task(
	FlutterTask task,
	uint64_t target_time,
	void *userdata
) {
	struct flutterpi_flutter_task *fpi_task;
	struct flutterpi *flutterpi;
	int ok;

	flutterpi = userdata;

	fpi_task = malloc(sizeof *fpi_task);
	if (fpi_task == NULL) {
		return;
	}
	
	fpi_task->task = task;
	fpi_task->flutterpi = flutterpi;

	ok = flutterpi_post_platform_task_with_time(
		flutterpi,
		on_execute_flutter_task,
		fpi_task,
		target_time / 1000
	);
	if (ok != 0) {
		free(fpi_task);
	}
}

/// platform messages
/*
static int on_send_platform_message(
	void *userdata
) {
	struct platform_message_response_handler_data *handler_data;
	struct platform_message *msg;
	FlutterEngineResult result;

	msg = userdata;

	if (msg->is_response) {
		result = msg->flutterpi.flutter.libflutter_engine.FlutterEngineSendPlatformMessageResponse(flutterpi.flutter.engine, msg->target_handle, msg->message, msg->message_size);
	} else {
		// if we have a response callback, allocate a response handle here.
		handlerdata = malloc(sizeof(struct platch_msg_resp_handler_data));
		if (!handlerdata) {
			return ENOMEM;
		}
		
		handlerdata->codec = response_codec;
		handlerdata->on_response = on_response;
		handlerdata->userdata = userdata;

		result = flutterpi.flutter.libflutter_engine.FlutterPlatformMessageCreateResponseHandle(flutterpi.flutter.engine, platch_on_response_internal, handlerdata, &response_handle);
		if (result != kSuccess) {
			fprintf(stderr, "[flutter-pi] Error create platform message response handle. FlutterPlatformMessageCreateResponseHandle: %s\n", FLUTTER_RESULT_TO_STRING(result));
			goto fail_free_handlerdata;
		}

		result = msg->flutterpi.flutter.libflutter_engine.FlutterEngineSendPlatformMessage(
			flutterpi.flutter.engine,
			&(FlutterPlatformMessage) {
				.struct_size = sizeof(FlutterPlatformMessage),
				.channel = msg->target_channel,
				.message = msg->message,
				.message_size = msg->message_size,
				.response_handle = msg->response_handle
			}
		);
	}

	if (msg->message) {
		free(msg->message);
	}

	if (msg->is_response == false) {
		free(msg->target_channel);
	}

	free(msg);

	if (result != kSuccess) {
		fprintf(stderr, "[flutter-pi] Error sending platform message. FlutterEngineSendPlatformMessage: %s\n", FLUTTER_RESULT_TO_STRING(result));
	}

	return 0;
}

int flutterpi_send_platform_message(
	struct flutterpi *flutterpi,
	const char *channel,
	const uint8_t *restrict message,
	size_t message_size,
	FlutterPlatformMessageResponseHandle *responsehandle
) {
	struct platform_message *msg;
	FlutterEngineResult result;
	int ok;
	
	if (runs_platform_tasks_on_current_thread(NULL)) {
		result = flutterpi.flutter.libflutter_engine.FlutterEngineSendPlatformMessage(
			flutterpi.flutter.engine,
			&(const FlutterPlatformMessage) {
				.struct_size = sizeof(FlutterPlatformMessage),
				.channel = channel,
				.message = message,
				.message_size = message_size,
				.response_handle = responsehandle
			}
		);
		if (result != kSuccess) {
			fprintf(stderr, "[flutter-pi] Error sending platform message. FlutterEngineSendPlatformMessage: %s\n", FLUTTER_RESULT_TO_STRING(result));
			return EIO;
		}
	} else {
		msg = calloc(1, sizeof *msg);
		if (msg == NULL) {
			return ENOMEM;
		}

		msg->is_response = false;
		msg->target_channel = strdup(channel);
		if (msg->target_channel == NULL) {
			free(msg);
			return ENOMEM;
		}

		msg->response_handle = responsehandle;
		
		if (message && message_size) {
			msg->message_size = message_size;
			msg->message = memdup(message, message_size);
			if (msg->message == NULL) {
				free(msg->target_channel);
				free(msg);
				return ENOMEM;
			}
		} else {
			msg->message = NULL;
			msg->message_size = 0;
		}

		ok = flutterpi_post_platform_task(
			on_send_platform_message,
			msg
		);
		if (ok != 0) {
			if (message && message_size) {
				free(msg->message);
			}
			free(msg->target_channel);
			free(msg);
			return ok;
		}
	}

	return 0;
}

int flutterpi_respond_to_platform_message(
	struct platform_message_response_handle *handle,
	const uint8_t *message,
	size_t message_size
) {
	struct flutterpi *flutterpi;
	struct platform_message *msg;
	FlutterEngineResult engine_result;
	int ok;

	flutterpi = handle->flutterpi;
	
	if (runs_platform_tasks_on_current_thread(NULL)) {
		engine_result = flutterpi->flutter.libflutter_engine.FlutterEngineSendPlatformMessageResponse(
			flutterpi->flutter.engine,
			handle->flutter_handle,
			message,
			message_size
		);
		if (engine_result != kSuccess) {
			fprintf(stderr, "[flutter-pi] Error sending platform message response. FlutterEngineSendPlatformMessageResponse: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
			return EIO;
		}
	} else {
		msg = malloc(sizeof *msg);
		if (msg == NULL) {
			return ENOMEM;
		}

		msg->is_response = true;
		msg->target_handle = handle;
		if (message && message_size) {
			msg->message_size = message_size;
			msg->message = memdup(message, message_size);
			if (msg->message == NULL) {
				free(msg);
				return ENOMEM;
			}
		} else {
			msg->message_size = 0;
			msg->message = 0;
		}

		ok = flutterpi_post_platform_task(
			on_send_platform_message,
			msg
		);
		if (ok != 0) {
			if (msg->message != NULL) {
				free(msg->message);
			}
			free(msg);
		}
	}

	return 0;
}
*/

static bool runs_platform_tasks_on_current_thread(void* userdata) {
	return flutterpi_runs_platform_tasks_on_current_thread(userdata);
}

bool flutterpi_runs_platform_tasks_on_current_thread(struct flutterpi *flutterpi) {
	DEBUG_ASSERT(flutterpi != NULL);
	return event_loop_processing_on_current_thread(flutterpi_get_event_loop(flutterpi, kPlatform));
}

static int run_main_loop(struct flutterpi *flutterpi) {
	DEBUG_ASSERT(flutterpi != NULL);
	return event_loop_process(flutterpi_get_event_loop(flutterpi, kPlatform));
}

static int init_main_loop(struct flutterpi *flutterpi) {
	struct event_loop *platform, *render;

	platform = event_loop_create(true, pthread_self());
	if (platform == NULL) {
		return ENOMEM;
	}

	render = event_loop_create(false, (pthread_t) 0);
	if (render == NULL) {
		event_loop_destroy(platform);
		return ENOMEM;
	}

	flutterpi->platform = platform;
	flutterpi->render = render;

	return 0;
}

/**************************
 * DISPLAY INITIALIZATION *
 **************************/
int flutterpi_fill_view_properties(
	struct flutterpi *flutterpi,
	bool has_orientation,
	enum device_orientation orientation,
	bool has_rotation,
	int rotation
) {
	enum device_orientation default_orientation = flutterpi->display.width >= flutterpi->display.height ? kLandscapeLeft : kPortraitUp;

	if (flutterpi->view.has_orientation) {
		if (flutterpi->view.has_rotation == false) {
			flutterpi->view.rotation = ANGLE_BETWEEN_ORIENTATIONS(default_orientation, flutterpi->view.orientation);
			flutterpi->view.has_rotation = true;
		}
	} else if (flutterpi->view.has_rotation) {
		for (int i = kPortraitUp; i <= kLandscapeRight; i++) {
			if (ANGLE_BETWEEN_ORIENTATIONS(default_orientation, i) == flutterpi->view.rotation) {
				flutterpi->view.orientation = i;
				flutterpi->view.has_orientation = true;
				break;
			}
		}
	} else {
		flutterpi->view.orientation = default_orientation;
		flutterpi->view.has_orientation = true;
		flutterpi->view.rotation = 0;
		flutterpi->view.has_rotation = true;
	}

	if (has_orientation) {
		flutterpi->view.rotation += ANGLE_BETWEEN_ORIENTATIONS(flutterpi->view.orientation, orientation);
		if (flutterpi->view.rotation >= 360) {
			flutterpi->view.rotation -= 360;
		}
		
		flutterpi->view.orientation = orientation;
	} else if (has_rotation) {
		for (int i = kPortraitUp; i <= kLandscapeRight; i++) {
			if (ANGLE_BETWEEN_ORIENTATIONS(default_orientation, i) == rotation) {
				flutterpi->view.orientation = i;
				flutterpi->view.rotation = rotation;
				break;
			}
		}
	}

	if ((flutterpi->view.rotation <= 45) || ((flutterpi->view.rotation >= 135) && (flutterpi->view.rotation <= 225)) || (flutterpi->view.rotation >= 315)) {
		flutterpi->view.width = flutterpi->display.width;
		flutterpi->view.height = flutterpi->display.height;
		flutterpi->view.width_mm = flutterpi->display.width_mm;
		flutterpi->view.height_mm = flutterpi->display.height_mm;
	} else {
		flutterpi->view.width = flutterpi->display.height;
		flutterpi->view.height = flutterpi->display.width;
		flutterpi->view.width_mm = flutterpi->display.height_mm;
		flutterpi->view.height_mm = flutterpi->display.width_mm;
	}

	if (flutterpi->view.rotation == 0) {
		flutterpi->view.view_to_display_transform = FLUTTER_TRANSLATION_TRANSFORMATION(0, 0);

		flutterpi->view.display_to_view_transform = FLUTTER_TRANSLATION_TRANSFORMATION(0, 0);
	} else if (flutterpi->view.rotation == 90) {
		flutterpi->view.view_to_display_transform = FLUTTER_ROTZ_TRANSFORMATION(90);
		flutterpi->view.view_to_display_transform.transX = flutterpi->display.width;

		flutterpi->view.display_to_view_transform = FLUTTER_ROTZ_TRANSFORMATION(-90);
		flutterpi->view.display_to_view_transform.transY = flutterpi->display.width;
	} else if (flutterpi->view.rotation == 180) {
		flutterpi->view.view_to_display_transform = FLUTTER_ROTZ_TRANSFORMATION(180);
		flutterpi->view.view_to_display_transform.transX = flutterpi->display.width;
		flutterpi->view.view_to_display_transform.transY = flutterpi->display.height;

		flutterpi->view.display_to_view_transform = FLUTTER_ROTZ_TRANSFORMATION(-180);
		flutterpi->view.display_to_view_transform.transX = flutterpi->display.width;
		flutterpi->view.display_to_view_transform.transY = flutterpi->display.height;
	} else if (flutterpi->view.rotation == 270) {
		flutterpi->view.view_to_display_transform = FLUTTER_ROTZ_TRANSFORMATION(270);
		flutterpi->view.view_to_display_transform.transY = flutterpi->display.height;

		flutterpi->view.display_to_view_transform = FLUTTER_ROTZ_TRANSFORMATION(-270);
		flutterpi->view.display_to_view_transform.transX = flutterpi->display.height;
	}

	return 0;
}

/*
static int load_egl_gl_procs(void) {
	// TODO: Make most of these optional.
	LOAD_EGL_PROC(flutterpi, getPlatformDisplay);
	LOAD_EGL_PROC(flutterpi, createPlatformWindowSurface);
	LOAD_EGL_PROC(flutterpi, createPlatformPixmapSurface);
	LOAD_EGL_PROC(flutterpi, createDRMImageMESA);
	LOAD_EGL_PROC(flutterpi, exportDRMImageMESA);
	LOAD_EGL_PROC(flutterpi, createImageKHR);
	LOAD_EGL_PROC(flutterpi, destroyImageKHR);

	LOAD_GL_PROC(flutterpi, EGLImageTargetTexture2DOES);
	LOAD_GL_PROC(flutterpi, EGLImageTargetRenderbufferStorageOES);

	return 0;
}
*/

int flutterpi_create_egl_context(struct flutterpi *flutterpi, EGLContext *context_out, EGLint *err_out) {
	EGLContext context;
	EGLint egl_error;
	bool has_current;

	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	has_current = eglGetCurrentContext() != EGL_NO_CONTEXT;

	eglGetError();

	if (!has_current) {
		eglMakeCurrent(flutterpi->egl.display, flutterpi->egl.surface, flutterpi->egl.surface, flutterpi->egl.root_context);
		if ((egl_error = eglGetError()) != EGL_SUCCESS) {
			if (err_out) {
				*err_out = egl_error;
			}
			if (context_out) {
				*context_out = EGL_NO_CONTEXT;
			}

			return EINVAL;
		}
	}

	context = eglCreateContext(flutterpi->egl.display, flutterpi->egl.config, flutterpi->egl.root_context, context_attribs);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		if (err_out) {
			*err_out = egl_error;
		}
		if (context_out) {
			*context_out = EGL_NO_CONTEXT;
		}

		eglMakeCurrent(flutterpi->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

		return EINVAL;
	}

	if (!has_current) {
		eglMakeCurrent(flutterpi->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	}

	if (err_out) {
		*err_out = EGL_SUCCESS;
	}
	if (context_out) {
		*context_out = context;
	}

	return 0;
}

/*
static int init_display(struct flutterpi *flutterpi) {
	**********************
	 * DRM INITIALIZATION *
	 **********************
	const struct drm_connector *connector;
	const struct drm_encoder *encoder;
	const struct drm_crtc *crtc;
	const drmModeModeInfo *mode, *mode_iter;
	drmDevicePtr devices[64];
	EGLint egl_error;
	int ok, num_devices;

	**********************
	 * DRM INITIALIZATION *
	 **********************
	
	num_devices = drmGetDevices2(0, devices, sizeof(devices)/sizeof(*devices));
	if (num_devices < 0) {
		fprintf(stderr, "[flutter-pi] Could not query DRM device list: %s\n", strerror(-num_devices));
		return -num_devices;
	}
	
	// find a GPU that has a primary node
	flutterpi->drm.drmdev = NULL;
	for (int i = 0; i < num_devices; i++) {
		drmDevicePtr device;
		
		device = devices[i];

		if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY))) {
			// We need a primary node.
			continue;
		}

		ok = drmdev_new_from_path(&flutterpi->drm.drmdev, device->nodes[DRM_NODE_PRIMARY]);
		if (ok != 0) {
			fprintf(stderr, "[flutter-pi] Could not create drmdev from device at \"%s\". Continuing.\n", device->nodes[DRM_NODE_PRIMARY]);
			continue;
		}

		break;
	}

	if (flutterpi->drm.drmdev == NULL) {
		fprintf(stderr, "flutter-pi couldn't find a usable DRM device.\n"
						"Please make sure you've enabled the Fake-KMS driver in raspi-config.\n"
						"If you're not using a Raspberry Pi, please make sure there's KMS support for your graphics chip.\n");
		return ENOENT;
	}

	// find a connected connector
	for_each_connector_in_drmdev(flutterpi->drm.drmdev, connector) {
		if (connector->connector->connection == DRM_MODE_CONNECTED) {
			// only update the physical size of the display if the values
			//   are not yet initialized / not set with a commandline option
			if ((flutterpi->display.width_mm == 0) || (flutterpi->display.height_mm == 0)) {
				if ((connector->connector->connector_type == DRM_MODE_CONNECTOR_DSI) &&
					(connector->connector->mmWidth == 0) &&
					(connector->connector->mmHeight == 0))
				{
					// if it's connected via DSI, and the width & height are 0,
					//   it's probably the official 7 inch touchscreen.
					flutterpi->display.width_mm = 155;
					flutterpi->display.height_mm = 86;
				} else if ((connector->connector->mmHeight % 10 == 0) &&
							(connector->connector->mmWidth % 10 == 0)) {
					// don't change anything.
				} else {
					flutterpi->display.width_mm = connector->connector->mmWidth;
					flutterpi->display.height_mm = connector->connector->mmHeight;
				}
			}

			break;
		}
	}

	if (connector == NULL) {
		fprintf(stderr, "[flutter-pi] Could not find a connected connector!\n");
		return EINVAL;
	}

	// Find the preferred mode (GPU drivers _should_ always supply a preferred mode, but of course, they don't)
	// Alternatively, find the mode with the highest width*height. If there are multiple modes with the same w*h,
	// prefer higher refresh rates. After that, prefer progressive scanout modes.
	mode = NULL;
	for_each_mode_in_connector(connector, mode_iter) {
		if (mode_iter->type & DRM_MODE_TYPE_PREFERRED) {
			mode = mode_iter;
			break;
		} else if (mode == NULL) {
			mode = mode_iter;
		} else {
			int area = mode_iter->hdisplay * mode_iter->vdisplay;
			int old_area = mode->hdisplay * mode->vdisplay;

			if ((area > old_area) ||
				((area == old_area) && (mode_iter->vrefresh > mode->vrefresh)) ||
				((area == old_area) && (mode_iter->vrefresh == mode->vrefresh) && ((mode->flags & DRM_MODE_FLAG_INTERLACE) == 0))) {
				mode = mode_iter;
			}
		}
	}

	if (mode == NULL) {
		fprintf(stderr, "[flutter-pi] Could not find a preferred output mode!\n");
		return EINVAL;
	}

	flutterpi->display.width = mode->hdisplay;
	flutterpi->display.height = mode->vdisplay;
	flutterpi->display.refresh_rate = mode->vrefresh;

	if ((flutterpi->display.width_mm == 0) || (flutterpi->display.height_mm == 0)) {
		fprintf(
			stderr,
			"[flutter-pi] WARNING: display didn't provide valid physical dimensions.\n"
			"             The device-pixel ratio will default to 1.0, which may not be the fitting device-pixel ratio for your display.\n"
		);
		flutterpi->display.pixel_ratio = 1.0;
	} else {
		flutterpi->display.pixel_ratio = (10.0 * flutterpi->display.width) / (flutterpi->display.width_mm * 38.0);
		
		int horizontal_dpi = (int) (flutterpi->display.width / (flutterpi->display.width_mm / 25.4));
		int vertical_dpi = (int) (flutterpi->display.height / (flutterpi->display.height_mm / 25.4));

		if (horizontal_dpi != vertical_dpi) {
		        // See https://github.com/flutter/flutter/issues/71865 for current status of this issue.
			fprintf(stderr, "[flutter-pi] WARNING: display has non-square pixels. Non-square-pixels are not supported by flutter.\n");
		}
	}
	
	for_each_encoder_in_drmdev(flutterpi->drm.drmdev, encoder) {
		if (encoder->encoder->encoder_id == connector->connector->encoder_id) {
			break;
		}
	}
	
	if (encoder == NULL) {
		for (int i = 0; i < connector->connector->count_encoders; i++, encoder = NULL) {
			for_each_encoder_in_drmdev(flutterpi->drm.drmdev, encoder) {
				if (encoder->encoder->encoder_id == connector->connector->encoders[i]) {
					break;
				}
			}

			if (encoder->encoder->possible_crtcs) {
				// only use this encoder if there's a crtc we can use with it
				break;
			}
		}
	}

	if (encoder == NULL) {
		fprintf(stderr, "[flutter-pi] Could not find a suitable DRM encoder.\n");
		return EINVAL;
	}

	for_each_crtc_in_drmdev(flutterpi->drm.drmdev, crtc) {
		if (crtc->crtc->crtc_id == encoder->encoder->crtc_id) {
			break;
		}
	}

	if (crtc == NULL) {
		for_each_crtc_in_drmdev(flutterpi->drm.drmdev, crtc) {
			if (encoder->encoder->possible_crtcs & crtc->bitmask) {
				// find a CRTC that is possible to use with this encoder
				break;
			}
		}
	}

	if (crtc == NULL) {
		fprintf(stderr, "[flutter-pi] Could not find a suitable DRM CRTC.\n");
		return EINVAL;
	}

	ok = drmdev_configure(flutterpi->drm.drmdev, connector->connector->connector_id, encoder->encoder->encoder_id, crtc->crtc->crtc_id, mode);
	if (ok != 0) return ok;

	// only enable vsync if the kernel supplies valid vblank timestamps
	{
		uint64_t ns = 0;
		ok = drmCrtcGetSequence(flutterpi->drm.drmdev->fd, flutterpi->drm.drmdev->selected_crtc->crtc->crtc_id, NULL, &ns);
		int _errno = errno;

		if ((ok == 0) && (ns != 0)) {
			flutterpi->drm.platform_supports_get_sequence_ioctl = true;
		} else {
			flutterpi->drm.platform_supports_get_sequence_ioctl = false;
			if (ok != 0) {
				fprintf(
					stderr,
					"WARNING: Error getting last vblank timestamp. drmCrtcGetSequence: %s\n",
					strerror(_errno)
				);
			} else {
				fprintf(
					stderr,
					"WARNING: Kernel didn't return a valid vblank timestamp. (timestamp == 0)\n"
				);
			}
			fprintf(
				stderr,
				"         VSync will be disabled.\n"
				"         See https://github.com/ardera/flutter-pi/issues/38 for more info.\n"
			);
		}
	}

	memset(&flutterpi->drm.evctx, 0, sizeof(drmEventContext));
	flutterpi->drm.evctx.version = 4;
	flutterpi->drm.evctx.page_flip_handler = on_pageflip_event;

	ok = sd_event_add_io(
		flutterpi->event_loop,
		&flutterpi->drm.drm_pageflip_event_source,
		flutterpi->drm.drmdev->fd,
		EPOLLIN | EPOLLHUP | EPOLLPRI,
		on_drm_fd_ready,
		NULL
	);
	if (ok < 0) {
		fprintf(stderr, "[flutter-pi] Could not add DRM pageflip event listener. sd_event_add_io: %s\n", strerror(-ok));
		return -ok;
	}

	printf(
		"===================================\n"
		"display mode:\n"
		"  resolution: %u x %u\n"
		"  refresh rate: %uHz\n"
		"  physical size: %umm x %umm\n"
		"  flutter device pixel ratio: %f\n"
		"===================================\n",
		flutterpi->display.width, flutterpi->display.height,
		flutterpi->display.refresh_rate,
		flutterpi->display.width_mm, flutterpi->display.height_mm,
		flutterpi->display.pixel_ratio
	);

	**********************
	 * GBM INITIALIZATION *
	 **********************
	flutterpi->gbm.device = gbm_create_device(flutterpi->drm.drmdev->fd);
	flutterpi->gbm.format = DRM_FORMAT_ARGB8888;
	flutterpi->gbm.surface = NULL;
	flutterpi->gbm.modifier = DRM_FORMAT_MOD_LINEAR;

	flutterpi->gbm.surface = gbm_surface_create_with_modifiers(flutterpi->gbm.device, flutterpi->display.width, flutterpi->display.height, flutterpi->gbm.format, &flutterpi->gbm.modifier, 1);
	if (flutterpi->gbm.surface == NULL) {
		perror("[flutter-pi] Could not create GBM Surface. gbm_surface_create_with_modifiers");
		return errno;
	}

	**********************
	 * EGL INITIALIZATION *
	 **********************
	EGLint major, minor;

	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_SAMPLES, 0,
		EGL_NONE
	};

	flutterpi->egl.lib = libegl_load();
	flutterpi->egl.client_info = egl_client_info_new(flutterpi->egl.lib);

	EGLDisplay display;
	if (flutterpi->egl.lib->eglGetPlatformDisplay != NULL) {
		display = flutterpi->egl.lib->eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, flutterpi->gbm.device, NULL);
		if ((egl_error = eglGetError()) != EGL_SUCCESS) {
			fprintf(stderr, "[flutter-pi] Could not get EGL display! eglGetPlatformDisplay: 0x%08X\n", egl_error);
			return EIO;
		}
	} else if (flutterpi->egl.client_info->supports_ext_platform_base && flutterpi->egl.client_info->supports_khr_platform_gbm) {
		display = flutterpi->egl.lib->eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, flutterpi->gbm.device, NULL);
		if ((egl_error = eglGetError()) != EGL_SUCCESS) {
			fprintf(stderr, "[flutter-pi] Could not get EGL display! eglGetPlatformDisplayEXT: 0x%08X\n", egl_error);
			return EIO;
		}
	} else {
		display = eglGetDisplay((void*) flutterpi->gbm.device);
		if ((egl_error = eglGetError()) != EGL_SUCCESS) {
			fprintf(stderr, "[flutter-pi] Could not get EGL display! eglGetDisplay: 0x%08X\n", egl_error);
			return EIO;
		}
	}

	flutterpi->egl.display = display;
	
	eglInitialize(flutterpi->egl.display, &major, &minor);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Failed to initialize EGL! eglInitialize: 0x%08X\n", egl_error);
		return EIO;
	}

	flutterpi->egl.display_info = egl_display_info_new(flutterpi->egl.lib, major, minor, flutterpi->egl.display);

	printf("EGL information:\n");
	printf("  version: %s\n", eglQueryString(flutterpi->egl.display, EGL_VERSION));
	printf("  vendor: \"%s\"\n", eglQueryString(flutterpi->egl.display, EGL_VENDOR));
	printf("  client extensions: \"%s\"\n", flutterpi->egl.display_info->client_extensions);
	printf("  display extensions: \"%s\"\n", flutterpi->egl.display_info->display_extensions);
	printf("===================================\n");

	if (!flutterpi->egl.display_info->supports_12) {
		fprintf(stderr, "[flutter-pi] EGL 1.2 or newer is required.\n");
	}

	eglBindAPI(EGL_OPENGL_ES_API);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Failed to bind OpenGL ES API! eglBindAPI: 0x%08X\n", egl_error);
		return EIO;
	}

	EGLint count = 0, matched = 0;
	EGLConfig *configs;
	bool _found_matching_config = false;
	
	eglGetConfigs(flutterpi->egl.display, NULL, 0, &count);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not get the number of EGL framebuffer configurations. eglGetConfigs: 0x%08X\n", egl_error);
		return EIO;
	}

	configs = malloc(count * sizeof(EGLConfig));
	if (!configs) return ENOMEM;

	eglChooseConfig(flutterpi->egl.display, config_attribs, configs, count, &matched);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not query EGL framebuffer configurations with fitting attributes. eglChooseConfig: 0x%08X\n", egl_error);
		return EIO;
	}

	if (matched == 0) {
		fprintf(stderr, "[flutter-pi] No fitting EGL framebuffer configuration found.\n");
		return EIO;
	}

	for (int i = 0; i < count; i++) {
		EGLint native_visual_id;

		eglGetConfigAttrib(flutterpi->egl.display, configs[i], EGL_NATIVE_VISUAL_ID, &native_visual_id);
		if ((egl_error = eglGetError()) != EGL_SUCCESS) {
			fprintf(stderr, "[flutter-pi] Could not query native visual ID of EGL config. eglGetConfigAttrib: 0x%08X\n", egl_error);
			continue;
		}

		if ((uint32_t) native_visual_id == flutterpi->gbm.format) {
			flutterpi->egl.config = configs[i];
			_found_matching_config = true;
			break;
		}
	}
	free(configs);

	if (_found_matching_config == false) {
		fprintf(stderr, "[flutter-pi] Could not find EGL framebuffer configuration with appropriate attributes & native visual ID.\n");
		return EIO;
	}

	****************************
	 * OPENGL ES INITIALIZATION *
	 ****************************
	flutterpi->egl.root_context = eglCreateContext(flutterpi->egl.display, flutterpi->egl.config, EGL_NO_CONTEXT, context_attribs);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not create OpenGL ES root context. eglCreateContext: 0x%08X\n", egl_error);
		return EIO;
	}

	flutterpi->egl.flutter_render_context = eglCreateContext(flutterpi->egl.display, flutterpi->egl.config, flutterpi->egl.root_context, context_attribs);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not create OpenGL ES context for flutter rendering. eglCreateContext: 0x%08X\n", egl_error);
		return EIO;
	}

	flutterpi->egl.flutter_resource_uploading_context = eglCreateContext(flutterpi->egl.display, flutterpi->egl.config, flutterpi->egl.root_context, context_attribs);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not create OpenGL ES context for flutter resource uploads. eglCreateContext: 0x%08X\n", egl_error);
		return EIO;
	}

	flutterpi->egl.compositor_context = eglCreateContext(flutterpi->egl.display, flutterpi->egl.config, flutterpi->egl.root_context, context_attribs);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not create OpenGL ES context for compositor. eglCreateContext: 0x%08X\n", egl_error);
		return EIO;
	}

	flutterpi->egl.surface = eglCreateWindowSurface(flutterpi->egl.display, flutterpi->egl.config, (EGLNativeWindowType) flutterpi->gbm.surface, NULL);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not create EGL window surface. eglCreateWindowSurface: 0x%08X\n", egl_error);
		return EIO;
	}

	eglMakeCurrent(flutterpi->egl.display, flutterpi->egl.surface, flutterpi->egl.surface, flutterpi->egl.root_context);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not make OpenGL ES root context current to get OpenGL information. eglMakeCurrent: 0x%08X\n", egl_error);
		return EIO;
	}

	flutterpi->gl.renderer = (char*) glGetString(GL_RENDERER);

	flutterpi->gl.version = (char*) glGetString(GL_VERSION);
	flutterpi->gl.shading_language_version = (char*) glGetString(GL_SHADING_LANGUAGE_VERSION);
	flutterpi->gl.vendor = (char*) glGetString(GL_VENDOR);
	flutterpi->gl.renderer = (char*) glGetString(GL_RENDERER);
	flutterpi->gl.extensions = (char*) glGetString(GL_EXTENSIONS);

	flutterpi->gl.is_vc4 = strncmp(flutterpi->gl.renderer, "VC4 V3D", strlen("VC4 V3D")) == 0;

	if (flutterpi->gl.is_vc4) {
		printf(
			"[flutter-pi] Detected VideoCore IV as underlying graphics chip, and VC4 as\n"
			"             the driver. Reporting modified GL_EXTENSIONS string that doesn't\n"
			"             contain non-working extensions.\n"
		);

		char *extensions = strdup(flutterpi->gl.extensions);
		if (extensions == NULL) {
			return ENOMEM;
		}

		*
		* working (apparently)
		*
		//cut_word_from_string(extensions, "GL_EXT_blend_minmax");
		//cut_word_from_string(extensions, "GL_EXT_multi_draw_arrays");
		//cut_word_from_string(extensions, "GL_EXT_texture_format_BGRA8888");
		//cut_word_from_string(extensions, "GL_OES_compressed_ETC1_RGB8_texture");
		//cut_word_from_string(extensions, "GL_OES_depth24");
		//cut_word_from_string(extensions, "GL_OES_texture_npot");
		//cut_word_from_string(extensions, "GL_OES_vertex_half_float");
		//cut_word_from_string(extensions, "GL_OES_EGL_image");
		//cut_word_from_string(extensions, "GL_OES_depth_texture");
		//cut_word_from_string(extensions, "GL_AMD_performance_monitor");
		//cut_word_from_string(extensions, "GL_OES_EGL_image_external");
		//cut_word_from_string(extensions, "GL_EXT_occlusion_query_boolean");
		//cut_word_from_string(extensions, "GL_KHR_texture_compression_astc_ldr");
		//cut_word_from_string(extensions, "GL_EXT_compressed_ETC1_RGB8_sub_texture");
		//cut_word_from_string(extensions, "GL_EXT_draw_elements_base_vertex");
		//cut_word_from_string(extensions, "GL_EXT_texture_border_clamp");
		//cut_word_from_string(extensions, "GL_OES_draw_elements_base_vertex");
		//cut_word_from_string(extensions, "GL_OES_texture_border_clamp");
		//cut_word_from_string(extensions, "GL_KHR_texture_compression_astc_sliced_3d");
		//cut_word_from_string(extensions, "GL_MESA_tile_raster_order");

		*
		* should be working, but isn't
		*
		cut_word_from_string(extensions, "GL_EXT_map_buffer_range");

		*
		* definitely broken
		*
		cut_word_from_string(extensions, "GL_OES_element_index_uint");
		cut_word_from_string(extensions, "GL_OES_fbo_render_mipmap");
		cut_word_from_string(extensions, "GL_OES_mapbuffer");
		cut_word_from_string(extensions, "GL_OES_rgb8_rgba8");
		cut_word_from_string(extensions, "GL_OES_stencil8");
		cut_word_from_string(extensions, "GL_OES_texture_3D");
		cut_word_from_string(extensions, "GL_OES_packed_depth_stencil");
		cut_word_from_string(extensions, "GL_OES_get_program_binary");
		cut_word_from_string(extensions, "GL_APPLE_texture_max_level");
		cut_word_from_string(extensions, "GL_EXT_discard_framebuffer");
		cut_word_from_string(extensions, "GL_EXT_read_format_bgra");
		cut_word_from_string(extensions, "GL_EXT_frag_depth");
		cut_word_from_string(extensions, "GL_NV_fbo_color_attachments");
		cut_word_from_string(extensions, "GL_OES_EGL_sync");
		cut_word_from_string(extensions, "GL_OES_vertex_array_object");
		cut_word_from_string(extensions, "GL_EXT_unpack_subimage");
		cut_word_from_string(extensions, "GL_NV_draw_buffers");
		cut_word_from_string(extensions, "GL_NV_read_buffer");
		cut_word_from_string(extensions, "GL_NV_read_depth");
		cut_word_from_string(extensions, "GL_NV_read_depth_stencil");
		cut_word_from_string(extensions, "GL_NV_read_stencil");
		cut_word_from_string(extensions, "GL_EXT_draw_buffers");
		cut_word_from_string(extensions, "GL_KHR_debug");
		cut_word_from_string(extensions, "GL_OES_required_internalformat");
		cut_word_from_string(extensions, "GL_OES_surfaceless_context");
		cut_word_from_string(extensions, "GL_EXT_separate_shader_objects");
		cut_word_from_string(extensions, "GL_KHR_context_flush_control");
		cut_word_from_string(extensions, "GL_KHR_no_error");
		cut_word_from_string(extensions, "GL_KHR_parallel_shader_compile");

		flutterpi->gl.extensions_override = extensions;
	} else {
		flutterpi->gl.extensions_override = NULL;
	}

	printf("OpenGL ES information:\n");
	printf("  version: \"%s\"\n", flutterpi->gl.vendor);
	printf("  shading language version: \"%s\"\n", flutterpi->gl.shading_language_version);
	printf("  vendor: \"%s\"\n", flutterpi->gl.vendor);
	printf("  renderer: \"%s\"\n", flutterpi->gl.renderer);
	printf("  extensions: \"%s\"\n", flutterpi->gl.extensions);
	printf("===================================\n");

	// it seems that after some Raspbian update, regular users are sometimes no longer allowed
	//   to use the direct-rendering infrastructure; i.e. the open the devices inside /dev/dri/
	//   as read-write. flutter-pi must be run as root then.
	// sometimes it works fine without root, sometimes it doesn't.
	if (strncmp(flutterpi->gl.renderer, "llvmpipe", sizeof("llvmpipe")-1) == 0) {
		printf("WARNING: Detected llvmpipe (ie. software rendering) as the OpenGL ES renderer.\n"
			   "         Check that flutter-pi has permission to use the 3D graphics hardware,\n"
			   "         or try running it as root.\n"
			   "         This warning will probably result in a \"failed to set mode\" error\n"
			   "         later on in the initialization.\n");
	}

	eglMakeCurrent(flutterpi->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if ((egl_error = eglGetError()) != EGL_SUCCESS) {
		fprintf(stderr, "[flutter-pi] Could not clear OpenGL ES context. eglMakeCurrent: 0x%08X\n", egl_error);
		return EIO;
	}

	/// miscellaneous initialization
	/// initialize the compositor
	struct compositor *compositor = compositor_new(
		flutterpi->drm.drmdev,
		flutterpi->gbm.surface,
		flutterpi->egl.lib,
		flutterpi->egl.display,
		flutterpi->egl.surface,
		flutterpi->egl.flutter_render_context,
		flutterpi->egl.display_info,
		NULL,
		NULL,
		NULL,
		NULL
	);
	if (compositor == NULL) {
		return EINVAL;
	}

	flutterpi->compositor = compositor;

	/// initialize the frame queue
	ok = cqueue_init(&flutterpi->frame_queue, sizeof(struct frame), QUEUE_DEFAULT_MAX_SIZE);
	if (ok != 0) {
		return ok;
	}

	/// We're starting without any rotation by default.
	flutterpi_fill_view_properties(flutterpi, false, 0, false, 0);

	return 0;

	// TODO: proper error handling

	fail_close_drmdev:
	drmdev_destroy(flutterpi->drm.drmdev);

	return ok;
}*/

static bool flutterpi_texture_frame_callback(
	void* userdata,
	int64_t texture_id,
	size_t width,
	size_t height,
	FlutterOpenGLTexture* texture_out
) {
	struct flutterpi *flutterpi = userdata;
	return texreg_on_external_texture_frame_callback(flutterpi->texture_registry, texture_id, width, height, texture_out);
}

/**************
 * USER INPUT *
 **************/
static void on_flutter_pointer_event(void *userdata, const FlutterPointerEvent *events, size_t n_events) {
	FlutterEngineResult engine_result;
	struct flutterpi *flutterpi;

	flutterpi = userdata;

	engine_result = flutterpi->flutter.libflutter_engine->FlutterEngineSendPointerEvent(
		flutterpi->flutter.engine,
		events,
		n_events
	);

	if (engine_result != kSuccess) {
		LOG_FLUTTERPI_ERROR("Error sending touchscreen / mouse events to flutter. FlutterEngineSendPointerEvent: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
		flutterpi_schedule_exit(flutterpi);
	}
}

static void on_utf8_character(void *userdata, uint8_t *character) {
#ifdef BUILD_TEXT_INPUT_PLUGIN
	struct flutterpi *flutterpi;
	int ok;

	flutterpi = userdata;

	if (flutterpi->private->textin != NULL) {
		ok = textin_on_utf8_char(flutterpi->private->textin, character);
		if (ok != 0) {
			LOG_FLUTTERPI_ERROR("Error handling keyboard event. textin_on_utf8_char: %s\n", strerror(ok));
			flutterpi_schedule_exit(flutterpi);
		}
	}
#endif
}

static void on_xkb_keysym(void *userdata, xkb_keysym_t keysym) {
#ifdef BUILD_TEXT_INPUT_PLUGIN
	struct flutterpi *flutterpi;
	int ok;

	flutterpi = userdata;

	if (flutterpi->private->textin != NULL) {
		ok = textin_on_xkb_keysym(flutterpi->private->textin, keysym);
		if (ok != 0) {
			LOG_FLUTTERPI_ERROR("Error handling keyboard event. textin_on_xkb_keysym: %s\n", strerror(ok));
			flutterpi_schedule_exit(flutterpi);
		}
	}
#endif
}

static void on_gtk_keyevent(
	void *userdata,
	uint32_t unicode_scalar_values,
    uint32_t key_code,
    uint32_t scan_code,
    uint32_t modifiers,
    bool is_down
) {
#ifdef BUILD_RAW_KEYBOARD_PLUGIN
	struct flutterpi *flutterpi;
	int ok;

	flutterpi = userdata;

	if (flutterpi->private->rawkb != NULL) {
		ok = rawkb_send_gtk_keyevent(
			flutterpi->private->rawkb,
			unicode_scalar_values,
			key_code,
			scan_code,
			modifiers,
			is_down
		);
		if (ok != 0) {
			LOG_FLUTTERPI_ERROR("Error handling keyboard event. rawkb_send_gtk_keyevent: %s\n", strerror(ok));
			flutterpi_schedule_exit(flutterpi);
		}
	}
#endif
}

static void on_set_cursor_enabled(void *userdata, bool enabled) {
	struct flutterpi *flutterpi;
	int ok;

	flutterpi = userdata;

	ok = compositor_set_cursor_state(
		flutterpi->compositor,
		true, enabled,
		false, 0,
		false, 0
	);
	if (ok != 0) {
		LOG_FLUTTERPI_ERROR("Error enabling / disabling mouse cursor. compositor_apply_cursor_state: %s\n", strerror(ok));
	}
}

static void on_move_cursor(void *userdata, unsigned int x, unsigned int y) {
	struct flutterpi *flutterpi;
	int ok;

	flutterpi = userdata;

	ok = compositor_set_cursor_pos(flutterpi->compositor, x, y);
	if (ok != 0) {
		LOG_FLUTTERPI_ERROR("Error moving mouse cursor. compositor_set_cursor_pos: %s\n", strerror(ok));
	}
}

static const struct user_input_interface user_input_interface = {
    .on_flutter_pointer_event = on_flutter_pointer_event,
    .on_utf8_character = on_utf8_character,
    .on_xkb_keysym = on_xkb_keysym,
    .on_gtk_keyevent = on_gtk_keyevent,
    .on_set_cursor_enabled = on_set_cursor_enabled,
    .on_move_cursor = on_move_cursor
};


/**************************
 * FLUTTER INITIALIZATION *
 **************************/
static int init_application(struct flutterpi *fpi) {
	FlutterEngineAOTDataSource aot_source;
	struct libflutter_engine *engine_lib;
	struct texture_registry *texreg;
	FlutterEngineAOTData aot_data;
	FlutterEngineResult engine_result;
	FlutterCompositor flutter_compositor;

	engine_lib = NULL;
	if (fpi->flutter.runtime_mode == kRelease) {
		engine_lib = libflutter_engine_load("libflutter_engine.so.release");
		if (engine_lib == NULL) {
			printf("[flutter-pi] Warning: Could not load libflutter_engine.so.release.\n");
		}
	} else if (fpi->flutter.runtime_mode == kDebug) {
		engine_lib = libflutter_engine_load("libflutter_engine.so.debug");
		if (engine_lib == NULL) {
			printf("[flutter-pi] Warning: Could not load libflutter_engine.so.debug.\n");
		}
	}

	if (engine_lib == NULL) {
		engine_lib = libflutter_engine_load("libflutter_engine.so");
		if (engine_lib == NULL) {
			fprintf(stderr, "[flutter-pi] Could not load libflutter_engine.so.\n");
		}
	}

	if (engine_lib == NULL) {
		fprintf(stderr, "[flutter-pi] Could not find a fitting libflutter_engine.\n");
		return EINVAL;
	}

	texreg = texreg_new(
		NULL,
		engine_lib->FlutterEngineRegisterExternalTexture,
		engine_lib->FlutterEngineMarkExternalTextureFrameAvailable,
		engine_lib->FlutterEngineUnregisterExternalTexture
	);
	if (texreg == NULL) {
		fprintf(stderr, "[flutter-pi] Could not create texture registry. texreg_new\n");
		return EINVAL;
	}

	compositor_fill_flutter_compositor(
		fpi->compositor,
		&flutter_compositor
	);

	bool engine_is_aot = engine_lib->FlutterEngineRunsAOTCompiledDartCode();
	if ((engine_is_aot == true) && (fpi->flutter.runtime_mode != kRelease)) {
		fprintf(
			stderr,
			"[flutter-pi] The flutter engine was built for release (AOT) mode,\n"
			"             but flutter-pi was not started up in release mode. \n"
			"             Either you swap out the libflutter_engine.so \n"
			"             with one that was built for debug mode, or you start\n"
			"             flutter-pi with the --release flag and make sure\n"
			"             a valid \"app.so\" is located inside the asset bundle\n"
			"             directory.\n"
		);
		return EINVAL;
	} else if ((engine_is_aot == false) && (fpi->flutter.runtime_mode != kDebug)) {
		fprintf(
			stderr,
			"[flutter-pi] The flutter engine was built for debug mode,\n"
			"             but flutter-pi was started up in release mode.\n"
			"             Either you swap out the libflutter_engine.so\n"
			"             with one that was built for release mode, or you\n"
			"             start flutter-pi without the --release flag.\n"
		);
		return EINVAL;
	}

	if (fpi->flutter.runtime_mode == kRelease) {
		aot_source = (FlutterEngineAOTDataSource) {
			.elf_path = fpi->flutter.app_elf_path,
			.type = kFlutterEngineAOTDataSourceTypeElfPath
		};

		engine_result = engine_lib->FlutterEngineCreateAOTData(&aot_source, &aot_data);
		if (engine_result != kSuccess) {
			fprintf(stderr, "[flutter-pi] Could not load AOT data. FlutterEngineCreateAOTData: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
			return EIO;
		}
	}

	engine_result = engine_lib->FlutterEngineInitialize(
		FLUTTER_ENGINE_VERSION,
		&(FlutterRendererConfig) {
			.type = kOpenGL,
			.open_gl = {
				.struct_size = sizeof(FlutterOpenGLRendererConfig),
				.make_current = on_flutter_gl_make_current,
				.clear_current = on_flutter_gl_clear_current,
				.present = on_flutter_gl_present,
				.fbo_callback = on_flutter_gl_get_fbo,
				.make_resource_current = on_flutter_gl_make_resource_context_current,
				.gl_proc_resolver = on_flutter_gl_resolve_proc,
				.surface_transformation = on_flutter_gl_get_surface_transformation,
				.gl_external_texture_frame_callback = on_flutter_gl_get_external_texture_frame,
			}
		},
		&(FlutterProjectArgs) {
			.struct_size = sizeof(FlutterProjectArgs),
			.assets_path = fpi->flutter.asset_bundle_path,
			.main_path__unused__ = NULL,
			.packages_path__unused__ = NULL,
			.icu_data_path = fpi->flutter.icu_data_path,
			.command_line_argc = fpi->flutter.engine_argc,
			.command_line_argv = (const char * const*) fpi->flutter.engine_argv,
			.platform_message_callback = on_platform_message,
			.vm_snapshot_data = NULL,
			.vm_snapshot_data_size = 0,
			.vm_snapshot_instructions = NULL,
			.vm_snapshot_instructions_size = 0,
			.isolate_snapshot_data = NULL,
			.isolate_snapshot_data_size = 0,
			.isolate_snapshot_instructions = NULL,
			.isolate_snapshot_instructions_size = 0,
			.root_isolate_create_callback = NULL,
			.update_semantics_node_callback = NULL,
			.update_semantics_custom_action_callback = NULL,
			.persistent_cache_path = NULL,
			.is_persistent_cache_read_only = false,
			.vsync_callback = on_frame_request,
			.custom_dart_entrypoint = NULL,
			.custom_task_runners = &(FlutterCustomTaskRunners) {
				.struct_size = sizeof(FlutterCustomTaskRunners),
				.platform_task_runner = &(FlutterTaskRunnerDescription) {
					.struct_size = sizeof(FlutterTaskRunnerDescription),
					.user_data = NULL,
					.runs_task_on_current_thread_callback = runs_platform_tasks_on_current_thread,
					.post_task_callback = on_post_flutter_task
				},
				.render_task_runner = NULL
			},
			.shutdown_dart_vm_when_done = true,
			.compositor = &flutter_compositor,
			.dart_old_gen_heap_size = -1,
			.aot_data = aot_data,
			.compute_platform_resolved_locale_callback = NULL
		},
		fpi,
		&fpi->flutter.engine
	);
	if (engine_result != kSuccess) {
		fprintf(stderr, "[flutter-pi] Could not run the flutter engine. FlutterEngineInitialize: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
		return EINVAL;
	}

	fpi->plugin_registry = plugin_registry_new(fpi);
	if (fpi->plugin_registry == NULL) {
		fprintf(stderr, "[flutter-pi] Could not initialize plugin registry.\n");
		return EINVAL;
	}

	fpi->texture_registry = texreg;
	texreg_set_engine(texreg, fpi->flutter.engine);

	compositor_set_tracing_interface(
		fpi->compositor,
		&(const struct flutter_tracing_interface) {
			.get_current_time = engine_lib->FlutterEngineGetCurrentTime,
			.trace_event_begin = engine_lib->FlutterEngineTraceEventDurationBegin,
			.trace_event_end = engine_lib->FlutterEngineTraceEventDurationEnd,
			.trace_event_instant = engine_lib->FlutterEngineTraceEventInstant
		}
	);

	fpi->private = malloc(sizeof *fpi->private);
	if (fpi->private == NULL) {
		return ENOMEM;
	}

	fpi->private->flutterpi = fpi;
	fpi->private->rawkb = rawkb_get_instance_via_plugin_registry(fpi->plugin_registry);
	fpi->private->textin = textin_get_instance_via_plugin_registry(fpi->plugin_registry);

	// update window size
	engine_result = engine_lib->FlutterEngineSendWindowMetricsEvent(
		fpi->flutter.engine,
		&(FlutterWindowMetricsEvent) {
			.struct_size = sizeof(FlutterWindowMetricsEvent),
			.width = fpi->view.width,
			.height = fpi->view.height,
			.pixel_ratio = fpi->display.pixel_ratio
		}
	);
	if (engine_result != kSuccess) {
		fprintf(stderr, "[flutter-pi] Could not send window metrics to flutter engine.\n");
		return EINVAL;
	}

	engine_result = engine_lib->FlutterEngineRunInitialized(fpi->flutter.engine);
	if (engine_result != kSuccess) {
		fprintf(stderr, "[flutter-pi] Could not run the flutter engine. FlutterEngineRunInitialized: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
		return EINVAL;
	}
	
	return 0;
}

int flutterpi_schedule_exit(struct flutterpi *flutterpi) {
	(void) flutterpi;
	/// TODO: Implement
	return 0;
}

static int setup_paths(
	const char *asset_bundle_path,
	enum flutter_runtime_mode runtime_mode,
	char **kernel_blob_path_out,
	char **icu_data_path_out,
	char **app_elf_path_out
) {
	char *kernel_blob_path;
	char *icu_data_path;
	char *app_elf_path;
	int ok;
	
#	define PATH_EXISTS(path) (access((path), R_OK) == 0)

	if (!PATH_EXISTS(asset_bundle_path)) {
		fprintf(stderr, "Asset Bundle Directory \"%s\" does not exist\n", asset_bundle_path);
		return ENOENT;
	}
	
	ok = asprintf(&kernel_blob_path, "%s/kernel_blob.bin", asset_bundle_path);
	if (ok == -1) {
		return ENOMEM;
	}
	
	ok = asprintf(&app_elf_path, "%s/app.so", asset_bundle_path);
	if (ok == -1) {
		return ENOMEM;
	}

	if (runtime_mode == kDebug) {
		if (!PATH_EXISTS(kernel_blob_path)) {
			LOG_FLUTTERPI_ERROR("Could not find \"kernel.blob\" file inside \"%s\", which is required for debug mode.\n", asset_bundle_path);
			free(kernel_blob_path);
			free(app_elf_path);
			return ENOENT;
		}
	} else if (runtime_mode == kRelease) {
		if (!PATH_EXISTS(app_elf_path)) {
			LOG_FLUTTERPI_ERROR("Could not find \"app.so\" file inside \"%s\", which is required for release and profile mode.\n", asset_bundle_path);
			free(kernel_blob_path);
			free(app_elf_path);
			return ENOENT;
		}
	}

	ok = asprintf(&icu_data_path, "/usr/lib/icudtl.dat");
	if (ok == -1) {
		return ENOMEM;
	}

	if (!PATH_EXISTS(icu_data_path)) {
		LOG_FLUTTERPI_ERROR("Could not find \"icudtl.dat\" file inside \"/usr/lib/\".\n");
		free(kernel_blob_path);
		free(app_elf_path);
		free(icu_data_path);
		return ENOENT;
	}

	*kernel_blob_path_out = kernel_blob_path;
	*icu_data_path_out = icu_data_path;
	*app_elf_path_out = app_elf_path;

	return 0;

#	undef PATH_EXISTS
}

/*
static int create_drmdev_egl_output(
	struct drmdev *drmdev,
	struct libegl *libegl,
	struct egl_client_info *egl_client_info,
	struct gbm_device **gbm_device_out,
	struct gbm_surface **gbm_surface_out,
	EGLDisplay *egl_display_out,
	struct egl_display_info **egl_display_info_out,
	EGLConfig *egl_config_out,
	EGLSurface *egl_surface_out,
	EGLContext *egl_context_out
) {
	struct egl_display_info *egl_display_info;
	struct gbm_surface *gbm_surface;
	struct gbm_device *gbm_device;
	EGLDisplay egl_display;
	EGLSurface egl_surface;
	EGLContext egl_context;
	EGLBoolean egl_ok;
	EGLConfig egl_config;
	EGLint major, minor, egl_error, n_matched;
	int ok;

	static const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NATIVE_VISUAL_ID, DRM_FORMAT_ARGB8888,
		EGL_NONE
	};

	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	
	gbm_device = gbm_create_device(drmdev->fd);
	if (gbm_device == NULL) {
		LOG_FLUTTERPI_ERROR("Could not create GBM device. gbm_create_device: %s\n", strerror(errno));
		ok = errno;
		goto fail_return_ok;
	}

	gbm_surface = gbm_surface_create_with_modifiers(
		gbm_device,
		drmdev->selected_mode->hdisplay,
		drmdev->selected_mode->vdisplay,
		DRM_FORMAT_ARGB8888,
		(uint64_t[1]) {DRM_FORMAT_MOD_LINEAR},
		1
	);
	if (gbm_surface == NULL) {
		LOG_FLUTTERPI_ERROR("Could not create GBM Surface. gbm_surface_create_with_modifiers: %s\n", strerror(errno));
		ok = errno;
		goto fail_destroy_gbm_device;
	}

	if (libegl->eglGetPlatformDisplay != NULL) {
		egl_display = libegl->eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_device, NULL);
		if (egl_error = eglGetError(), egl_display == EGL_NO_DISPLAY || egl_error != EGL_SUCCESS) {
			LOG_FLUTTERPI_ERROR("Could not get EGL display! eglGetPlatformDisplay: 0x%08X\n", egl_error);
			ok = EIO;
			goto fail_destroy_gbm_surface;
		}
	} else if (egl_client_info->supports_ext_platform_base && egl_client_info->supports_khr_platform_gbm) {
		egl_display = libegl->eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm_device, NULL);
		if (egl_error = eglGetError(), egl_display == EGL_NO_DISPLAY || egl_error != EGL_SUCCESS) {
			LOG_FLUTTERPI_ERROR("Could not get EGL display! eglGetPlatformDisplayEXT: 0x%08X\n", egl_error);
			ok = EIO;
			goto fail_destroy_gbm_surface;
		}
	} else {
		egl_display = eglGetDisplay((void*) gbm_device);
		if (egl_error = eglGetError(), egl_display == EGL_NO_DISPLAY || egl_error != EGL_SUCCESS) {
			LOG_FLUTTERPI_ERROR("Could not get EGL display! eglGetDisplay: 0x%08X\n", egl_error);
			ok = EIO;
			goto fail_destroy_gbm_surface;
		}
	}

	egl_ok = eglInitialize(egl_display, &major, &minor);
	if (egl_error = eglGetError(), egl_ok == EGL_FALSE || egl_error != EGL_SUCCESS) {
		LOG_FLUTTERPI_ERROR("Could not initialize EGL display! eglInitialize: 0x%08X\n", egl_error);
		ok = EIO;
		goto fail_destroy_gbm_surface;
	}

	egl_display_info = egl_display_info_new(libegl, major, minor, egl_display);
	if (egl_display_info == NULL) {
		LOG_FLUTTERPI_ERROR("Could not create EGL display info!\n");
		ok = EIO;
		goto fail_terminate_display;
	}

	// We take the first config with ARGB8888. EGL orders all matching configs
	// so the top ones are most "desirable", so we should be fine
	// with just fetching the first config.
	egl_ok = eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &n_matched);
	if (egl_error = eglGetError(), egl_ok == EGL_FALSE || egl_error != EGL_SUCCESS) {
		LOG_FLUTTERPI_ERROR("Error finding a hardware accelerated EGL framebuffer configuration. eglChooseConfig: 0x%08X\n", egl_error);
		ok = EIO;
		goto fail_destroy_egl_display_info;
	}

	if (n_matched == 0) {
		LOG_FLUTTERPI_ERROR("Couldn't configure a hardware accelerated EGL framebuffer configuration.\n");
		ok = EIO;
		goto fail_destroy_egl_display_info;
	}

	egl_ok = eglBindAPI(EGL_OPENGL_ES_API);
	if (egl_error = eglGetError(), egl_ok == EGL_FALSE || egl_error != EGL_SUCCESS) {
		LOG_FLUTTERPI_ERROR("Failed to bind OpenGL ES API! eglBindAPI: 0x%08X\n", egl_error);
		ok = EIO;
		goto fail_destroy_egl_display_info;
	}

	egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attribs);
	if (egl_error = eglGetError(), egl_context == EGL_NO_CONTEXT || egl_error != EGL_SUCCESS) {
		LOG_FLUTTERPI_ERROR("Could not create OpenGL ES context. eglCreateContext: 0x%08X\n", egl_error);
		ok = EIO;
		goto fail_destroy_egl_display_info;
	}

	egl_surface = eglCreateWindowSurface(egl_display, egl_config, (EGLNativeWindowType) gbm_surface, NULL);
	if (egl_error = eglGetError(), egl_surface == EGL_NO_SURFACE || egl_error != EGL_SUCCESS) {
		LOG_FLUTTERPI_ERROR("Could not create EGL window surface. eglCreateWindowSurface: 0x%08X\n", egl_error);
		ok = EIO;
		goto fail_destroy_egl_context;
	}

	*gbm_device_out = gbm_device;
	*gbm_surface_out = gbm_surface;
	*egl_display_out = egl_display;
	*egl_display_info_out = egl_display_info;
	*egl_config_out = egl_config;
	*egl_surface_out = egl_surface;
	*egl_context_out = egl_context;
	
	return 0;

	fail_destroy_egl_context:
	eglDestroyContext(egl_display, egl_context);

	fail_destroy_egl_display_info:
	egl_display_info_destroy(egl_display_info);

	fail_terminate_display:
	eglTerminate(egl_display);

	fail_destroy_gbm_surface:
	gbm_surface_destroy(gbm_surface);

	fail_destroy_gbm_device:
	gbm_device_destroy(gbm_device);

	fail_return_ok:
	return ok;
}
*/

struct flutterpi *flutterpi_new_from_args(
	enum flutter_runtime_mode runtime_mode,
	bool has_rotation,
	int rotation,
	bool has_orientation,
	enum device_orientation orientation,
	bool has_explicit_dimensions,
	unsigned int width_mm, unsigned int height_mm,
	char *asset_bundle_path,
	int n_engine_args,
	const char **engine_args
) {
	struct flutterpi_private *private;
	struct flutter_messenger *messenger;
	struct libflutter_engine *engine_lib;
	struct texture_registry *texture_registry;
	struct egl_display_info *egl_display_info;
	struct plugin_registry *plugin_registry;
	struct egl_client_info *egl_client_info;
	FlutterEngineResult engine_result;
	struct gbm_surface *gbm_surface;
	struct event_loop *platform, *render;
	struct gbm_device *gbm_device;
	struct compositor *compositor;
	struct user_input *input;
	struct flutterpi *flutterpi;
	struct display *main_display;
	struct libegl *libegl;
	struct kmsdev *kmsdev;
	struct fbdev *fbdev;
	EGLDisplay egl_display;
	EGLSurface egl_surface;
	EGLContext egl_context;
	EGLConfig egl_config;
	char **engine_argv_dup;
	char *kernel_blob_path, *app_elf_path, *icu_data_path;
	bool use_kms;
	int ok;
	
	// allocate memory for our flutterpi instance
	flutterpi = malloc(sizeof *flutterpi);
	if (flutterpi == NULL) {
		goto fail_return_null;
	}

	private = malloc(sizeof *private);
	if (private == NULL) {
		goto fail_free_flutterpi;
	}

	// copy the asset bundle path and engine options,
	// so the caller doesn't need to pass the ownership
	asset_bundle_path = strdup(asset_bundle_path);
	if (asset_bundle_path == NULL) {
		goto fail_free_private;
	}

	engine_argv_dup = calloc(n_engine_args + 1, sizeof engine_args);
	if (engine_argv_dup == NULL) {
		goto fail_free_asset_bundle_path;
	}

	engine_argv_dup[0] = strdup("flutter-pi");
	if (engine_argv_dup[0] == NULL) {
		goto fail_free_engine_args;
	}
	
	for (int i = 0; i < n_engine_args; i++) {
		engine_args[i + 1] = strdup(engine_args[i]);
		if (engine_args[i + 1] == NULL) {
			goto fail_free_engine_args;
		}
	}

	// setup the paths to our kernel_blob.bin, icudtl.dat and app.so files
	// depending on the specified asset bundle path and runtime mode 
	ok = setup_paths(
		asset_bundle_path,
		runtime_mode,
		&kernel_blob_path,
		&icu_data_path,
		&app_elf_path
	);
	if (ok != 0) {
		goto fail_free_engine_args;
	}

	platform = event_loop_create(true, pthread_self());
	if (platform == NULL) {
		goto fail_free_paths;
	}

	render = event_loop_create(false, (pthread_t) 0);
	if (render == NULL) {
		goto fail_free_platform_event_loop;
	}

	// initialize the display
	kmsdev = NULL;
	fbdev = NULL;
	if (use_kms) {
		kmsdev = kmsdev_new_auto(render);
		if (kmsdev == NULL) {
			goto fail_free_paths;
		}

		for (int i = 0; i < kmsdev_get_n_connectors(kmsdev); i++) {
			if (kmsdev_is_connector_connected(kmsdev, i)) {
				ok = kmsdev_configure_crtc_with_preferences(
					kmsdev,
					0,
					i,
					(enum kmsdev_mode_preference[]) {
						kKmsdevModePreferencePreferred,
						kKmsdevModePreferenceHighestRefreshrate,
						kKmsdevModePreferenceHighestResolution,
						kKmsdevModePreferenceProgressive,
						kKmsdevModePreferenceNone
					}
				);
				if (ok != 0) {
					goto fail_destroy_kmsdev;
				}
			}
		}
	} else {
		fbdev = fbdev_new_from_path("/dev/fb0");
		if (fbdev == NULL) {
			goto fail_free_paths;
		}
	}

	libegl = libegl_load();
	if (libegl == NULL) {
		goto fail_destroy_gbm_device_and_surface;
	}

	egl_client_info = egl_client_info_new(libegl);
	if (egl_client_info == NULL) {
		goto fail_unload_libegl;
	}

	ok = create_drmdev_egl_output(
		drmdev,
		libegl,
		egl_client_info,
		&gbm_device,
		&gbm_surface,
		&egl_display,
		&egl_display_info,
		&egl_config,
		&egl_surface,
		&egl_context
	);
	if (ok != 0) {
		goto fail_destroy_egl_client_info;
	}

	compositor = compositor_new(
		use_kms ? &(const struct graphics_output) {
			.type = kGraphicsOutputTypeKmsdev,
			.kmsdev = kmsdev
		} : &(const struct graphics_output) {
			.type = kGraphicsOutputTypeFbdev,
			.fbdev = fbdev
		},
		kOpenGL,
		renderer,
		NULL,
		render
	);
	if (compositor == NULL) {
		goto fail_destroy_egl_output;
	}

	engine_lib = libflutter_engine_load_for_runtime_mode(runtime_mode);
	if (engine_lib == NULL) {
		goto fail_destroy_compositor;
	}
	
	bool engine_is_aot = engine_lib->FlutterEngineRunsAOTCompiledDartCode();
	if ((engine_is_aot == true) && (fpi->flutter.runtime_mode != kRelease)) {
		fprintf(
			stderr,
			"[flutter-pi] The flutter engine was built for release (AOT) mode,\n"
			"             but flutter-pi was not started up in release mode. \n"
			"             Either you swap out the libflutter_engine.so \n"
			"             with one that was built for debug mode, or you start\n"
			"             flutter-pi with the --release flag and make sure\n"
			"             a valid \"app.so\" is located inside the asset bundle\n"
			"             directory.\n"
		);
		return EINVAL;
	} else if ((engine_is_aot == false) && (fpi->flutter.runtime_mode != kDebug)) {
		fprintf(
			stderr,
			"[flutter-pi] The flutter engine was built for debug mode,\n"
			"             but flutter-pi was started up in release mode.\n"
			"             Either you swap out the libflutter_engine.so\n"
			"             with one that was built for release mode, or you\n"
			"             start flutter-pi without the --release flag.\n"
		);
		return EINVAL;
	}

	if (fpi->flutter.runtime_mode == kRelease) {
		aot_source = (FlutterEngineAOTDataSource) {
			.elf_path = fpi->flutter.app_elf_path,
			.type = kFlutterEngineAOTDataSourceTypeElfPath
		};

		engine_result = engine_lib->FlutterEngineCreateAOTData(&aot_source, &aot_data);
		if (engine_result != kSuccess) {
			fprintf(stderr, "[flutter-pi] Could not load AOT data. FlutterEngineCreateAOTData: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
			return EIO;
		}
	}

	FlutterRendererConfig renderer_config;

	struct renderer *renderer = gl_renderer_new(

	);

	renderer_fill_flutter_renderer_config(renderer, &renderer_config);

	engine_result = engine_lib->FlutterEngineInitialize(
		FLUTTER_ENGINE_VERSION,
		&renderer_config,
		&(FlutterProjectArgs) {
			.struct_size = sizeof(FlutterProjectArgs),
			.assets_path = asset_bundle_path,
			.main_path__unused__ = NULL,
			.packages_path__unused__ = NULL,
			.icu_data_path = icu_data_path,
			.command_line_argc = n_engine_args,
			.command_line_argv = (const char * const*) engine_args,
			.platform_message_callback = on_platform_message,
			.vm_snapshot_data = NULL,
			.vm_snapshot_data_size = 0,
			.vm_snapshot_instructions = NULL,
			.vm_snapshot_instructions_size = 0,
			.isolate_snapshot_data = NULL,
			.isolate_snapshot_data_size = 0,
			.isolate_snapshot_instructions = NULL,
			.isolate_snapshot_instructions_size = 0,
			.root_isolate_create_callback = NULL,
			.update_semantics_node_callback = NULL,
			.update_semantics_custom_action_callback = NULL,
			.persistent_cache_path = NULL,
			.is_persistent_cache_read_only = false,
			.vsync_callback = on_frame_request,
			.custom_dart_entrypoint = NULL,
			.custom_task_runners = &(FlutterCustomTaskRunners) {
				.struct_size = sizeof(FlutterCustomTaskRunners),
				.platform_task_runner = &(FlutterTaskRunnerDescription) {
					.struct_size = sizeof(FlutterTaskRunnerDescription),
					.user_data = flutterpi,
					.runs_task_on_current_thread_callback = runs_platform_tasks_on_current_thread,
					.post_task_callback = on_post_flutter_task
				},
				.render_task_runner = NULL
			},
			.shutdown_dart_vm_when_done = true,
			.compositor = &flutter_compositor,
			.dart_old_gen_heap_size = -1,
			.aot_data = aot_data,
			.compute_platform_resolved_locale_callback = NULL,
			.dart_entrypoint_argc = 0,
			.dart_entrypoint_argv = NULL
		},
		flutterpi,
		&engine
	);
	if (engine_result != kSuccess) {
		fprintf(stderr, "[flutter-pi] Could not run the flutter engine. FlutterEngineInitialize: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
		return EINVAL;
	}
	// engine = create_engine();

	// create the user input "subsystem".
	// display needs to be initialized before this so we can set
	// the correct view here.
	input = user_input_new(
		&user_input_interface, flutterpi,
		&FLUTTER_TRANSLATION_TRANSFORMATION(0, 0),
		0, 0
	);
	if (input == NULL) {
		LOG_FLUTTERPI_ERROR("ERROR: Couldn't initialize user input.\n");
		goto fail_free_paths;
	}

	// initialize the plugin registry
	plugin_registry = plugin_registry_new(flutterpi);
	if (plugin_registry == NULL) {
		LOG_FLUTTERPI_ERROR("ERROR: Couldn't initialize plugin registry.\n");
		goto fail_free_paths;
	}




	private->flutterpi = flutterpi;
	private->rawkb = NULL;
	private->textin = NULL;

	memset(flutterpi, 0, sizeof *flutterpi);

	/// TODO: Add more initializers in case the struct changes.
	flutterpi->private = private;
	flutterpi->input.cursor_x = 0.0;
	flutterpi->input.cursor_y = 0.0;

	if (has_explicit_dimensions) {
		flutterpi->display.width_mm = width_mm;
		flutterpi->display.height_mm = height_mm;
	}

	flutterpi->view.has_rotation = has_rotation;
	flutterpi->view.rotation = rotation;
	flutterpi->view.has_orientation = has_orientation;
	flutterpi->view.orientation = orientation;

	flutterpi->flutter.runtime_mode = runtime_mode;
	flutterpi->flutter.asset_bundle_path = asset_bundle_path;
	flutterpi->flutter.engine_argc = n_engine_args + 1;
	flutterpi->flutter.engine_argv = engine_argv_dup;

	return flutterpi;

	fail_free_input:
	//user_input_destroy(input);

	fail_destroy_compositor:
	compositor_destroy(compositor);

	fail_destroy_egl_output:
	eglDestroyContext(egl_display, egl_context);
	eglDestroySurface(egl_display, egl_surface);
	egl_display_info_destroy(egl_display_info);
	eglTerminate(egl_display);
	gbm_surface_destroy(gbm_surface);
	gbm_device_destroy(gbm_device);

	fail_destroy_egl_client_info:
	egl_client_info_destroy(egl_client_info);

	fail_unload_libegl:
	libegl_unload(libegl);

	fail_destroy_gbm_device_and_surface:
	gbm_surface_destroy(gbm_surface);
	gbm_device_destroy(gbm_device);

	fail_destroy_kmsdev:
	kmsdev_destroy(kmsdev);

	fail_free_render_event_loop:
	event_loop_destroy(render);

	fail_free_platform_event_loop:
	event_loop_destroy(platform);

	fail_free_paths:
	if (kernel_blob_path != NULL) {
		free(kernel_blob_path);
	}
	if (app_elf_path != NULL) {
		free(app_elf_path);
	}
	if (icu_data_path != NULL) {
		free(icu_data_path);
	}

	fail_free_engine_args:
	for (int i = 0; i < (n_engine_args + 1); i++) {
		if (engine_argv_dup[i] != NULL) {
			free(engine_argv_dup[i]);
		} else {
			break;
		}
	}
	free(engine_argv_dup);

	fail_free_asset_bundle_path:
	free(asset_bundle_path);	

	fail_free_private:
	free(private);

	fail_free_flutterpi:
	free(flutterpi);

	fail_return_null:
	return NULL;
}

struct flutterpi *flutterpi_new_from_cmdline(
	int argc,
	char **argv
) {
	enum device_orientation orientation;
	unsigned int width_mm, height_mm;
	glob_t *input_devices_glob = {0};
	bool has_orientation, has_rotation, has_explicit_dimensions, input_specified, finished_parsing_options;
	int runtime_mode_int, rotation, ok, opt;

	struct option long_options[] = {
		{"release", no_argument, &runtime_mode_int, kRelease},
		{"input", required_argument, NULL, 'i'},
		{"orientation", required_argument, NULL, 'o'},
		{"rotation", required_argument, NULL, 'r'},
		{"dimensions", required_argument, NULL, 'd'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};

	runtime_mode_int = kDebug;
	has_orientation = false;
	has_rotation = false;
	input_specified = false;
	finished_parsing_options = false;

	while (!finished_parsing_options) {
		opt = getopt_long(argc, argv, "+i:o:r:d:h", long_options, NULL);

		switch (opt) {
			case 0:
				// flag was encountered. just continue
				break;
			case 'i':
				if (input_devices_glob == NULL) {
					input_devices_glob = calloc(1, sizeof *input_devices_glob);
					if (input_devices_glob == NULL) {
						fprintf(stderr, "Out of memory\n");
						return NULL;
					}
				}

				glob(optarg, GLOB_BRACE | GLOB_TILDE | (input_specified ? GLOB_APPEND : 0), NULL, input_devices_glob);
				input_specified = true;
				break;

			case 'o':
				if (STREQ(optarg, "portrait_up")) {
					orientation = kPortraitUp;
					has_orientation = true;
				} else if (STREQ(optarg, "landscape_left")) {
					orientation = kLandscapeLeft;
					has_orientation = true;
				} else if (STREQ(optarg, "portrait_down")) {
					orientation = kPortraitDown;
					has_orientation = true;
				} else if (STREQ(optarg, "landscape_right")) {
					orientation = kLandscapeRight;
					has_orientation = true;
				} else {
					fprintf(
						stderr, 
						"ERROR: Invalid argument for --orientation passed.\n"
						"Valid values are \"portrait_up\", \"landscape_left\", \"portrait_down\", \"landscape_right\".\n"
						"%s", 
						usage
					);
					return NULL;
				}

				break;
			
			case 'r':
				errno = 0;
				long rotation_long = strtol(optarg, NULL, 0);
				if ((errno != 0) || ((rotation_long != 0) && (rotation_long != 90) && (rotation_long != 180) && (rotation_long != 270))) {
					fprintf(
						stderr,
						"ERROR: Invalid argument for --rotation passed.\n"
						"Valid values are 0, 90, 180, 270.\n"
						"%s",
						usage
					);
					return NULL;
				}

				rotation = rotation_long;
				has_rotation = true;
				break;
			
			case 'd': ;
				ok = sscanf(optarg, "%u,%u", &width_mm, &height_mm);
				if ((ok == 0) || (ok == EOF)) {
					fprintf(stderr, "ERROR: Invalid argument for --dimensions passed.\n%s", usage);
					return NULL;
				}

				has_explicit_dimensions = true;

				break;
			
			case 'h':
				printf("%s", usage);
				return NULL;

			case '?':
			case ':':
				fprintf(stderr, "Invalid option specified.\n%s", usage);
				return NULL;
			
			case -1:
				finished_parsing_options = true;
				break;
			
			default:
				break;
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "error: expected asset bundle path after options.\n");
		printf("%s", usage);
		return NULL;
	}

	return flutterpi_new_from_args(
		(enum flutter_runtime_mode) runtime_mode_int,
		has_rotation, rotation,
		has_orientation, orientation,
		has_explicit_dimensions, width_mm, height_mm,
		input_devices_glob,
		argv[optind],
		argc - (optind + 1),
		(const char **) &argv[optind + 1]
	);
}

int flutterpi_init(struct flutterpi *flutterpi) {
	int ok;

	ok = setup_paths(
		flutterpi->flutter.asset_bundle_path,
		flutterpi->flutter.runtime_mode,
		&flutterpi->flutter.kernel_blob_path,
		&flutterpi->flutter.icu_data_path,
		&flutterpi->flutter.app_elf_path
	);
	if (ok == false) {
		return EINVAL;
	}

	ok = init_main_loop(flutterpi);
	if (ok != 0) {
		return ok;
	}

	ok = init_display(flutterpi);
	if (ok != 0) {
		return ok;
	}

	ok = init_user_input(flutterpi);
	if (ok != 0) {
		return ok;
	}

	ok = init_application(flutterpi);
	if (ok != 0) {
		return ok;
	}

	return 0;
}

int flutterpi_run(struct flutterpi *flutterpi) {
	return run_main_loop(flutterpi);
}

void flutterpi_deinit(struct flutterpi *flutterpi) {
	return;
}


int main(int argc, char **argv) {
	struct flutterpi *flutterpi;
	int ok;

	flutterpi = flutterpi_new_from_cmdline(argc, argv);
	if (flutterpi == NULL) {
		return EXIT_FAILURE;
	}

	ok = flutterpi_init(flutterpi);
	if (ok != 0) {
		return EXIT_FAILURE;
	}

	ok = flutterpi_run(flutterpi);
	if (ok != 0) {
		return EXIT_FAILURE;
	}

	flutterpi_deinit(flutterpi);

	return EXIT_SUCCESS;
}
