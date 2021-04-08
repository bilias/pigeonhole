/* Copyright (c) 2002-2018 Pigeonhole authors, see the included COPYING file
 */

#include "lib.h"
#include "str.h"
#include "istream.h"
#include "ostream.h"
#include "buffer.h"
#include "time-util.h"
#include "eacces-error.h"
#include "home-expand.h"
#include "hostpid.h"
#include "message-address.h"
#include "mail-user.h"

#include "sieve-settings.h"
#include "sieve-extensions.h"
#include "sieve-plugins.h"

#include "sieve-address.h"
#include "sieve-script.h"
#include "sieve-storage-private.h"
#include "sieve-ast.h"
#include "sieve-binary.h"
#include "sieve-actions.h"
#include "sieve-result.h"

#include "sieve-parser.h"
#include "sieve-validator.h"
#include "sieve-generator.h"
#include "sieve-interpreter.h"
#include "sieve-binary-dumper.h"

#include "sieve.h"
#include "sieve-common.h"
#include "sieve-limits.h"
#include "sieve-error-private.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <dirent.h>

struct event_category event_category_sieve = {
	.name = "sieve",
};

/*
 * Main Sieve library interface
 */

struct sieve_instance *
sieve_init(const struct sieve_environment *env,
	   const struct sieve_callbacks *callbacks, void *context, bool debug)
{
	struct sieve_instance *svinst;
	const char *domain;
	pool_t pool;

	/* Create Sieve engine instance */
	pool = pool_alloconly_create("sieve", 8192);
	svinst = p_new(pool, struct sieve_instance, 1);
	svinst->pool = pool;
	svinst->callbacks = callbacks;
	svinst->context = context;
	svinst->debug = debug;
	svinst->base_dir = p_strdup_empty(pool, env->base_dir);
	svinst->username = p_strdup_empty(pool, env->username);
	svinst->home_dir = p_strdup_empty(pool, env->home_dir);
	svinst->temp_dir = p_strdup_empty(pool, env->temp_dir);
	svinst->flags = env->flags;
	svinst->env_location = env->location;
	svinst->delivery_phase = env->delivery_phase;

	svinst->event = event_create(env->event_parent);
	event_add_category(svinst->event, &event_category_sieve);
	event_set_forced_debug(svinst->event, debug);
	event_set_append_log_prefix(svinst->event, "sieve: ");
	event_add_str(svinst->event, "user", env->username);

	/* Determine domain */
	if (env->domainname != NULL && *(env->domainname) != '\0')
		domain = env->domainname;
	else {
		/* Fall back to parsing username localpart@domain */
		domain = svinst->username == NULL ? NULL :
			strchr(svinst->username, '@');
		if (domain == NULL || *(domain+1) == '\0') {
			/* Fall back to parsing hostname host.domain */
			domain = (env->hostname != NULL ?
				  strchr(env->hostname, '.') : NULL);
			if (domain == NULL || *(domain+1) == '\0' ||
			    strchr(domain+1, '.') == NULL) {
				/* Fall back to bare hostname */
				domain = env->hostname;
			} else {
				domain++;
			}
		} else {
			domain++;
		}
	}
	svinst->hostname = p_strdup_empty(pool, env->hostname);
	svinst->domainname = p_strdup(pool, domain);

	sieve_errors_init(svinst);

	e_debug(svinst->event, "%s version %s initializing",
		PIGEONHOLE_NAME, PIGEONHOLE_VERSION_FULL);

	/* Read configuration */

	sieve_settings_load(svinst);

	/* Initialize extensions */
	if (!sieve_extensions_init(svinst)) {
		sieve_deinit(&svinst);
		return NULL;
	}

	/* Initialize storage classes */
	sieve_storages_init(svinst);

	/* Initialize plugins */
	sieve_plugins_load(svinst, NULL, NULL);

	/* Configure extensions */
	sieve_extensions_configure(svinst);

	return svinst;
}

void sieve_deinit(struct sieve_instance **_svinst)
{
	struct sieve_instance *svinst = *_svinst;

	sieve_plugins_unload(svinst);
	sieve_storages_deinit(svinst);
	sieve_extensions_deinit(svinst);
	sieve_errors_deinit(svinst);

	event_unref(&svinst->event);

	pool_unref(&(svinst)->pool);
	*_svinst = NULL;
}

void sieve_set_extensions(struct sieve_instance *svinst, const char *extensions)
{
	sieve_extensions_set_string(svinst, extensions, FALSE, FALSE);
}

const char *
sieve_get_capabilities(struct sieve_instance *svinst, const char *name)
{
	if (name == NULL || *name == '\0')
		return sieve_extensions_get_string(svinst);

	return sieve_extension_capabilities_get_string(svinst, name);
}

struct event *sieve_get_event(struct sieve_instance *svinst)
{
	return svinst->event;
}

/*
 * Low-level compiler functions
 */

struct sieve_ast *
sieve_parse(struct sieve_script *script, struct sieve_error_handler *ehandler,
	    enum sieve_error *error_r)
{
	struct sieve_parser *parser;
	struct sieve_ast *ast = NULL;

	/* Parse */
	parser = sieve_parser_create(script, ehandler, error_r);
	if (parser == NULL)
		return NULL;

 	if (!sieve_parser_run(parser, &ast))
 		ast = NULL;
 	else
		sieve_ast_ref(ast);

	sieve_parser_free(&parser);

	if (error_r != NULL) {
		if (ast == NULL)
			*error_r = SIEVE_ERROR_NOT_VALID;
		else
			*error_r = SIEVE_ERROR_NONE;
	}
	return ast;
}

bool sieve_validate(struct sieve_ast *ast, struct sieve_error_handler *ehandler,
		    enum sieve_compile_flags flags, enum sieve_error *error_r)
{
	bool result = TRUE;
	struct sieve_validator *validator =
		sieve_validator_create(ast, ehandler, flags);

	if (!sieve_validator_run(validator))
		result = FALSE;

	sieve_validator_free(&validator);

	if (error_r != NULL) {
		if (!result)
			*error_r = SIEVE_ERROR_NOT_VALID;
		else
			*error_r = SIEVE_ERROR_NONE;
	}
	return result;
}

static struct sieve_binary *
sieve_generate(struct sieve_ast *ast, struct sieve_error_handler *ehandler,
	       enum sieve_compile_flags flags, enum sieve_error *error_r)
{
	struct sieve_generator *generator =
		sieve_generator_create(ast, ehandler, flags);
	struct sieve_binary *sbin = NULL;

	sbin = sieve_generator_run(generator, NULL);

	sieve_generator_free(&generator);

	if (error_r != NULL) {
		if (sbin == NULL)
			*error_r = SIEVE_ERROR_NOT_VALID;
		else
			*error_r = SIEVE_ERROR_NONE;
	}
	return sbin;
}

/*
 * Sieve compilation
 */

struct sieve_binary *
sieve_compile_script(struct sieve_script *script,
		     struct sieve_error_handler *ehandler,
		     enum sieve_compile_flags flags,
		     enum sieve_error *error_r)
{
	struct sieve_ast *ast;
	struct sieve_binary *sbin;
	enum sieve_error error, *errorp;

	if (error_r != NULL)
		errorp = error_r;
	else
		errorp = &error;
	*errorp = SIEVE_ERROR_NONE;

	/* Parse */
	ast = sieve_parse(script, ehandler, errorp);
	if (ast == NULL) {
		switch (*errorp) {
		case SIEVE_ERROR_NOT_FOUND:
			if (error_r == NULL) {
				sieve_error(ehandler, sieve_script_name(script),
					    "script not found");
			}
			break;
		default:
			sieve_error(ehandler, sieve_script_name(script),
				    "parse failed");
		}
		return NULL;
	}

	/* Validate */
	if (!sieve_validate(ast, ehandler, flags, errorp)) {
		sieve_error(ehandler, sieve_script_name(script),
			    "validation failed");

 		sieve_ast_unref(&ast);
 		return NULL;
 	}

	/* Generate */
	sbin = sieve_generate(ast, ehandler, flags, errorp);
	if (sbin == NULL) {
		sieve_error(ehandler, sieve_script_name(script),
			    "code generation failed");
		sieve_ast_unref(&ast);
		return NULL;
	}

	/* Cleanup */
	sieve_ast_unref(&ast);
	return sbin;
}

struct sieve_binary *
sieve_compile(struct sieve_instance *svinst, const char *script_location,
	      const char *script_name, struct sieve_error_handler *ehandler,
	      enum sieve_compile_flags flags, enum sieve_error *error_r)
{
	struct sieve_script *script;
	struct sieve_binary *sbin;
	enum sieve_error error;

	script = sieve_script_create_open(svinst, script_location,
					  script_name, &error);
	if (script == NULL) {
		if (error_r != NULL)
			*error_r = error;
		switch (error) {
		case SIEVE_ERROR_NOT_FOUND:
			sieve_error(ehandler, script_name, "script not found");
			break;
		default:
			sieve_internal_error(ehandler, script_name,
					     "failed to open script");
		}
		return NULL;
	}

	sbin = sieve_compile_script(script, ehandler, flags, error_r);
	if (sbin != NULL) {
		e_debug(svinst->event,
			"Script `%s' from %s successfully compiled",
			sieve_script_name(script),
			sieve_script_location(script));
	}

	sieve_script_unref(&script);
	return sbin;
}

/*
 * Sieve runtime
 */

static int
sieve_run(struct sieve_binary *sbin, struct sieve_result *result,
	  struct sieve_execute_env *eenv, struct sieve_error_handler *ehandler)
{
	struct sieve_interpreter *interp;
	int ret = 0;

	/* Create the interpreter */
	interp = sieve_interpreter_create(sbin, NULL, eenv, ehandler);
	if (interp == NULL)
		return SIEVE_EXEC_BIN_CORRUPT;

	/* Run the interpreter */
	ret = sieve_interpreter_run(interp, result);

	/* Free the interpreter */
	sieve_interpreter_free(&interp);

	return ret;
}

/*
 * Reading/writing sieve binaries
 */

struct sieve_binary *
sieve_load(struct sieve_instance *svinst, const char *bin_path,
	   enum sieve_error *error_r)
{
	return sieve_binary_open(svinst, bin_path, NULL, error_r);
}

static struct sieve_binary *
sieve_open_script_real(struct sieve_script *script,
		       struct sieve_error_handler *ehandler,
		       enum sieve_compile_flags flags,
		       enum sieve_error *error_r)
{
	struct sieve_instance *svinst = sieve_script_svinst(script);
	struct sieve_resource_usage rusage;
	struct sieve_binary *sbin;
	enum sieve_error error;
	const char *errorstr = NULL;
	int ret;

	if (error_r == NULL)
		error_r = &error;

	sieve_resource_usage_init(&rusage);

	/* Try to open the matching binary */
	sbin = sieve_script_binary_load(script, error_r);
	if (sbin != NULL) {
		sieve_binary_get_resource_usage(sbin, &rusage);

		/* Ok, it exists; now let's see if it is up to date */
		if (!sieve_resource_usage_is_excessive(svinst, &rusage) &&
		    !sieve_binary_up_to_date(sbin, flags)) {
			/* Not up to date */
			e_debug(svinst->event,
				"Script binary %s is not up-to-date",
				sieve_binary_path(sbin));
			sieve_binary_close(&sbin);
		}
	}

	/* If the binary does not exist or is not up-to-date, we need
	 * to (re-)compile.
	 */
	if (sbin != NULL) {
		e_debug(svinst->event,
			"Script binary %s successfully loaded",
			sieve_binary_path(sbin));
	} else {
		sbin = sieve_compile_script(script, ehandler, flags, error_r);
		if (sbin == NULL)
			return NULL;

		e_debug(svinst->event,
			"Script `%s' from %s successfully compiled",
			sieve_script_name(script),
			sieve_script_location(script));

		sieve_binary_set_resource_usage(sbin, &rusage);
	}

	/* Check whether binary can be executed. */
	ret = sieve_binary_check_executable(sbin, error_r, &errorstr);
	if (ret <= 0) {
		const char *path = sieve_binary_path(sbin);

		if (path != NULL) {
			e_debug(svinst->event,
				"Script binary %s cannot be executed",
				path);
		} else {
			e_debug(svinst->event,
				"Script binary from %s cannot be executed",
				sieve_binary_source(sbin));
		}
		if (ret < 0) {
			sieve_internal_error(ehandler,
					     sieve_script_name(script),
					     "failed to open script");
		} else {
			sieve_error(ehandler, sieve_script_name(script),
				    "%s", errorstr);
		}
		sieve_binary_close(&sbin);
	}

	return sbin;
}

struct sieve_binary *
sieve_open_script(struct sieve_script *script,
		  struct sieve_error_handler *ehandler,
		  enum sieve_compile_flags flags, enum sieve_error *error_r)
{
	struct sieve_binary *sbin;

	T_BEGIN {
		sbin = sieve_open_script_real(script, ehandler, flags, error_r);
	} T_END;

	return sbin;
}

struct sieve_binary *
sieve_open(struct sieve_instance *svinst, const char *script_location,
	   const char *script_name, struct sieve_error_handler *ehandler,
	   enum sieve_compile_flags flags, enum sieve_error *error_r)
{
	struct sieve_script *script;
	struct sieve_binary *sbin;
	enum sieve_error error;

	/* First open the scriptfile itself */
	script = sieve_script_create_open(svinst, script_location,
					  script_name, &error);
	if (script == NULL) {
		/* Failed */
		if (error_r != NULL)
			*error_r = error;
		switch (error) {
		case SIEVE_ERROR_NOT_FOUND:
			sieve_error(ehandler, script_name, "script not found");
			break;
		default:
			sieve_internal_error(ehandler, script_name,
					     "failed to open script");
		}
		return NULL;
	}

	sbin = sieve_open_script(script, ehandler, flags, error_r);

	/* Drop script reference, if sbin != NULL it holds a reference of its own.
	 * Otherwise the script object is freed here.
	 */
	sieve_script_unref(&script);

	return sbin;
}

const char *sieve_get_source(struct sieve_binary *sbin)
{
	return sieve_binary_source(sbin);
}

bool sieve_is_loaded(struct sieve_binary *sbin)
{
	return sieve_binary_loaded(sbin);
}

int sieve_save_as(struct sieve_binary *sbin, const char *bin_path, bool update,
		  mode_t save_mode, enum sieve_error *error_r)
{
	if (bin_path == NULL)
		return sieve_save(sbin, update, error_r);

	return sieve_binary_save(sbin, bin_path, update, save_mode, error_r);
}

int sieve_save(struct sieve_binary *sbin, bool update,
	       enum sieve_error *error_r)
{
	struct sieve_script *script = sieve_binary_script(sbin);

	if (script == NULL)
		return sieve_binary_save(sbin, NULL, update, 0600, error_r);

	return sieve_script_binary_save(script, sbin, update, error_r);
}

bool sieve_record_resource_usage(struct sieve_binary *sbin,
				 const struct sieve_resource_usage *rusage)
{
	return sieve_binary_record_resource_usage(sbin, rusage);
}

int sieve_check_executable(struct sieve_binary *sbin,
			   enum sieve_error *error_r,
			   const char **client_error_r)
{
	return sieve_binary_check_executable(sbin, error_r, client_error_r);
}

void sieve_close(struct sieve_binary **_sbin)
{
	sieve_binary_close(_sbin);
}

/*
 * Debugging
 */

void sieve_dump(struct sieve_binary *sbin, struct ostream *stream, bool verbose)
{
	struct sieve_binary_dumper *dumpr = sieve_binary_dumper_create(sbin);

	sieve_binary_dumper_run(dumpr, stream, verbose);

	sieve_binary_dumper_free(&dumpr);
}

void sieve_hexdump(struct sieve_binary *sbin, struct ostream *stream)
{
	struct sieve_binary_dumper *dumpr = sieve_binary_dumper_create(sbin);

	sieve_binary_dumper_hexdump(dumpr, stream);

	sieve_binary_dumper_free(&dumpr);
}

int sieve_test(struct sieve_binary *sbin,
	       const struct sieve_message_data *msgdata,
	       const struct sieve_script_env *senv,
	       struct sieve_error_handler *ehandler, struct ostream *stream,
	       enum sieve_execute_flags flags)
{
	struct sieve_instance *svinst = sieve_binary_svinst(sbin);
	struct sieve_result *result;
	struct sieve_execute_env eenv;
	pool_t pool;
	int ret;

	pool = pool_alloconly_create("sieve execution", 4096);
	sieve_execute_init(&eenv, svinst, pool, msgdata, senv, flags);

	/* Create result object */
	result = sieve_result_create(svinst, pool, &eenv);

	/* Run the script */
	ret = sieve_run(sbin, result, &eenv, ehandler);

	/* Print result if successful */
	if (ret > 0) {
		ret = (sieve_result_print(result, senv, stream, NULL) ?
		       SIEVE_EXEC_OK : SIEVE_EXEC_FAILURE);
	}

	/* Cleanup */
	if (result != NULL)
		sieve_result_unref(&result);
	sieve_execute_deinit(&eenv);
	pool_unref(&pool);

	return ret;
}

/*
 * Script execution
 */

int sieve_script_env_init(struct sieve_script_env *senv, struct mail_user *user,
			  const char **error_r)
{
	const struct message_address *postmaster;
	const char *error;

	if (!mail_user_get_postmaster_address(user, &postmaster, &error)) {
		*error_r = t_strdup_printf(
			"Invalid postmaster_address: %s", error);
		return -1;
	}

	i_zero(senv);
	senv->user = user;
	senv->postmaster_address = postmaster;
	return 0;
}

int sieve_execute(struct sieve_binary *sbin,
		  const struct sieve_message_data *msgdata,
		  const struct sieve_script_env *senv,
		  struct sieve_error_handler *exec_ehandler,
		  struct sieve_error_handler *action_ehandler,
		  enum sieve_execute_flags flags)
{
	struct sieve_instance *svinst = sieve_binary_svinst(sbin);
	struct sieve_result *result = NULL;
	struct sieve_execute_env eenv;
	pool_t pool;
	int ret;

	pool = pool_alloconly_create("sieve execution", 4096);
	sieve_execute_init(&eenv, svinst, pool, msgdata, senv, flags);

	/* Create result object */
	result = sieve_result_create(svinst, pool, &eenv);

	/* Run the script */
	ret = sieve_run(sbin, result, &eenv, exec_ehandler);

	/* Evaluate status and execute the result:
	   Strange situations, e.g. currupt binaries, must be handled by the
	   caller. In that case no implicit keep is attempted, because the
	   situation may be resolved.
	 */
	if (ret > 0) {
		/* Execute result */
		ret = sieve_result_execute(result, TRUE, NULL, action_ehandler);
	} else if (ret == SIEVE_EXEC_FAILURE) {
		/* Perform implicit keep if script failed with a normal runtime
		   error
		 */
		switch (sieve_result_implicit_keep(result, action_ehandler,
						   FALSE)) {
		case SIEVE_EXEC_OK:
			break;
		case SIEVE_EXEC_TEMP_FAILURE:
			ret = SIEVE_EXEC_TEMP_FAILURE;
			break;
		default:
			ret = SIEVE_EXEC_KEEP_FAILED;
		}
	}

	/* Cleanup */
	if (result != NULL)
		sieve_result_unref(&result);
	sieve_execute_deinit(&eenv);
	pool_unref(&pool);

	return ret;
}

/*
 * Multiscript support
 */

struct sieve_multiscript {
	pool_t pool;
	struct sieve_execute_env exec_env;
	struct sieve_result *result;

	int status;
	bool keep;

	struct ostream *teststream;

	bool active:1;
	bool discard_handled:1;
};

struct sieve_multiscript *
sieve_multiscript_start_execute(struct sieve_instance *svinst,
				const struct sieve_message_data *msgdata,
				const struct sieve_script_env *senv)
{
	pool_t pool;
	struct sieve_result *result;
	struct sieve_multiscript *mscript;

	pool = pool_alloconly_create("sieve execution", 4096);
	mscript = p_new(pool, struct sieve_multiscript, 1);
	mscript->pool = pool;
	sieve_execute_init(&mscript->exec_env, svinst, pool, msgdata, senv, 0);

	result = sieve_result_create(svinst, pool, &mscript->exec_env);
	sieve_result_set_keep_action(result, NULL, NULL);
	mscript->result = result;

	mscript->status = SIEVE_EXEC_OK;
	mscript->active = TRUE;
	mscript->keep = TRUE;

	return mscript;
}

static void sieve_multiscript_destroy(struct sieve_multiscript **_mscript)
{
	struct sieve_multiscript *mscript = *_mscript;

	if (mscript == NULL)
		return;
	*_mscript = NULL;

	sieve_result_unref(&mscript->result);
	sieve_execute_deinit(&mscript->exec_env);
	pool_unref(&mscript->pool);
}

struct sieve_multiscript *
sieve_multiscript_start_test(struct sieve_instance *svinst,
			     const struct sieve_message_data *msgdata,
			     const struct sieve_script_env *senv,
			     struct ostream *stream)
{
	struct sieve_multiscript *mscript =
		sieve_multiscript_start_execute(svinst, msgdata, senv);

	mscript->teststream = stream;

	return mscript;
}

static void
sieve_multiscript_test(struct sieve_multiscript *mscript)
{
	const struct sieve_script_env *senv = mscript->exec_env.scriptenv;

	if (mscript->status > 0) {
		mscript->status =
			(sieve_result_print(mscript->result, senv,
					    mscript->teststream,
					    &mscript->keep) ?
			 SIEVE_EXEC_OK : SIEVE_EXEC_FAILURE);
	} else {
		mscript->keep = TRUE;
	}

	sieve_result_mark_executed(mscript->result);
}

static void
sieve_multiscript_execute(struct sieve_multiscript *mscript,
			  struct sieve_error_handler *ehandler,
			  enum sieve_execute_flags flags)
{
	mscript->exec_env.flags = flags;

	if (mscript->status > 0) {
		mscript->status = sieve_result_execute(mscript->result, FALSE,
						       &mscript->keep,
						       ehandler);
	} else {
		if (sieve_result_implicit_keep(mscript->result, ehandler,
					       FALSE) <= 0)
			mscript->status = SIEVE_EXEC_KEEP_FAILED;
		else
			mscript->keep = TRUE;
	}
}

bool sieve_multiscript_run(struct sieve_multiscript *mscript,
			   struct sieve_binary *sbin,
			   struct sieve_error_handler *exec_ehandler,
			   struct sieve_error_handler *action_ehandler,
			   enum sieve_execute_flags flags)
{
	if (!mscript->active)
		return FALSE;

	/* Run the script */
	mscript->exec_env.flags = flags;
	mscript->status = sieve_run(sbin, mscript->result, &mscript->exec_env,
				    exec_ehandler);

	if (mscript->status >= 0) {
		mscript->keep = FALSE;

		if (mscript->teststream != NULL)
			sieve_multiscript_test(mscript);
		else {
			sieve_multiscript_execute(mscript, action_ehandler,
						  flags);
		}
		if (!mscript->keep)
			mscript->active = FALSE;
	}

	if (!mscript->active || mscript->status <= 0) {
		mscript->active = FALSE;
		return FALSE;
	}

	return TRUE;
}

bool sieve_multiscript_will_discard(struct sieve_multiscript *mscript)
{
	return (!mscript->active && mscript->status == SIEVE_EXEC_OK &&
		!sieve_result_executed_delivery(mscript->result));
}

void sieve_multiscript_run_discard(struct sieve_multiscript *mscript,
				   struct sieve_binary *sbin,
				   struct sieve_error_handler *exec_ehandler,
				   struct sieve_error_handler *action_ehandler,
				   enum sieve_execute_flags flags)
{
	if (!sieve_multiscript_will_discard(mscript))
		return;
	i_assert(!mscript->discard_handled);

	sieve_result_set_keep_action(mscript->result, NULL, &act_store);

	/* Run the discard script */
	flags |= SIEVE_EXECUTE_FLAG_DEFER_KEEP;
	mscript->exec_env.flags = flags;
	mscript->status = sieve_run(sbin, mscript->result, &mscript->exec_env,
				    exec_ehandler);

	if (mscript->status >= 0) {
		mscript->keep = FALSE;

		if (mscript->teststream != NULL)
			sieve_multiscript_test(mscript);
		else {
			sieve_multiscript_execute(mscript, action_ehandler,
						  flags);
		}
		if (mscript->status == SIEVE_EXEC_FAILURE)
			mscript->status = SIEVE_EXEC_KEEP_FAILED;
		mscript->active = FALSE;
	}

	mscript->discard_handled = TRUE;
}

int sieve_multiscript_status(struct sieve_multiscript *mscript)
{
	return mscript->status;
}

int sieve_multiscript_tempfail(struct sieve_multiscript **_mscript,
			       struct sieve_error_handler *action_ehandler,
			       enum sieve_execute_flags flags)
{
	struct sieve_multiscript *mscript = *_mscript;
	struct sieve_result *result = mscript->result;
	int ret = mscript->status;

	mscript->exec_env.flags = flags;
	sieve_result_set_keep_action(mscript->result, NULL, &act_store);

	if (mscript->active) {
		ret = SIEVE_EXEC_TEMP_FAILURE;

		if (mscript->teststream == NULL &&
		    sieve_result_executed(result)) {
			/* Part of the result is already executed, need to fall
			   back to to implicit keep (FIXME)
			 */
			switch (sieve_result_implicit_keep(
				result, action_ehandler, FALSE)) {
			case SIEVE_EXEC_OK:
				ret = SIEVE_EXEC_FAILURE;
				break;
			default:
				ret = SIEVE_EXEC_KEEP_FAILED;
			}
		}
	}

	/* Cleanup */
	sieve_multiscript_destroy(&mscript);

	return ret;
}

int sieve_multiscript_finish(struct sieve_multiscript **_mscript,
			     struct sieve_error_handler *action_ehandler,
			     enum sieve_execute_flags flags)
{
	struct sieve_multiscript *mscript = *_mscript;
	struct sieve_result *result = mscript->result;
	int ret = mscript->status;

	mscript->exec_env.flags = flags;
	sieve_result_set_keep_action(mscript->result, NULL, &act_store);

	if (mscript->active) {
		if (mscript->teststream != NULL)
			mscript->keep = TRUE;
		else {
			switch (sieve_result_implicit_keep(
				result, action_ehandler, TRUE)) {
			case SIEVE_EXEC_OK:
				mscript->keep = TRUE;
				break;
			case SIEVE_EXEC_TEMP_FAILURE:
				if (!sieve_result_executed(result)) {
					ret = SIEVE_EXEC_TEMP_FAILURE;
					break;
				}
				/* fall through */
			default:
				ret = SIEVE_EXEC_KEEP_FAILED;
			}
		}
	}

	sieve_result_finish(result, action_ehandler, (ret == SIEVE_EXEC_OK));

	/* Cleanup */
	sieve_multiscript_destroy(&mscript);

	return ret;
}

/*
 * Configured Limits
 */

unsigned int sieve_max_redirects(struct sieve_instance *svinst)
{
	return svinst->max_redirects;
}

unsigned int sieve_max_actions(struct sieve_instance *svinst)
{
	return svinst->max_actions;
}

size_t sieve_max_script_size(struct sieve_instance *svinst)
{
	return svinst->max_script_size;
}

/*
 * User log
 */

const char *
sieve_user_get_log_path(struct sieve_instance *svinst,
			struct sieve_script *user_script)
{
	const char *log_path = NULL;

	/* Determine user log file path */
	log_path = sieve_setting_get(svinst, "sieve_user_log");
	if (log_path == NULL) {
		const char *path;

		if (user_script == NULL ||
		    (path = sieve_file_script_get_path(user_script)) == NULL) {
			/* Default */
			if (svinst->home_dir != NULL) {
				log_path = t_strconcat(
					svinst->home_dir, "/.dovecot.sieve.log",
					NULL);
			}
		} else {
			/* Use script file as a base (legacy behavior) */
			log_path = t_strconcat(path, ".log", NULL);
		}
	} else if (svinst->home_dir != NULL) {
		/* Expand home dir if necessary */
		if (log_path[0] == '~') {
			log_path = home_expand_tilde(log_path,
						     svinst->home_dir);
		} else if (log_path[0] != '/') {
			log_path = t_strconcat(svinst->home_dir, "/",
					       log_path, NULL);
		}
	}
	return log_path;
}

/*
 * Script trace log
 */

struct sieve_trace_log {
	struct ostream *output;
};

int sieve_trace_log_create(struct sieve_instance *svinst, const char *path,
			   struct sieve_trace_log **trace_log_r)
{
	struct sieve_trace_log *trace_log;
	struct ostream *output;
	int fd;

	*trace_log_r = NULL;

	if (path == NULL)
		output = o_stream_create_fd(1, 0);
	else {
		fd = open(path, O_CREAT | O_APPEND | O_WRONLY, 0600);
		if (fd == -1) {
			e_error(svinst->event, "trace: "
				"creat(%s) failed: %m", path);
			return -1;
		}
		output = o_stream_create_fd_autoclose(&fd, 0);
		o_stream_set_name(output, path);
	}

	trace_log = i_new(struct sieve_trace_log, 1);
	trace_log->output = output;

	*trace_log_r = trace_log;
	return 0;
}

int sieve_trace_log_create_dir(struct sieve_instance *svinst, const char *dir,
			       struct sieve_trace_log **trace_log_r)
{
	static unsigned int counter = 0;
	const char *timestamp, *prefix;
	struct stat st;

	*trace_log_r = NULL;

	if (stat(dir, &st) < 0) {
		if (errno != ENOENT && errno != EACCES) {
			e_error(svinst->event, "trace: "
				"stat(%s) failed: %m", dir);
		}
		return -1;
	}

	timestamp = t_strflocaltime("%Y%m%d-%H%M%S", ioloop_time);

	counter++;

	prefix = t_strdup_printf("%s/%s.%s.%u.trace",
				 dir, timestamp, my_pid, counter);
	return sieve_trace_log_create(svinst, prefix, trace_log_r);
}

int sieve_trace_log_open(struct sieve_instance *svinst,
			 struct sieve_trace_log **trace_log_r)
{
	const char *trace_dir =
		sieve_setting_get(svinst, "sieve_trace_dir");

	*trace_log_r = NULL;
	if (trace_dir == NULL)
		return -1;

	if (svinst->home_dir != NULL) {
		/* Expand home dir if necessary */
		if (trace_dir[0] == '~') {
			trace_dir = home_expand_tilde(trace_dir,
						      svinst->home_dir);
		} else if (trace_dir[0] != '/') {
			trace_dir = t_strconcat(svinst->home_dir, "/",
						trace_dir, NULL);
		}
	}

	return sieve_trace_log_create_dir(svinst, trace_dir, trace_log_r);
}

void sieve_trace_log_write_line(struct sieve_trace_log *trace_log,
				const string_t *line)
{
	struct const_iovec iov[2];

	if (line == NULL) {
		o_stream_nsend_str(trace_log->output, "\n");
		return;
	}

	memset(iov, 0, sizeof(iov));
	iov[0].iov_base = str_data(line);
	iov[0].iov_len = str_len(line);
	iov[1].iov_base = "\n";
	iov[1].iov_len = 1;
	o_stream_nsendv(trace_log->output, iov, 2);
}

void sieve_trace_log_printf(struct sieve_trace_log *trace_log,
			    const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	T_BEGIN {
		o_stream_nsend_str(trace_log->output,
				   t_strdup_vprintf(fmt, args));
	} T_END;
	va_end(args);
}

void sieve_trace_log_free(struct sieve_trace_log **_trace_log)
{
	struct sieve_trace_log *trace_log = *_trace_log;

	*_trace_log = NULL;

	if (o_stream_finish(trace_log->output) < 0) {
		i_error("write(%s) failed: %s",
			o_stream_get_name(trace_log->output),
			o_stream_get_error(trace_log->output));
	}
	o_stream_destroy(&trace_log->output);
	i_free(trace_log);
}

int sieve_trace_config_get(struct sieve_instance *svinst,
			   struct sieve_trace_config *tr_config)
{
	const char *tr_level =
		sieve_setting_get(svinst, "sieve_trace_level");
	bool tr_debug, tr_addresses;

	i_zero(tr_config);

	if (tr_level == NULL || *tr_level == '\0' ||
	    strcasecmp(tr_level, "none") == 0)
		return -1;

	if (strcasecmp(tr_level, "actions") == 0)
		tr_config->level = SIEVE_TRLVL_ACTIONS;
	else if (strcasecmp(tr_level, "commands") == 0)
		tr_config->level = SIEVE_TRLVL_COMMANDS;
	else if (strcasecmp(tr_level, "tests") == 0)
		tr_config->level = SIEVE_TRLVL_TESTS;
	else if (strcasecmp(tr_level, "matching") == 0)
		tr_config->level = SIEVE_TRLVL_MATCHING;
	else {
		e_error(svinst->event, "Unknown trace level: %s", tr_level);
		return -1;
	}

	tr_debug = FALSE;
	(void)sieve_setting_get_bool_value(svinst, "sieve_trace_debug",
					   &tr_debug);
	tr_addresses = FALSE;
	(void)sieve_setting_get_bool_value(svinst, "sieve_trace_addresses",
					   &tr_addresses);

	if (tr_debug)
		tr_config->flags |= SIEVE_TRFLG_DEBUG;
	if (tr_addresses)
		tr_config->flags |= SIEVE_TRFLG_ADDRESSES;
	return 0;
}

/*
 * User e-mail address
 */

const struct smtp_address *sieve_get_user_email(struct sieve_instance *svinst)
{
	struct smtp_address *address;
	const char *username = svinst->username;

	if (svinst->user_email_implicit != NULL)
		return svinst->user_email_implicit;
	if (svinst->user_email != NULL)
		return svinst->user_email;

	if (smtp_address_parse_mailbox(svinst->pool, username, 0,
				       &address, NULL) >= 0) {
		svinst->user_email_implicit = address;
		return svinst->user_email_implicit;
	}

	if (svinst->domainname != NULL) {
		svinst->user_email_implicit = smtp_address_create(
			svinst->pool, username, svinst->domainname);
		return svinst->user_email_implicit;
	}
	return NULL;
}

/*
 * Postmaster address
 */

const struct message_address *
sieve_get_postmaster(const struct sieve_script_env *senv)
{
	i_assert(senv->postmaster_address != NULL);
	return senv->postmaster_address;
}

const struct smtp_address *
sieve_get_postmaster_smtp(const struct sieve_script_env *senv)
{
	struct smtp_address *addr;
	int ret;

	ret = smtp_address_create_from_msg_temp(
		sieve_get_postmaster(senv), &addr);
	i_assert(ret >= 0);
	return addr;
}

const char *sieve_get_postmaster_address(const struct sieve_script_env *senv)
{
	const struct message_address *postmaster =
		sieve_get_postmaster(senv);
	string_t *addr = t_str_new(256);

	message_address_write(addr, postmaster);
	return str_c(addr);
}

/*
 * Resource usage
 */

void sieve_resource_usage_init(struct sieve_resource_usage *rusage_r)
{
	i_zero(rusage_r);
}

void sieve_resource_usage_add(struct sieve_resource_usage *dst,
			      const struct sieve_resource_usage *src)
{
	if ((UINT_MAX - dst->cpu_time_msecs) < src->cpu_time_msecs)
		dst->cpu_time_msecs = UINT_MAX;
	else
		dst->cpu_time_msecs += src->cpu_time_msecs;
}

bool sieve_resource_usage_is_high(struct sieve_instance *svinst ATTR_UNUSED,
				  const struct sieve_resource_usage *rusage)
{
	return (rusage->cpu_time_msecs > SIEVE_HIGH_CPU_TIME_MSECS);
}

bool sieve_resource_usage_is_excessive(
	struct sieve_instance *svinst,
	const struct sieve_resource_usage *rusage)
{
	i_assert(svinst->max_cpu_time_secs <= (UINT_MAX / 1000));
	return (rusage->cpu_time_msecs > (svinst->max_cpu_time_secs * 1000));
}

const char *
sieve_resource_usage_get_summary(const struct sieve_resource_usage *rusage)
{
	if (rusage->cpu_time_msecs == 0)
		return "no usage recorded";

	return t_strdup_printf("cpu time = %u ms", rusage->cpu_time_msecs);
}
