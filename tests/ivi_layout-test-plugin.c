/*
 * Copyright © 2012 Intel Corporation
 * Copyright © 2013 DENSO CORPORATION
 * Copyright © 2015 Collabora, Ltd.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <assert.h>

#include "../src/compositor.h"
#include "weston-test-server-protocol.h"
#include "ivi-test.h"
#include "../ivi-shell/ivi-layout-export.h"

struct test_context;

struct runner_test {
	const char *name;
	void (*run)(struct test_context *);
} __attribute__ ((aligned (32)));

#define RUNNER_TEST(name)					\
	static void runner_func_##name(struct test_context *);	\
								\
	const struct runner_test runner_test_##name		\
		__attribute__ ((section ("test_section"))) =	\
	{							\
		#name, runner_func_##name			\
	};							\
								\
	static void runner_func_##name(struct test_context *ctx)

extern const struct runner_test __start_test_section;
extern const struct runner_test __stop_test_section;

static const struct runner_test *
find_runner_test(const char *name)
{
	const struct runner_test *t;

	for (t = &__start_test_section; t < &__stop_test_section; t++) {
		if (strcmp(t->name, name) == 0)
			return t;
	}

	return NULL;
}

struct test_launcher {
	struct weston_compositor *compositor;
	char exe[2048];
	struct weston_process process;
	const struct ivi_controller_interface *controller_interface;
};

struct test_context {
	const struct ivi_controller_interface *controller_interface;
	struct wl_resource *runner_resource;
	uint32_t user_flags;
};

static struct test_context static_context;

static void
destroy_runner(struct wl_resource *resource)
{
	assert(static_context.runner_resource == NULL ||
	       static_context.runner_resource == resource);

	static_context.controller_interface = NULL;
	static_context.runner_resource = NULL;
}

static void
runner_destroy_handler(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
runner_run_handler(struct wl_client *client, struct wl_resource *resource,
		   const char *test_name)
{
	struct test_launcher *launcher;
	const struct runner_test *t;

	assert(static_context.runner_resource == NULL ||
	       static_context.runner_resource == resource);

	launcher = wl_resource_get_user_data(resource);
	static_context.controller_interface = launcher->controller_interface;
	static_context.runner_resource = resource;

	t = find_runner_test(test_name);
	if (!t) {
		weston_log("Error: runner test \"%s\" not found.\n",
			   test_name);
		wl_resource_post_error(resource,
				       WESTON_TEST_RUNNER_ERROR_UNKNOWN_TEST,
				       "weston_test_runner: unknown: '%s'",
				       test_name);
		return;
	}

	weston_log("weston_test_runner.run(\"%s\")\n", test_name);

	t->run(&static_context);

	weston_test_runner_send_finished(resource);
}

static const struct weston_test_runner_interface runner_implementation = {
	runner_destroy_handler,
	runner_run_handler
};

static void
bind_runner(struct wl_client *client, void *data,
	    uint32_t version, uint32_t id)
{
	struct test_launcher *launcher = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client, &weston_test_runner_interface,
				      1, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &runner_implementation,
				       launcher, destroy_runner);

	if (static_context.runner_resource != NULL) {
		weston_log("test FATAL: "
			   "attempting to run several tests in parallel.\n");
		wl_resource_post_error(resource,
				       WESTON_TEST_RUNNER_ERROR_TEST_FAILED,
				       "attempt to run parallel tests");
	}
}

static void
test_client_sigchld(struct weston_process *process, int status)
{
	struct test_launcher *launcher =
		container_of(process, struct test_launcher, process);
	struct weston_compositor *c = launcher->compositor;

	/* Chain up from weston-test-runner's exit code so that automake
	 * knows the exit status and can report e.g. skipped tests. */
	if (WIFEXITED(status))
		weston_compositor_exit_with_code(c, WEXITSTATUS(status));
	else
		weston_compositor_exit_with_code(c, EXIT_FAILURE);
}

static void
idle_launch_client(void *data)
{
	struct test_launcher *launcher = data;
	pid_t pid;
	sigset_t allsigs;

	pid = fork();
	if (pid == -1) {
		weston_log("fatal: failed to fork '%s': %m\n", launcher->exe);
		weston_compositor_exit_with_code(launcher->compositor,
						 EXIT_FAILURE);
		return;
	}

	if (pid == 0) {
		sigfillset(&allsigs);
		sigprocmask(SIG_UNBLOCK, &allsigs, NULL);
		execl(launcher->exe, launcher->exe, NULL);
		weston_log("compositor: executing '%s' failed: %m\n",
			   launcher->exe);
		_exit(EXIT_FAILURE);
	}

	launcher->process.pid = pid;
	launcher->process.cleanup = test_client_sigchld;
	weston_watch_process(&launcher->process);
}

int
controller_module_init(struct weston_compositor *compositor,
		       int *argc, char *argv[],
		       const struct ivi_controller_interface *iface,
		       size_t iface_version);

WL_EXPORT int
controller_module_init(struct weston_compositor *compositor,
		       int *argc, char *argv[],
		       const struct ivi_controller_interface *iface,
		       size_t iface_version)
{
	struct wl_event_loop *loop;
	struct test_launcher *launcher;
	const char *path;

	/* strict check, since this is an internal test module */
	if (iface_version != sizeof(*iface)) {
		weston_log("fatal: controller interface mismatch\n");
		return -1;
	}

	path = getenv("WESTON_BUILD_DIR");
	if (!path) {
		weston_log("test setup failure: WESTON_BUILD_DIR not set\n");
		return -1;
	}

	launcher = zalloc(sizeof *launcher);
	if (!launcher)
		return -1;

	launcher->compositor = compositor;
	launcher->controller_interface = iface;
	snprintf(launcher->exe, sizeof launcher->exe,
		 "%s/ivi-layout.ivi", path);

	if (wl_global_create(compositor->wl_display,
			     &weston_test_runner_interface, 1,
			     launcher, bind_runner) == NULL)
		return -1;

	loop = wl_display_get_event_loop(compositor->wl_display);
	wl_event_loop_add_idle(loop, idle_launch_client, launcher);

	return 0;
}

static void
runner_assert_fail(const char *cond, const char *file, int line,
		   const char *func, struct test_context *ctx)
{
	weston_log("Assert failure in %s:%d, %s: '%s'\n",
		   file, line, func, cond);

	assert(ctx->runner_resource);
	wl_resource_post_error(ctx->runner_resource,
			       WESTON_TEST_RUNNER_ERROR_TEST_FAILED,
			       "Assert failure in %s:%d, %s: '%s'\n",
			       file, line, func, cond);
}

#define runner_assert(cond) ({					\
	bool b_ = (cond);					\
	if (!b_)						\
		runner_assert_fail(#cond, __FILE__, __LINE__,	\
				   __func__, ctx);		\
	b_;							\
})

#define runner_assert_or_return(cond) do {			\
	bool b_ = (cond);					\
	if (!b_) {						\
		runner_assert_fail(#cond, __FILE__, __LINE__,	\
				   __func__, ctx);		\
		return;						\
	}							\
} while (0)


/*************************** tests **********************************/

/*
 * This is a controller module: a plugin to ivi-shell.so, i.e. a sub-plugin.
 * This module is specially written to execute tests that target the
 * ivi_layout API.
 *
 * This module is listed in TESTS in Makefile.am. weston-tests-env handles
 * this module specially by loading it in ivi-shell.
 *
 * Once Weston init completes, this module launches one test program:
 * ivi-layout.ivi (ivi_layout-test.c). That program uses the weston-test-runner
 * framework to fork and exec each TEST() in ivi_layout-test.c with a fresh
 * connection to the single compositor instance.
 *
 * Each TEST() in ivi_layout-test.c will bind to weston_test_runner global
 * interface. A TEST() will set up the client state, and issue
 * weston_test_runner.run request to execute the compositor-side of the test.
 *
 * The compositor-side parts of the tests are in this file. They are specified
 * by RUNNER_TEST() macro, where the name argument matches the name string
 * passed to weston_test_runner.run.
 *
 * A RUNNER_TEST() function simply returns when it succeeds. If it fails,
 * a fatal protocol error is sent to the client from runner_assert() or
 * runner_assert_or_return(). This module catches the test program exit
 * code and passes it out of Weston to the test harness.
 *
 * A single TEST() in ivi_layout-test.c may use multiple RUNNER_TEST()s to
 * achieve multiple test points over a client action sequence.
 */

RUNNER_TEST(surface_create_p1)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf[2];
	uint32_t ivi_id;

	ivisurf[0] = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf[0]);

	ivisurf[1] = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(1));
	runner_assert(ivisurf[1]);

	ivi_id = ctl->get_id_of_surface(ivisurf[0]);
	runner_assert(ivi_id == IVI_TEST_SURFACE_ID(0));

	ivi_id = ctl->get_id_of_surface(ivisurf[1]);
	runner_assert(ivi_id == IVI_TEST_SURFACE_ID(1));
}

RUNNER_TEST(surface_create_p2)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;

	/* the ivi_surface was destroyed by the client */
	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf == NULL);
}

RUNNER_TEST(surface_visibility)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;
	int32_t ret;
	bool visibility;
	const struct ivi_layout_surface_properties *prop;

	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf);

	ret = ctl->surface_set_visibility(ivisurf, true);
	runner_assert(ret == IVI_SUCCEEDED);

	ctl->commit_changes();

	visibility = ctl->surface_get_visibility(ivisurf);
	runner_assert(visibility == true);

	prop = ctl->get_properties_of_surface(ivisurf);
	runner_assert(prop->visibility == true);
}

RUNNER_TEST(surface_opacity)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;
	int32_t ret;
	wl_fixed_t opacity;
	const struct ivi_layout_surface_properties *prop;

	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf);

	runner_assert(ctl->surface_get_opacity(ivisurf) ==
		      wl_fixed_from_double(1.0));

	ret = ctl->surface_set_opacity(ivisurf, wl_fixed_from_double(0.5));
	runner_assert(ret == IVI_SUCCEEDED);

	runner_assert(ctl->surface_get_opacity(ivisurf) ==
		      wl_fixed_from_double(1.0));

	ctl->commit_changes();

	opacity = ctl->surface_get_opacity(ivisurf);
	runner_assert(opacity == wl_fixed_from_double(0.5));

	prop = ctl->get_properties_of_surface(ivisurf);
	runner_assert(prop->opacity == wl_fixed_from_double(0.5));
}

RUNNER_TEST(surface_orientation)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;
	const struct ivi_layout_surface_properties *prop;

	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf != NULL);

	runner_assert(ctl->surface_get_orientation(ivisurf) ==
		      WL_OUTPUT_TRANSFORM_NORMAL);

	runner_assert(ctl->surface_set_orientation(
		      ivisurf, WL_OUTPUT_TRANSFORM_90) == IVI_SUCCEEDED);

	runner_assert(ctl->surface_get_orientation(ivisurf) ==
		      WL_OUTPUT_TRANSFORM_NORMAL);

	ctl->commit_changes();

	runner_assert(ctl->surface_get_orientation(
		      ivisurf) == WL_OUTPUT_TRANSFORM_90);

	prop = ctl->get_properties_of_surface(ivisurf);
	runner_assert_or_return(prop);
	runner_assert(prop->orientation == WL_OUTPUT_TRANSFORM_90);
}

RUNNER_TEST(surface_dimension)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;
	const struct ivi_layout_surface_properties *prop;
	int32_t dest_width;
	int32_t dest_height;

	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf != NULL);

	runner_assert(ctl->surface_get_dimension(
		      ivisurf, &dest_width, &dest_height) == IVI_SUCCEEDED);
	runner_assert(dest_width == 1);
	runner_assert(dest_height == 1);

	runner_assert(IVI_SUCCEEDED ==
		      ctl->surface_set_dimension(ivisurf, 200, 300));

	runner_assert(ctl->surface_get_dimension(
		      ivisurf, &dest_width, &dest_height) == IVI_SUCCEEDED);
	runner_assert(dest_width == 1);
	runner_assert(dest_height == 1);

	ctl->commit_changes();

	runner_assert(ctl->surface_get_dimension(
		      ivisurf, &dest_width, &dest_height) == IVI_SUCCEEDED);
	runner_assert(dest_width == 200);
	runner_assert(dest_height == 300);

	prop = ctl->get_properties_of_surface(ivisurf);
	runner_assert_or_return(prop);
	runner_assert(prop->dest_width == 200);
	runner_assert(prop->dest_height == 300);
}

RUNNER_TEST(surface_position)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;
	const struct ivi_layout_surface_properties *prop;
	int32_t dest_x;
	int32_t dest_y;

	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf != NULL);

	runner_assert(ctl->surface_get_position(
		      ivisurf, &dest_x, &dest_y) == IVI_SUCCEEDED);
	runner_assert(dest_x == 0);
	runner_assert(dest_y == 0);

	runner_assert(ctl->surface_set_position(
		      ivisurf, 20, 30) == IVI_SUCCEEDED);

	runner_assert(ctl->surface_get_position(
		      ivisurf, &dest_x, &dest_y) == IVI_SUCCEEDED);
	runner_assert(dest_x == 0);
	runner_assert(dest_y == 0);

	ctl->commit_changes();

	runner_assert(ctl->surface_get_position(
		      ivisurf, &dest_x, &dest_y) == IVI_SUCCEEDED);
	runner_assert(dest_x == 20);
	runner_assert(dest_y == 30);

	prop = ctl->get_properties_of_surface(ivisurf);
	runner_assert_or_return(prop);
	runner_assert(prop->dest_x == 20);
	runner_assert(prop->dest_y == 30);
}

RUNNER_TEST(surface_destination_rectangle)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;
	const struct ivi_layout_surface_properties *prop;
	int32_t dest_width;
	int32_t dest_height;
	int32_t dest_x;
	int32_t dest_y;

	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf != NULL);

	prop = ctl->get_properties_of_surface(ivisurf);
	runner_assert_or_return(prop);
	runner_assert(prop->dest_width == 1);
	runner_assert(prop->dest_height == 1);
	runner_assert(prop->dest_x == 0);
	runner_assert(prop->dest_y == 0);

	runner_assert(ctl->surface_set_destination_rectangle(
		      ivisurf, 20, 30, 200, 300) == IVI_SUCCEEDED);

	prop = ctl->get_properties_of_surface(ivisurf);
	runner_assert_or_return(prop);
	runner_assert(prop->dest_width == 1);
	runner_assert(prop->dest_height == 1);
	runner_assert(prop->dest_x == 0);
	runner_assert(prop->dest_y == 0);

	ctl->commit_changes();

	runner_assert(ctl->surface_get_dimension(
		      ivisurf, &dest_width, &dest_height) == IVI_SUCCEEDED);
	runner_assert(dest_width == 200);
	runner_assert(dest_height == 300);

	runner_assert(ctl->surface_get_position(ivisurf, &dest_x, &dest_y) == IVI_SUCCEEDED);
	runner_assert(dest_x == 20);
	runner_assert(dest_y == 30);

	prop = ctl->get_properties_of_surface(ivisurf);
	runner_assert_or_return(prop);
	runner_assert(prop->dest_width == 200);
	runner_assert(prop->dest_height == 300);
	runner_assert(prop->dest_x == 20);
	runner_assert(prop->dest_y == 30);
}

RUNNER_TEST(surface_source_rectangle)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;
	const struct ivi_layout_surface_properties *prop;

	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf != NULL);

	prop = ctl->get_properties_of_surface(ivisurf);
	runner_assert_or_return(prop);
	runner_assert(prop->source_width == 0);
	runner_assert(prop->source_height == 0);
	runner_assert(prop->source_x == 0);
	runner_assert(prop->source_y == 0);

	runner_assert(ctl->surface_set_source_rectangle(
		      ivisurf, 20, 30, 200, 300) == IVI_SUCCEEDED);

	prop = ctl->get_properties_of_surface(ivisurf);
	runner_assert_or_return(prop);
	runner_assert(prop->source_width == 0);
	runner_assert(prop->source_height == 0);
	runner_assert(prop->source_x == 0);
	runner_assert(prop->source_y == 0);

	ctl->commit_changes();

	prop = ctl->get_properties_of_surface(ivisurf);
	runner_assert_or_return(prop);
	runner_assert(prop->source_width == 200);
	runner_assert(prop->source_height == 300);
	runner_assert(prop->source_x == 20);
	runner_assert(prop->source_y == 30);
}

RUNNER_TEST(surface_bad_opacity)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;
	wl_fixed_t opacity;

	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf != NULL);

	runner_assert(ctl->surface_set_opacity(
		      NULL, wl_fixed_from_double(0.3)) == IVI_FAILED);

	runner_assert(ctl->surface_set_opacity(
		      ivisurf, wl_fixed_from_double(0.3)) == IVI_SUCCEEDED);

	runner_assert(ctl->surface_set_opacity(
		      ivisurf, wl_fixed_from_double(-1)) == IVI_FAILED);

	ctl->commit_changes();

	opacity = ctl->surface_get_opacity(ivisurf);
	runner_assert(opacity == wl_fixed_from_double(0.3));

	runner_assert(ctl->surface_set_opacity(
		      ivisurf, wl_fixed_from_double(1.1)) == IVI_FAILED);

	ctl->commit_changes();

	opacity = ctl->surface_get_opacity(ivisurf);
	runner_assert(opacity == wl_fixed_from_double(0.3));

	runner_assert(ctl->surface_set_opacity(
		      NULL, wl_fixed_from_double(0.5)) == IVI_FAILED);

	ctl->commit_changes();

	opacity = ctl->surface_get_opacity(NULL);
	runner_assert(opacity == wl_fixed_from_double(0.0));
}

RUNNER_TEST(ivi_layout_commit_changes)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;

	ctl->commit_changes();
}

RUNNER_TEST(commit_changes_after_visibility_set_surface_destroy)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;

	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf != NULL);
	runner_assert(ctl->surface_set_visibility(
		      ivisurf, true) == IVI_SUCCEEDED);
}

RUNNER_TEST(commit_changes_after_opacity_set_surface_destroy)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;

	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf != NULL);
	runner_assert(ctl->surface_set_opacity(
		      ivisurf, wl_fixed_from_double(0.5)) == IVI_SUCCEEDED);
}

RUNNER_TEST(commit_changes_after_orientation_set_surface_destroy)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;

	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf != NULL);
	runner_assert(ctl->surface_set_orientation(
		      ivisurf, WL_OUTPUT_TRANSFORM_90) == IVI_SUCCEEDED);
}

RUNNER_TEST(commit_changes_after_dimension_set_surface_destroy)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;

	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf != NULL);
	runner_assert(ctl->surface_set_dimension(
		      ivisurf, 200, 300) == IVI_SUCCEEDED);
}

RUNNER_TEST(commit_changes_after_position_set_surface_destroy)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;

	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf != NULL);
	runner_assert(ctl->surface_set_position(
		      ivisurf, 20, 30) == IVI_SUCCEEDED);
}

RUNNER_TEST(commit_changes_after_source_rectangle_set_surface_destroy)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;

	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf != NULL);
	runner_assert(ctl->surface_set_source_rectangle(
		      ivisurf, 20, 30, 200, 300) == IVI_SUCCEEDED);
}

RUNNER_TEST(commit_changes_after_destination_rectangle_set_surface_destroy)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;

	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf != NULL);
	runner_assert(ctl->surface_set_destination_rectangle(
		      ivisurf, 20, 30, 200, 300) == IVI_SUCCEEDED);
}

RUNNER_TEST(get_surface_after_destroy_surface)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;

	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert(ivisurf == NULL);
}

RUNNER_TEST(layer_render_order)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_layer *ivilayer;
	struct ivi_layout_surface *ivisurfs[IVI_TEST_SURFACE_COUNT] = {};
	struct ivi_layout_surface **array;
	int32_t length = 0;
	uint32_t i;

	ivilayer = ctl->layer_create_with_dimension(IVI_TEST_LAYER_ID(0), 200, 300);

	for (i = 0; i < IVI_TEST_SURFACE_COUNT; i++) {
		ivisurfs[i] = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(i));
	}

	runner_assert(ctl->layer_set_render_order(
		      ivilayer, ivisurfs, IVI_TEST_SURFACE_COUNT) == IVI_SUCCEEDED);

	ctl->commit_changes();

	runner_assert(ctl->get_surfaces_on_layer(
		      ivilayer, &length, &array) == IVI_SUCCEEDED);
	runner_assert(IVI_TEST_SURFACE_COUNT == length);
	for (i = 0; i < IVI_TEST_SURFACE_COUNT; i++) {
		runner_assert(array[i] == ivisurfs[i]);
	}

	if (length > 0) {
		free(array);
	}

	runner_assert(ctl->layer_set_render_order(
		      ivilayer, NULL, 0) == IVI_SUCCEEDED);

	array = NULL;

	ctl->commit_changes();

	runner_assert(ctl->get_surfaces_on_layer(
		      ivilayer, &length, &array) == IVI_SUCCEEDED);
	runner_assert(length == 0 && array == NULL);

	ctl->layer_remove(ivilayer);
}

RUNNER_TEST(test_layer_render_order_destroy_one_surface_p1)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_layer *ivilayer;
	struct ivi_layout_surface *ivisurfs[IVI_TEST_SURFACE_COUNT] = {};
	struct ivi_layout_surface **array;
	int32_t length = 0;
	uint32_t i;

	ivilayer = ctl->layer_create_with_dimension(IVI_TEST_LAYER_ID(0), 200, 300);

	for (i = 0; i < IVI_TEST_SURFACE_COUNT; i++) {
		ivisurfs[i] = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(i));
	}

	runner_assert(ctl->layer_set_render_order(
		      ivilayer, ivisurfs, IVI_TEST_SURFACE_COUNT) == IVI_SUCCEEDED);

	ctl->commit_changes();

	runner_assert(ctl->get_surfaces_on_layer(
		      ivilayer, &length, &array) == IVI_SUCCEEDED);
	runner_assert_or_return(IVI_TEST_SURFACE_COUNT == length);
	for (i = 0; i < IVI_TEST_SURFACE_COUNT; i++) {
		runner_assert(array[i] == ivisurfs[i]);
	}

	if (length > 0) {
		free(array);
	}
}

RUNNER_TEST(test_layer_render_order_destroy_one_surface_p2)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_layer *ivilayer;
	struct ivi_layout_surface *ivisurfs[2] = {};
	struct ivi_layout_surface **array;
	int32_t length = 0;
	uint32_t i;

	ivilayer = ctl->get_layer_from_id(IVI_TEST_LAYER_ID(0));
	ivisurfs[0] = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	ivisurfs[1] = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(2));

	runner_assert(ctl->get_surfaces_on_layer(
		      ivilayer, &length, &array) == IVI_SUCCEEDED);
	runner_assert_or_return(2 == length);
	for (i = 0; i < 2; i++) {
		runner_assert(array[i] == ivisurfs[i]);
	}

	if (length > 0) {
		free(array);
	}

	ctl->layer_remove(ivilayer);
}

RUNNER_TEST(layer_bad_render_order)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_layer *ivilayer;
	struct ivi_layout_surface *ivisurfs[IVI_TEST_SURFACE_COUNT] = {};
	struct ivi_layout_surface **array = NULL;
	int32_t length = 0;
	uint32_t i;

	ivilayer = ctl->layer_create_with_dimension(IVI_TEST_LAYER_ID(0), 200, 300);

	for (i = 0; i < IVI_TEST_SURFACE_COUNT; i++) {
		ivisurfs[i] = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(i));
	}

	runner_assert(ctl->layer_set_render_order(
		      NULL, ivisurfs, IVI_TEST_SURFACE_COUNT) == IVI_FAILED);

	ctl->commit_changes();

	runner_assert(ctl->get_surfaces_on_layer(
		      NULL, &length, &array) == IVI_FAILED);
	runner_assert(length == 0 && array == NULL);

	runner_assert(ctl->get_surfaces_on_layer(
		      ivilayer, NULL, &array) == IVI_FAILED);
	runner_assert(array == NULL);

	runner_assert(ctl->get_surfaces_on_layer(
		      ivilayer, &length, NULL) == IVI_FAILED);
	runner_assert(length == 0);

	ctl->layer_remove(ivilayer);
}

RUNNER_TEST(commit_changes_after_render_order_set_surface_destroy)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_layer *ivilayer;
	struct ivi_layout_surface *ivisurfs[IVI_TEST_SURFACE_COUNT] = {};
	int i;

	ivilayer = ctl->layer_create_with_dimension(IVI_TEST_LAYER_ID(0), 200, 300);

	for (i = 0; i < IVI_TEST_SURFACE_COUNT; i++) {
		ivisurfs[i] = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(i));
	}

	runner_assert(ctl->layer_set_render_order(
		      ivilayer, ivisurfs, IVI_TEST_SURFACE_COUNT) == IVI_SUCCEEDED);
}

RUNNER_TEST(cleanup_layer)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_layer *ivilayer;

	ivilayer = ctl->get_layer_from_id(IVI_TEST_LAYER_ID(0));
	ctl->layer_remove(ivilayer);
}

static void
test_surface_add_notification_callback(struct ivi_layout_surface *ivisurf,
				       const struct ivi_layout_surface_properties *prop,
				       enum ivi_layout_notification_mask mask,
				       void *userdata)
{
	struct test_context *ctx = userdata;
	const struct ivi_controller_interface *ctl = ctx->controller_interface;

	runner_assert_or_return(ctl->get_id_of_surface(ivisurf) == IVI_TEST_SURFACE_ID(0));

	ctx->user_flags = 1;
}

RUNNER_TEST(surface_add_notification)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	const uint32_t id_surface = IVI_TEST_SURFACE_ID(0);
	struct ivi_layout_surface *ivisurf;

	ctx->user_flags = 0;

	ivisurf = ctl->get_surface_from_id(id_surface);
	runner_assert(ivisurf != NULL);

	runner_assert(ctl->surface_add_notification(
		      ivisurf, test_surface_add_notification_callback, ctx) == IVI_SUCCEEDED);

	ctl->commit_changes();

	runner_assert(ctx->user_flags == 1);

	ctl->surface_remove_notification(ivisurf);
	ctl->commit_changes();
}

static void
test_surface_configure_notification_callback(struct ivi_layout_surface *ivisurf,
					     void *userdata)
{
	struct test_context *ctx = userdata;
	const struct ivi_controller_interface *ctl = ctx->controller_interface;

	runner_assert_or_return(ctl->get_id_of_surface(ivisurf) == IVI_TEST_SURFACE_ID(0));

	ctx->user_flags = 1;
}

RUNNER_TEST(surface_configure_notification_p1)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;

	runner_assert(IVI_SUCCEEDED == ctl->add_notification_configure_surface(test_surface_configure_notification_callback, ctx));
	ctl->commit_changes();

	ctx->user_flags = 0;
}

RUNNER_TEST(surface_configure_notification_p2)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;

	runner_assert(ctx->user_flags == 1);

	ctl->remove_notification_configure_surface(test_surface_configure_notification_callback, ctx);
	ctl->commit_changes();
}

static void
test_surface_create_notification_callback(struct ivi_layout_surface *ivisurf,
					  void *userdata)
{
	struct test_context *ctx = userdata;
	const struct ivi_controller_interface *ctl = ctx->controller_interface;

	runner_assert_or_return(ctl->get_id_of_surface(ivisurf) == IVI_TEST_SURFACE_ID(0));

	ctx->user_flags = 1;
}

RUNNER_TEST(surface_create_notification_p1)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;

	runner_assert(ctl->add_notification_create_surface(
		      test_surface_create_notification_callback, ctx) == IVI_SUCCEEDED);

	ctx->user_flags = 0;
}

RUNNER_TEST(surface_create_notification_p2)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;

	runner_assert(ctx->user_flags == 1);

	ctl->remove_notification_create_surface(
		test_surface_create_notification_callback, ctx);
}

static void
test_surface_remove_notification_callback(struct ivi_layout_surface *ivisurf,
					  void *userdata)
{
	struct test_context *ctx = userdata;
	const struct ivi_controller_interface *ctl = ctx->controller_interface;

	runner_assert_or_return(ctl->get_id_of_surface(ivisurf) == IVI_TEST_SURFACE_ID(0));

	ctx->user_flags = 1;
}

RUNNER_TEST(surface_remove_notification_p1)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;

	runner_assert(ctl->add_notification_remove_surface(
		      test_surface_remove_notification_callback, ctx) == IVI_SUCCEEDED);

	ctx->user_flags = 0;
}

RUNNER_TEST(surface_remove_notification_p2)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;

	runner_assert(ctx->user_flags == 1);

	ctl->remove_notification_remove_surface(test_surface_remove_notification_callback, ctx);
}

static void
test_surface_bad_add_notification_callback(struct ivi_layout_surface *ivisurf,
	const struct ivi_layout_surface_properties *prop,
	enum ivi_layout_notification_mask mask,
	void *userdata)
{
}

RUNNER_TEST(surface_bad_add_notification)
{
	const struct ivi_controller_interface *ctl = ctx->controller_interface;
	struct ivi_layout_surface *ivisurf;
	ivisurf = ctl->get_surface_from_id(IVI_TEST_SURFACE_ID(0));
	runner_assert_or_return(ivisurf != NULL);

	runner_assert_or_return(ctl->surface_add_notification(
		    NULL, test_surface_bad_add_notification_callback, NULL) == IVI_FAILED);
	runner_assert_or_return(ctl->surface_add_notification(
		    ivisurf, NULL, NULL) == IVI_FAILED);
}

