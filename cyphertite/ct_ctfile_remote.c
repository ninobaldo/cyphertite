/*
 * Copyright (c) 2011 Conformal Systems LLC <info@conformal.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <inttypes.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <libgen.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <regex.h>
#include <errno.h>

#include <assl.h>
#include <clog.h>
#include <exude.h>
#include <xmlsd.h>

#include <ctutil.h>

#include "ct.h"

void	ctfile_find_for_extract(struct ct_op *);
void	ctfile_find_for_extract_complete(struct ct_op *);
void	ctfile_extract_nextop(struct ct_op *);
void	ctfile_download_next(struct ct_op *);
void	ctfile_nextop_extract_cleanup(struct ct_op *);
void	ctfile_nextop_archive_cleanup(struct ct_op *);
int	ct_file_on_server(char *filename);

void
ctfile_mode_setup(const char *mode)
{
	CNDBG(CT_LOG_CTFILE, "%s", mode ? mode : "");
	if (mode == NULL)
		return;

	if (strcmp(mode, "remote") == 0)
		ctfile_mode = CT_MDMODE_REMOTE;
	else if (strcmp(mode, "local") == 0)
		ctfile_mode = CT_MDMODE_LOCAL;
	else
		CFATALX("invalid ctfile mode specified");
}

char *
ctfile_cook_name(const char *path)
{
	char	*bname;

	bname = basename((char *)path);
	if (bname == NULL)
		CFATAL("can't basename metadata path");
	if (bname[0] == '/')
		CFATALX("invalid metadata filename");

	return (e_strdup(bname));
}

/*
 * Return boolean whether the ctfile in question is already in the cache.
 */
int
ctfile_in_cache(const char *ctfile)
{
	struct dirent	*dp;
	DIR		*dirp;
	int		 found = 0;

	if ((dirp = opendir(ctfile_cachedir)) == NULL)
		CFATALX("can't open metadata cache dir");
	while ((dp = readdir(dirp)) != NULL) {
		if (strcmp(dp->d_name, ctfile) == 0) {
			CNDBG(CT_LOG_CTFILE, "found in cachedir");
			found = 1;
			break;
		}
	}
	closedir(dirp);

	return (found);
}

/*
 * return the filename in the cache directory that a ctfile would have
 * if it extisted.
 */
char *
ctfile_get_cachename(const char *ctfile)
{
	char	*cachename;

	/* cachedir was made sure to terminate with / earlier */
	e_asprintf(&cachename, "%s%s", ctfile_cachedir, ctfile);
	return cachename;
}

/*
 * returns boolean 1/0 whether or not the ctfile in question is the full tag
 * with date/time or not.
 */
int
ctfile_is_fullname(const char *ctfile)
{
	char			*pattern = "^[[:digit:]]{8}-[[:digit:]]{6}-";
	char			error[1024];
	regex_t			re;
	int			match = 0, rv;
	if ((rv = regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB)) != 0) {
		regerror(rv, &re, error, sizeof(error) - 1);
		CFATALX("%s: regcomp failed: %s", __func__, error);
	}
	if (regexec(&re, ctfile, 0, NULL, 0) == 0)
		match = 1;

	regfree(&re);
	return match;
}

struct ct_ctfile_find_args {
	char			*ccfa_tag;
	ctfile_find_callback	*ccfa_nextop;
	void			*ccfa_nextop_args;
	int			 ccfa_download_chain;
	int			 ccfa_empty_ok;
};

struct ct_ctfile_find_fileop_args {
	struct ct_ctfileop_args	 ccffa_base;
	ctfile_find_callback	*ccffa_nextop;
	void			*ccffa_nextop_args;
	int			 ccffa_download_chain;
};

void
ctfile_find_for_operation(char *tag, ctfile_find_callback *nextop,
    void *nextop_args, int download_chain, int empty_ok)
{
	struct ct_ctfile_find_args  *ccfa;
	ccfa = e_calloc(1, sizeof(*ccfa));
	ccfa->ccfa_tag = tag;
	ccfa->ccfa_nextop = nextop;
	ccfa->ccfa_nextop_args = nextop_args;
	ccfa->ccfa_download_chain = download_chain;
	ccfa->ccfa_empty_ok = empty_ok;

	ct_add_operation(ctfile_find_for_extract,
	    ctfile_find_for_extract_complete, ccfa);
}
/*
 * filenames passed in remote mode are opaque tags for the backup.
 * they are stored on the server and in remote mode in the form
 * YYYYMMDD-HHMMSS-<strnvis(mname)>
 */
void
ctfile_find_for_extract(struct ct_op *op)
{
	struct ct_ctfile_find_args	*ccfa = op->op_args;
	const char			*ctfile = ccfa->ccfa_tag;
	struct ct_op			*list_fakeop;
	struct ct_ctfile_list_args	*ccla;

	/* cook the ctfile so we only search for the actual tag */
	ctfile = ctfile_cook_name(ctfile);

	list_fakeop = e_calloc(1, sizeof(*list_fakeop));
	ccla = e_calloc(1, sizeof(*ccla));
	list_fakeop->op_args = ccla;
	ccla->ccla_search = e_calloc(2, sizeof(char **));
	if (ctfile_is_fullname(ctfile)) {
		/* use list as stat() for now */
		*ccla->ccla_search = e_strdup(ctfile);
		ccla->ccla_matchmode = CT_MATCH_GLOB;
	} else {
		e_asprintf(ccla->ccla_search,
		    "^[[:digit:]]{8}-[[:digit:]]{6}-%s$", ctfile);

		ccla->ccla_matchmode = CT_MATCH_REGEX;
		/*
		 * get the list of files matching this tag from the server.
		 * list returns an empty list if it found
		 * nothing and NULL upon failure.
		 */
	}
	e_free(&ctfile);

	CNDBG(CT_LOG_CTFILE, "looking for %s", ccla->ccla_search[0]);

	op->op_priv = list_fakeop;
	ctfile_list_start(list_fakeop);
}

/*
 * List has completed.
 *
 * Select the best filename for download, and download it if missing.
 */
void
ctfile_find_for_extract_complete(struct ct_op *op)
{
	struct ct_ctfile_find_args		*ccfa = op->op_args;
	struct ct_ctfile_find_fileop_args	*ccffa;
	struct ct_op				*list_fakeop = op->op_priv;
	struct ct_ctfile_list_args		*ccla = list_fakeop->op_args;
	struct ctfile_list_tree			 result;
	struct ctfile_list_file			*tmp;
	char	 				*best = NULL;

	RB_INIT(&result);
	ctfile_list_complete(ccla->ccla_matchmode, ccla->ccla_search,
	    ccla->ccla_exclude, &result);
	e_free(ccla->ccla_search);
	e_free(&ccla->ccla_search);
	e_free(&ccla);
	e_free(&list_fakeop);

	/*
	 * Prepare arguments for next operation.
	 * either we'll download the next file, or skip straight to
	 * the callback for after the download, either way we need the nextop
	 */
	ccffa = e_calloc(1, sizeof(*ccffa));
	ccffa->ccffa_nextop = ccfa->ccfa_nextop;
	ccffa->ccffa_nextop_args = ccfa->ccfa_nextop_args;
	ccffa->ccffa_download_chain = ccfa->ccfa_download_chain;

	/* grab the newest one */
	if ((tmp = RB_MAX(ctfile_list_tree, &result)) == NULL) {
		if (ccfa->ccfa_empty_ok) 
			goto do_operation;
		else 
			CFATALX("unable to find metadata tagged %s",
			    ccfa->ccfa_tag);
	}

	/* pick the newest one */
	best = e_strdup(tmp->mlf_name);
	CNDBG(CT_LOG_CTFILE, "backup file is %s", best);

	while((tmp = RB_ROOT(&result)) != NULL) {
		RB_REMOVE(ctfile_list_tree, &result, tmp);
		e_free(&tmp);
	}

	/*
	 * if the metadata file is not in the cache directory then we
	 * need to download it first. if we need to recursively download
	 * a differential chain then that code will handle scheduling
	 * those operations too. If we have it, we still need to check
	 * that all others in the chain exist, however.
	 */
	if (!ctfile_in_cache(best)) {
		ccffa->ccffa_base.cca_localname = best;
		ccffa->ccffa_base.cca_tdir = ctfile_cachedir;
		ccffa->ccffa_base.cca_remotename = e_strdup(best);
		ct_add_operation(ctfile_extract, ctfile_extract_nextop,
		    ccffa);
	} else {
do_operation:
		/*
		 * No download needed, fake the next operation callback
		 * to see if we need anymore.
		 */
		ccffa->ccffa_base.cca_localname = best;
		ccffa->ccffa_base.cca_tdir = ctfile_cachedir;
		op->op_args = ccffa;
		ctfile_extract_nextop(op);
	}
	e_free(&ccfa);
}

/*
 * now the operation has completed we can kick off the next operation knowing
 * that everything has been set up for it.
 */
void
ctfile_extract_nextop(struct ct_op *op)
{
	struct ct_ctfile_find_fileop_args	*ccffa = op->op_args;
	struct ct_ctfileop_args			*cca;
	char					*cachename;

	/*
	 * If this is an operation that needs the full differential chain
	 * recursively fetch the next one in the chain till done.
	 */
	if (ccffa->ccffa_download_chain) {
		/*
		 * download_next takes ownership of the pointers it is given,
		 * duplicate our copy.
		 */
		cca = e_calloc(1, sizeof(*cca));
		if (ccffa->ccffa_base.cca_localname)
			cca->cca_localname =
			    e_strdup(ccffa->ccffa_base.cca_localname);
		if (ccffa->ccffa_base.cca_remotename)
			cca->cca_remotename =
			    e_strdup(ccffa->ccffa_base.cca_remotename);
		cca->cca_tdir = ccffa->ccffa_base.cca_tdir;
		op->op_args = cca;
		ctfile_download_next(op);
	}

	/*
	 * We now have the name of the file we wish to perform the main
	 * operation on, the nextop callback will add this operation
	 * to the operation list. Ownership of the allocated pointer
	 * passes to the child.
	 */
	if (ccffa->ccffa_base.cca_localname != NULL) {
		cachename =
		    ctfile_get_cachename(ccffa->ccffa_base.cca_localname);
	} else {
		cachename = NULL;
	}
	ccffa->ccffa_nextop(cachename, ccffa->ccffa_nextop_args);
	if (ccffa->ccffa_base.cca_localname)
		e_free(&ccffa->ccffa_base.cca_localname);
	if (ccffa->ccffa_base.cca_remotename)
		e_free(&ccffa->ccffa_base.cca_remotename);
	e_free(&ccffa);
}

/*
 * Download all dependant ctfiles of the current ctfile.
 * (called repeatedly until all are fetched).
 */
void
ctfile_download_next(struct ct_op *op)
{
	struct ct_ctfileop_args	*cca = op->op_args, *nextcca; 
	const char		*ctfile = cca->cca_localname;
	const char		*rfile = cca->cca_remotename;
	char			*prevfile;
	char			*cookedname;
	char			*cachename;

again:
	CNDBG(CT_LOG_CTFILE, "ctfile %s", ctfile);

	cachename = ctfile_get_cachename(ctfile);
	/* this will provide us the path that we need to use */
	prevfile = ctfile_get_previous(cachename);
	e_free(&cachename);
	if (prevfile == NULL)
		goto out;

	if (prevfile[0] != '\0') {
		cookedname = ctfile_cook_name(prevfile);
		CNDBG(CT_LOG_CTFILE, "prev file %s cookedname %s", prevfile,
		    cookedname);
		if (!ctfile_in_cache(cookedname)) {
			nextcca = e_calloc(1, sizeof(*nextcca));
			nextcca->cca_localname = cookedname;
			nextcca->cca_remotename = e_strdup(cookedname);
			nextcca->cca_tdir = ctfile_cachedir;
			ct_add_operation_after(op, ctfile_extract,
			    ctfile_download_next, nextcca);
		} else {
			CWARNX("don't need to get %s", cookedname);
			if (ctfile)
				e_free(&ctfile);
			if (rfile)
				e_free(&rfile);
			e_free(&cookedname);
			ctfile = prevfile;
			goto again;
		}
	} else
		e_free(&prevfile);

out:
	if (ctfile)
		e_free(&ctfile);
	if (rfile)
		e_free(&rfile);
	e_free(&cca);
}

void
ctfile_nextop_extract(char *ctfile, void *args)
{
	struct ct_extract_args	*cea = args;

	cea->cea_local_ctfile = ctfile;
	ct_add_operation(ct_extract, ctfile_nextop_extract_cleanup, cea);
}

void
ctfile_nextop_list(char *ctfile, void *args)
{
	struct ct_extract_args	*cea = args;

	cea->cea_local_ctfile = ctfile;
	ct_add_operation(ct_list_op, ctfile_nextop_extract_cleanup, cea);
}

void
ctfile_nextop_extract_cleanup(struct ct_op *op)
{
	struct ct_extract_args	*cea = op->op_args;

	if (cea->cea_local_ctfile)
		e_free(&cea->cea_local_ctfile);
}

void
ctfile_nextop_archive(char *basis, void *args)
{
	struct ct_archive_args	*caa = args;
	struct ct_ctfileop_args	*cca;
	char			*ctfile;
	char	 		 buf[TIMEDATA_LEN], *fullname, *cachename;
	time_t	 		 now;

	CNDBG(CT_LOG_CTFILE, "setting basisname %s", basis);
	caa->caa_basis = basis;

	/*
	 * We now have the basis found for us, cook and prepare the tag
	 * we wish to create then add the operation.
	 */
	ctfile = ctfile_cook_name(caa->caa_tag);

	if (ctfile_is_fullname(ctfile) != 0)
		CFATALX("metadata name with date tag already filled in");

	now = time(NULL);
	if (strftime(buf, TIMEDATA_LEN, "%Y%m%d-%H%M%S",
	    localtime(&now)) == 0)
		CFATALX("can't format time");
	e_asprintf(&fullname, "%s-%s", buf, ctfile);
	CNDBG(CT_LOG_CTFILE, "backup file is %s", fullname);

	/* check it isn't already in the cache */
	cachename = ctfile_get_cachename(fullname);
	if (ctfile_in_cache(fullname))
		CFATALX("generated metadata name %s already in cache dir",
		    fullname);

	e_free(&ctfile);
	e_free(&fullname);

	caa->caa_local_ctfile = cachename;
	ct_add_operation(ct_archive, NULL, caa);
	/*
	 * set up an additional operation to upload the newly created
	 * ctfile after the archive is completed.
	 */
	cca = e_calloc(1, sizeof(*cca));
	cca->cca_localname = cachename;
	cca->cca_encrypted = caa->caa_encrypted;
	ct_add_operation(ctfile_archive, ctfile_nextop_archive_cleanup, cca);
}

void
ctfile_nextop_archive_cleanup(struct ct_op *op)
{
	struct ct_ctfileop_args	*cca = op->op_args;

	if (cca->cca_localname)
		e_free(&cca->cca_localname);
	if (cca->cca_remotename)
		e_free(&cca->cca_remotename);
	e_free(&cca);
}

void
ctfile_nextop_justdl(char *ctfile, void *args)
{
	char		**filename = args;

	*filename = ctfile;

	/* done, jump out of the loop */
	ct_add_operation(ct_shutdown_op, NULL, NULL);
}

void	ct_compare_secrets(struct ct_op *);
void
ct_check_secrets_extract(struct ct_op *op)
{
	struct ct_ctfileop_args	*cca;

	if (!ct_file_on_server("crypto.secrets"))
		CFATALX("upload_crypto_secrets set but not secrets file on"
		    "server, please use cyphertitectl secrets_upload");

	cca = e_calloc(1, sizeof(*cca));
	/* XXX temporary name? */
	cca->cca_localname = "cyphertite-server.secrets";
	cca->cca_remotename = "crypto.secrets";
	cca->cca_tdir = ctfile_cachedir;
	cca->cca_encrypted = 0; /* ignored */
	ct_add_operation_after(op, ctfile_extract, ct_compare_secrets, cca);
	/* start download of secrets with finish file being the comparison */

}

void
ct_compare_secrets(struct ct_op *op)
{
	struct ct_ctfileop_args		*cca = op->op_args;
	FILE				*f, *tf;
	char		 		 temp_path[PATH_MAX];
	struct stat	 		 sb, tsb;
	char				 buf[1024], tbuf[1024];
	size_t				 sz, rsz;

	/* cachedir is '/' terminated */
	strlcpy(temp_path, ctfile_cachedir, sizeof(temp_path));
	strlcat(temp_path, "cyphertite-server.secrets", sizeof(temp_path));
	if (stat(ct_crypto_secrets, &sb) != 0)
		CFATAL("can't stat secrets file at \"%s\"", ct_crypto_secrets);
	if (stat(temp_path, &tsb) != 0) 
		CFATAL("can't stat temporary secrets file");

	/* Compare size first */
	if (tsb.st_size != sb.st_size)
		CFATALX("size doesn't match for server secrets file "
		    "(%" PRId64 " vs %" PRId64 "), please confirm that local "
		    "secrets file is the correct one", tsb.st_size, sb.st_size);

	if ((f = fopen(ct_crypto_secrets, "r")) == NULL)
		CFATAL("can't open secrets file");
	if ((tf = fopen(temp_path, "r")) == NULL)
		CFATAL("can't open temporary secrets file");
	/* read then throw away */
	unlink(temp_path);
	while (sb.st_size > 0) {
		sz = sb.st_size;
		if (sz > 1024)
			sz = 1024;
		sb.st_size -= sz;
		CNDBG(CT_LOG_FILE, "sz = %zu remaining = %" PRId64,
		    sz, sb.st_size);
		if ((rsz = fread(buf, 1, sz, f)) != sz)
			CFATALX("short read on secrets file (%zu %zu)",
			    sz, rsz);
		if ((rsz = fread(tbuf, 1, sz, tf)) != sz)
			CFATALX("short read on temporary secrets file "
			    "(%zu %zu)", sz, rsz);

		if (memcmp(buf, tbuf, sz) != 0)
			CFATALX("secrets file on server differs from local"
			    " please check which is correct and rectify");
	}
	fclose(f);
	fclose(tf);
	e_free(&cca);
}

void
ct_check_secrets_upload(struct ct_op *op)
{
	struct ct_ctfileop_args	*cca = op->op_args;
	char			 answer[1024];


	/* Check to see if we already have a secrets file on the server */
	if (ct_file_on_server(cca->cca_remotename)) {
		if (ct_get_answer("There is already a crypto secrets file on "
		    "the server, would you like to replace it? [no]: ",
		    "yes", "no", "no", answer, sizeof answer, 0) != 1)
			CFATALX("not uploading secrets file");
		op = ct_add_operation_after(op, ctfile_delete, NULL,
		    cca->cca_remotename);
	}

	ct_add_operation_after(op, ctfile_archive, NULL, cca);

}

/*
 * return boolean whether or not the last ctfile_list contained
 * filename.
 */
int
ct_file_on_server(char *filename)
{
	struct ctfile_list_tree	 results;
	struct ctfile_list_file	*file = NULL;
	char			*filelist[2];
	int			 exists = 0;

	RB_INIT(&results);
	filelist[0] = filename;
	filelist[1] = NULL;
	ctfile_list_complete(CT_MATCH_GLOB, filelist, NULL, &results);

	/* Check to see if we already have a secrets file on the server */
	if (RB_MIN(ctfile_list_tree, &results) != NULL) {
		exists = 1;
	}
	while ((file = RB_ROOT(&results)) != NULL) {
		RB_REMOVE(ctfile_list_tree, &results, file);
		e_free(&file);
	}

	return (exists);
}

void
ct_secrets_exists(struct ct_op *op)
{
	int *exists = op->op_args;

	*exists = ct_file_on_server("crypto.secrets");
}

int
ct_have_remote_secrets_file(void)
{
	int have;

	ct_do_operation(ctfile_list_start, ct_secrets_exists, &have, 0, 1);

	return (have);
}
void
ct_download_secrets_file(void)
{
	struct ct_ctfileop_args	 cca;

	CWARNX("Downloading secrets file from server...");

	cca.cca_localname = ct_crypto_secrets;
	cca.cca_remotename = "crypto.secrets";
	cca.cca_tdir = NULL;
	cca.cca_encrypted = 0;

	ct_do_operation(ctfile_extract, NULL, &cca, 0, 1);
}

void
ct_upload_secrets_file(void)
{
	struct ct_ctfileop_args	 cca;

	CWARNX("Uploading secrets file to server...");

	cca.cca_localname = ct_crypto_secrets;
	cca.cca_remotename = "crypto.secrets";
	cca.cca_tdir = NULL;
	cca.cca_encrypted = 0;

	ct_do_operation(ctfile_list_start, ct_check_secrets_upload, &cca, 0, 1);
}
