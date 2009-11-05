/* Copyright (c) 2002-2009 Dovecot Sieve authors, see the included COPYING file
 */

/* FIXME: This file was gratefully stolen from dovecot/src/deliver/deliver.c and 
 * altered to suit our needs. So, this contains lots and lots of duplicated 
 * code. 
 */

#include "lib.h"
#include "istream.h"
#include "istream-seekable.h"
#include "fd-set-nonblock.h"
#include "str.h"
#include "str-sanitize.h"
#include "strescape.h"
#include "safe-mkstemp.h"
#include "close-keep-errno.h"
#include "mkdir-parents.h"
#include "message-address.h"
#include "mbox-from.h"
#include "raw-storage.h"
#include "mail-namespace.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "settings-parser.h"
#include "mail-raw.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>

/*
 * Configuration
 */

#define DEFAULT_ENVELOPE_SENDER "MAILER-DAEMON"

/* After buffer grows larger than this, create a temporary file to /tmp
   where to read the mail. */
#define MAIL_MAX_MEMORY_BUFFER (1024*128)

static const char *wanted_headers[] = {
	"From", "Message-ID", "Subject", "Return-Path",
	NULL
};

/*
 * Global data
 */

static struct mail_namespace *raw_ns;
static struct mail_namespace_settings raw_ns_set;
static struct mail_user *raw_mail_user;

char *raw_tmp_prefix;

/*
 * Raw mail implementation
 */

static int seekable_fd_callback
(const char **path_r, void *context ATTR_UNUSED)
{
	const char *dir, *p;
	string_t *path;
	int fd;

 	path = t_str_new(128);
 	str_append(path, "/tmp/dovecot.sieve-tool.");
	fd = safe_mkstemp(path, 0600, (uid_t)-1, (gid_t)-1);
	if (fd == -1 && errno == ENOENT) {
		dir = str_c(path);
		p = strrchr(dir, '/');
		if (p != NULL) {
			dir = t_strdup_until(dir, p);
			if ( mkdir_parents(dir, 0600) < 0 ) {
				i_error("mkdir_parents(%s) failed: %m", dir);
				return -1;
			}
			fd = safe_mkstemp(path, 0600, (uid_t)-1, (gid_t)-1);
		}
	}

	if (fd == -1) {
		i_error("safe_mkstemp(%s) failed: %m", str_c(path));
		return -1;
	}

	/* we just want the fd, unlink it */
	if (unlink(str_c(path)) < 0) {
		/* shouldn't happen.. */
		i_error("unlink(%s) failed: %m", str_c(path));
		close_keep_errno(fd);
		return -1;
	}

	*path_r = str_c(path);
	return fd;
}

static struct istream *create_raw_stream
(int fd, time_t *mtime_r, const char **sender)
{
	struct istream *input, *input2, *input_list[2];
	const unsigned char *data;
	size_t i, size;
	int ret, tz;
	char *env_sender = NULL;

	*mtime_r = (time_t)-1;
	fd_set_nonblock(fd, FALSE);

	input = i_stream_create_fd(fd, 4096, FALSE);
	input->blocking = TRUE;
	/* If input begins with a From-line, drop it */
	ret = i_stream_read_data(input, &data, &size, 5);
	if (ret > 0 && size >= 5 && memcmp(data, "From ", 5) == 0) {
		/* skip until the first LF */
		i_stream_skip(input, 5);
		while ((ret = i_stream_read_data(input, &data, &size, 0)) > 0) {
			for (i = 0; i < size; i++) {
				if (data[i] == '\n')
					break;
			}
			if (i != size) {
				(void)mbox_from_parse(data, i, mtime_r, &tz, &env_sender);
				i_stream_skip(input, i + 1);
				break;
			}
			i_stream_skip(input, size);
		}
	}

	if (env_sender != NULL && sender != NULL) {
		*sender = t_strdup(env_sender);
	}
	i_free(env_sender);

	if (input->v_offset == 0) {
		input2 = input;
		i_stream_ref(input2);
	} else {
		input2 = i_stream_create_limit(input, (uoff_t)-1);
	}
	i_stream_unref(&input);

	input_list[0] = input2; input_list[1] = NULL;
	input = i_stream_create_seekable(input_list, MAIL_MAX_MEMORY_BUFFER,
		seekable_fd_callback, raw_mail_user);
	i_stream_unref(&input2);
	return input;
}

/*
 * Init/Deinit
 */

void mail_raw_init
(struct master_service *service, const char *user,
	struct mail_user *mail_user) 
{
	const char *errstr;
	void **sets;
	
	sets = master_service_settings_get_others(service);

	raw_mail_user = mail_user_alloc(user, mail_user->set_info, sets[0]);
	mail_user_set_home(raw_mail_user, "/");
   
	if (mail_user_init(raw_mail_user, &errstr) < 0)
		i_fatal("Raw user initialization failed: %s", errstr);

	memset(&raw_ns_set, 0, sizeof(raw_ns_set));
	raw_ns_set.location = "/tmp";

	raw_ns = mail_namespaces_init_empty(raw_mail_user);
	raw_ns->flags |= NAMESPACE_FLAG_NOQUOTA | NAMESPACE_FLAG_NOACL;
	raw_ns->set = &raw_ns_set;
    
	if (mail_storage_create(raw_ns, "raw", 0, &errstr) < 0)
		i_fatal("Couldn't create internal raw storage: %s", errstr);

	raw_tmp_prefix = i_strdup(mail_user_get_temp_prefix(mail_user));
}

void mail_raw_deinit(void)
{
	i_free(raw_tmp_prefix);

	mail_user_unref(&raw_mail_user);
}

/*
 * Open raw mail data
 */

static struct mail_raw *mail_raw_create
(struct istream *input, const char *mailfile, const char *sender,
	time_t mtime)
{
	pool_t pool;
	struct raw_mailbox *raw_box;
	struct mail_raw *mailr;
	enum mail_error error;
	struct mailbox_header_lookup_ctx *headers_ctx;

	if ( mailfile != NULL ) {
		if ( *mailfile != '/') {
			char cwd[PATH_MAX];

			/* Expand relative paths */
			if (getcwd(cwd, sizeof(cwd)) == NULL)
				i_fatal("getcwd() failed: %m");

			mailfile = t_strconcat(cwd, "/", mailfile, NULL);		
		} 
	}

	pool = pool_alloconly_create("mail_raw", 1024);
	mailr = p_new(pool, struct mail_raw, 1);
	mailr->pool = pool;

	if ( mailfile == NULL ) {
		mailr->box = mailbox_alloc(raw_ns->list, "Dovecot Delivery Mail",
			input, MAILBOX_FLAG_NO_INDEX_FILES);
	} else {
		mtime = (time_t)-1;
		mailr->box = mailbox_alloc(raw_ns->list, mailfile, NULL,
			MAILBOX_FLAG_NO_INDEX_FILES);
	}

	if ( mailbox_open(mailr->box) < 0 ) {
        i_fatal("Can't open mail stream as raw: %s",
            mail_storage_get_last_error(raw_ns->storage, &error));
    }
    if ( mailbox_sync(mailr->box, 0, 0, NULL) < 0 ) {
        i_fatal("Can't sync delivery mail: %s",
            mail_storage_get_last_error(raw_ns->storage, &error));
    }

	raw_box = (struct raw_mailbox *)mailr->box;
	raw_box->envelope_sender = sender != NULL ? sender : DEFAULT_ENVELOPE_SENDER;
	raw_box->mtime = mtime;

	mailr->trans = mailbox_transaction_begin(mailr->box, 0);
	headers_ctx = mailbox_header_lookup_init(mailr->box, wanted_headers);
	mailr->mail = mail_alloc(mailr->trans, 0, headers_ctx);
    mailbox_header_lookup_unref(&headers_ctx);
	mail_set_seq(mailr->mail, 1);

	return mailr;
}

struct mail_raw *mail_raw_open_data(string_t *mail_data)
{
	struct mail_raw *mailr;
	struct istream *input;

	input = i_stream_create_from_data(str_data(mail_data), str_len(mail_data));
	
	mailr = mail_raw_create(input, NULL, NULL, (time_t)-1);

	i_stream_unref(&input);

	return mailr;
}
	
struct mail_raw *mail_raw_open_file(const char *path)
{
	struct mail_raw *mailr;
	struct istream *input = NULL;
	time_t mtime;
	const char *sender = NULL;
	
	if ( path == NULL || strcmp(path, "-") == 0 ) {
		path = NULL;
		input = create_raw_stream(0, &mtime, &sender);
	}

	mailr = mail_raw_create(input, path, sender, mtime);

	if ( input != NULL )
		i_stream_unref(&input);

	return mailr;
}

void mail_raw_close(struct mail_raw *mailr) 
{
	mail_free(&mailr->mail);
	mailbox_transaction_rollback(&mailr->trans);
	mailbox_close(&mailr->box);

	pool_unref(&mailr->pool);
}

